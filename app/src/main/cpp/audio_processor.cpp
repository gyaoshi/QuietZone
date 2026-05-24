/**
 * 音频处理器 (v3 — 标量 FxLMS 修正版)
 *
 * 三种ANC模式完整实现，针对手机单麦克风场景优化
 *
 * v3 关键修正:
 *   1. FxLMS 更新使用标量 x̂(n) = Ŝ * x(n)
 *      权重更新: w = leaky·w + μ·x̂(n)·e(n)·x_buf
 *   2. 逐样本 process+update 交错:
 *      process(x_n) → compute e_n → filterSample(x_n) → update(x_hat_n, e_n)
 *      保证 x_buffer_ 在 update 时与当前样本对齐
 *   3. 反馈 ANC 使用独立的输出次级路径滤波:
 *      Ŝ·y(n-1) 估计反噪声贡献，避免 NaN
 *   4. Hybrid ANC 解耦前馈/反馈:
 *      前馈先用麦克风输入，反馈用残差估计
 *
 * 算法对照 (旧版 bug vs 新版修正):
 *   旧: update(&filtered_ref_[i], err_buffer_[i], 1)
 *       → 只更新 weights_[0]，且 filtered_ref 是向量用法
 *   新: update(x_hat_n, e_n)
 *       → 更新全部权重，x_hat_n 是标量
 *
 * 三种模式:
 *   Feedforward: x(n)=mic, e(n)=mic+anti_noise, 适合宽带噪声
 *   Feedback:    e(n)=mic, d̂(n)=e(n)-Ŝ·y(n), 适合窄带周期噪声
 *   Hybrid:      FF+FB叠加, 宽窄带兼顾 (推荐)
 */

#include "anc_engine.h"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace anc {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// AudioProcessor
// ============================================================

AudioProcessor::AudioProcessor()
    : enabled_(false)
    , calibrating_(false)
    , sec_output_idx_(0)
    , prev_output_(0.0f)
    , ref_power_accum_(0.0f)
    , err_power_accum_(0.0f)
    , stats_counter_(0)
{
}

AudioProcessor::~AudioProcessor() = default;

bool AudioProcessor::init(const ANCConfig& config) {
    config_ = config;

    // 前馈滤波器
    forward_filter_ = std::make_unique<FxLMSFilter>(
        config.filterLength, config.stepSize, config.leakyFactor);

    // 反馈滤波器 (步长减半, 更保守)
    feedback_filter_ = std::make_unique<FxLMSFilter>(
        config.filterLength, config.stepSize * 0.5f, config.leakyFactor);

    // 次级路径估计器
    sec_path_ = std::make_unique<SecondaryPathEstimator>(
        config.secondaryPathLength, config.blockSize);

    // 频谱分析器
    ref_analyzer_ = std::make_unique<SpectrumAnalyzer>(config.spectrumBins);
    err_analyzer_ = std::make_unique<SpectrumAnalyzer>(config.spectrumBins);

    // 预分配所有缓冲区
    int block = config.blockSize;

    ref_buffer_.resize(block, 0.0f);
    err_buffer_.resize(block, 0.0f);
    output_buffer_.resize(block, 0.0f);
    auxiliary_noise_.resize(block, 0.0f);

    // 输出次级路径滤波缓冲 (与 sec_path_ 独立)
    sec_output_buf_.resize(config.secondaryPathLength, 0.0f);
    sec_output_idx_ = 0;

    // 反馈分支独立次级路径缓冲 (用于 Hybrid: x̂_fb = Ŝ·d̂(n))
    sec_fb_buf_.resize(config.secondaryPathLength, 0.0f);
    sec_fb_buf_idx_ = 0;

    prev_output_ = 0.0f;

    stats_ = ANCStats{};
    ref_power_accum_ = 0.0f;
    err_power_accum_ = 0.0f;
    stats_counter_ = 0;

    return true;
}

void AudioProcessor::processFrame(const float* mic_input, float* speaker_output, int numSamples) {
    if (!enabled_.load()) {
        memset(speaker_output, 0, numSamples * sizeof(float));
        return;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    switch (config_.mode) {
        case 0:  processFeedforward(mic_input, speaker_output, numSamples); break;
        case 1:  processFeedback(mic_input, speaker_output, numSamples);    break;
        case 2:  processHybrid(mic_input, speaker_output, numSamples);      break;
        default: processHybrid(mic_input, speaker_output, numSamples);      break;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    float us = std::chrono::duration<float, std::micro>(t1 - t0).count();
    stats_.processingTimeUs = 0.9f * stats_.processingTimeUs + 0.1f * us; // EMA
    stats_.frameCount++;
}

// ============================================================
// 输出次级路径滤波辅助方法
// ============================================================
float AudioProcessor::filterOutputSecondaryPath(float y_n) {
    /**
     * 对输出信号做次级路径 FIR 滤波: Ŝ * y(n)
     *
     * 用途: 反馈 ANC 中估计原始噪声
     *   d̂(n) = e(n) - Ŝ * y(n)
     *
     * 使用独立的环形缓冲，不与 sec_path_->filterSample() 冲突
     * (sec_path_ 的 path_buffer_ 用于滤波参考信号，这里用于滤波输出信号)
     */
    const float* coeffs = sec_path_->getPathCoeffs();
    int len = sec_path_->getPathLength();

    sec_output_buf_[sec_output_idx_] = y_n;

    float sum = 0.0f;
    for (int i = 0; i < len; i++) {
        int idx = (sec_output_idx_ - i + len) % len;
        sum += coeffs[i] * sec_output_buf_[idx];
    }

    sec_output_idx_ = (sec_output_idx_ + 1) % len;
    return sum;
}

// ============================================================
// Feedforward ANC (宽带前馈)
// ============================================================
void AudioProcessor::processFeedforward(const float* mic, float* out, int n) {
    /**
     * Feedforward ANC — 标量 FxLMS 正确实现
     *
     * 手机单麦克风场景:
     *   - 参考信号 x(n) = 麦克风输入
     *   - 误差信号 e(n) = x(n) + anti_noise (简化估计)
     *   - 在真实系统中: e(n) = d(n) + S₂·y(n)，由误差麦克风测量
     *     手机只有一个麦克风，用简化近似
     *
     * 逐样本处理流程:
     *   1. y(n) = W·x(n)           [自适应滤波器输出]
     *   2. anti_noise = -y(n)      [反噪声]
     *   3. e(n) = x(n) + anti      [误差估计]
     *   4. x̂(n) = Ŝ·x(n)          [标量滤波参考]
     *   5. W ← leaky·W + μ·x̂(n)·e(n)·x_buf  [权重更新]
     */

    float ref_power = 0.0f;
    float err_power = 0.0f;

    for (int i = 0; i < n; i++) {
        float x_n = mic[i];  // 参考信号 = 麦克风输入

        // 1. 自适应滤波器输出
        float y_n = forward_filter_->process(x_n);

        // 2. 反噪声 (取反 + 增益)
        float anti_noise = -y_n * config_.outputGain;

        // 3. 输出限幅 (防削波, 保护扬声器)
        anti_noise = std::clamp(anti_noise, -0.95f, 0.95f);
        out[i] = anti_noise;

        // 4. 误差估计:
        //    真实: e(n) = d(n) + S₂·y(n)
        //    近似: e(n) ≈ x(n) + anti_noise (单麦克风简化)
        float e_n = x_n + anti_noise;

        // 5. 标量滤波参考: x̂(n) = Ŝ * x(n)
        float x_hat_n = sec_path_->filterSample(x_n);

        // 6. FxLMS 权重更新 (标量 x̂(n) · 完整向量更新)
        forward_filter_->update(x_hat_n, e_n);

        // 统计与频谱
        ref_buffer_[i] = x_n;
        err_buffer_[i] = e_n;
        ref_power += x_n * x_n;
        err_power += e_n * e_n;
    }

    updateStats(ref_power / n, err_power / n);

    if (stats_.frameCount % 8 == 0) {
        ref_analyzer_->analyze(ref_buffer_.data(), n);
        err_analyzer_->analyze(err_buffer_.data(), n);
    }
}

// ============================================================
// Feedback ANC (窄带反馈)
// ============================================================
void AudioProcessor::processFeedback(const float* mic, float* out, int n) {
    /**
     * Feedback ANC — 标量 FxLMS 正确实现
     *
     * 不需要参考麦克风，从误差信号估计原始噪声:
     *   d̂(n) = e(n) - Ŝ * y(n)
     *
     * 因果性处理:
     *   - 使用前一帧输出 y(n-1) 通过次级路径估计 Ŝ·y(n-1)
     *   - 避免循环依赖: y(n) 依赖 d̂(n)，d̂(n) 依赖 Ŝ·y(n)
     *   - 实际系统中次级路径本身有延迟，y(n-1) 是合理近似
     *
     * 逐样本处理流程:
     *   1. e(n) = mic(n)                    [误差=麦克风残差]
     *   2. s_hat_y = Ŝ·y_prev               [次级路径输出估计]
     *   3. d̂(n) = e(n) - s_hat_y            [原始噪声估计]
     *   4. y(n) = W_fb·d̂(n)                 [反馈滤波器输出]
     *   5. anti_noise = -y(n)               [反噪声]
     *   6. x̂(n) = Ŝ·d̂(n)                   [标量滤波参考]
     *   7. W_fb ← leaky·W_fb + μ·x̂(n)·e(n)·d̂_buf  [权重更新]
     *
     * 适合: 周期性噪声 (引擎、风扇、压缩机)
     * 有效频段: 50-500Hz
     */

    float ref_power = 0.0f;
    float err_power = 0.0f;

    for (int i = 0; i < n; i++) {
        float e_n = mic[i]; // 误差信号 = 麦克风输入 (残差)

        // 1. 估计次级路径对前一帧输出的贡献: Ŝ * y(n-1)
        //    使用独立的输出次级路径滤波缓冲
        float s_hat_y = filterOutputSecondaryPath(prev_output_);

        // 2. 估计原始噪声: d̂(n) = e(n) - Ŝ·y(n-1)
        float d_hat = e_n - s_hat_y;

        // 3. 限幅噪声估计 (防止异常值导致发散)
        d_hat = std::clamp(d_hat, -1.0f, 1.0f);

        // 4. 反馈自适应滤波器输出
        float y_n = feedback_filter_->process(d_hat);

        // 5. 反噪声
        float anti_noise = -y_n * config_.outputGain;
        anti_noise = std::clamp(anti_noise, -0.95f, 0.95f);
        out[i] = anti_noise;

        // 保存当前输出供下一帧使用
        prev_output_ = anti_noise;

        // 6. 标量滤波参考: x̂(n) = Ŝ * d̂(n)
        float x_hat_n = sec_path_->filterSample(d_hat);

        // 7. FxLMS 权重更新 (标量 x̂(n) · 完整向量更新)
        feedback_filter_->update(x_hat_n, e_n);

        // 统计与频谱
        ref_buffer_[i] = d_hat;
        err_buffer_[i] = e_n;
        ref_power += d_hat * d_hat;
        err_power += e_n * e_n;
    }

    updateStats(ref_power / n, err_power / n);

    if (stats_.frameCount % 8 == 0) {
        ref_analyzer_->analyze(ref_buffer_.data(), n);
        err_analyzer_->analyze(err_buffer_.data(), n);
    }
}

// ============================================================
// Hybrid ANC (混合模式 — 推荐)
// ============================================================
void AudioProcessor::processHybrid(const float* mic, float* out, int n) {
    /**
     * Hybrid ANC — 标量 FxLMS 正确实现 (前馈 + 反馈)
     *
     * 结构:
     *   x(n) ──→ [W_ff] ──→ y_ff(n) ──┐
     *                                    ├──→ y(n) = y_ff + y_fb ──→ 扬声器
     *   d̂(n) ──→ [W_fb] ──→ y_fb(n) ──┘
     *
     * 前馈分支: 处理宽带成分 (利用参考信号预测)
     *   x(n) = mic(n)
     *   e(n) = x(n) + anti_noise
     *   x̂_ff(n) = Ŝ * x(n)
     *   W_ff ← leaky·W_ff + μ_ff·x̂_ff(n)·e(n)·x_buf
     *
     * 反馈分支: 处理窄带残余 (弥补前馈不足)
     *   d̂(n) = e(n) - Ŝ·y_ff(n)  (只减去前馈贡献，避免循环依赖)
     *   x̂_fb(n) = Ŝ * d̂(n)
     *   W_fb ← leaky·W_fb + μ_fb·x̂_fb(n)·e(n)·d̂_buf
     *
     * 解耦设计:
     *   反馈分支的参考估计 d̂(n) 只使用 Ŝ·y_ff(n)
     *   而非 Ŝ·y(n) = Ŝ·(y_ff + y_fb)，避免 y_fb 依赖自身的循环
     */

    float ref_power = 0.0f;
    float err_power = 0.0f;

    for (int i = 0; i < n; i++) {
        float x_n = mic[i];  // 参考信号

        // ── 前馈分支 ──
        float y_ff = forward_filter_->process(x_n);

        // ── 反馈分支: 估计窄带残余噪声 ──
        // 使用前馈输出通过次级路径的估计
        float s_hat_y_ff = filterOutputSecondaryPath(-y_ff * config_.outputGain);
        float e_n_approx = x_n + (-y_ff * config_.outputGain);  // 前馈残差
        e_n_approx = std::clamp(e_n_approx, -1.0f, 1.0f);

        // 原始噪声估计 (只减去前馈贡献)
        float d_hat = e_n_approx - s_hat_y_ff;
        d_hat = std::clamp(d_hat, -1.0f, 1.0f);

        // 反馈滤波器输出
        float y_fb = feedback_filter_->process(d_hat);

        // ── 混合输出 ──
        float y_n = y_ff + y_fb;
        float anti_noise = -y_n * config_.outputGain;
        anti_noise = std::clamp(anti_noise, -0.95f, 0.95f);
        out[i] = anti_noise;

        // ── 误差估计 (用于两路权重更新) ──
        float e_n = x_n + anti_noise;

        // ── 前馈 FxLMS 更新 ──
        float x_hat_ff = sec_path_->filterSample(x_n);
        forward_filter_->update(x_hat_ff, e_n);

        // ── 反馈 FxLMS 更新 ──
        // 使用独立缓冲计算 x̂_fb = Ŝ·d̂(n)，避免与前馈的 filterSample 共享 path_buffer_
        float x_hat_fb = sec_path_->filterSampleWithBuffer(d_hat, sec_fb_buf_, sec_fb_buf_idx_);
        feedback_filter_->update(x_hat_fb, e_n);

        // 统计与频谱
        ref_buffer_[i] = x_n;
        err_buffer_[i] = e_n;
        ref_power += x_n * x_n;
        err_power += e_n * e_n;
    }

    updateStats(ref_power / n, err_power / n);

    if (stats_.frameCount % 8 == 0) {
        ref_analyzer_->analyze(ref_buffer_.data(), n);
        err_analyzer_->analyze(err_buffer_.data(), n);
    }
}

// ============================================================
// 统计与辅助
// ============================================================

void AudioProcessor::updateStats(float ref_power, float err_power) {
    // 指数移动平均 (EMA)
    const float alpha = 0.005f;
    ref_power_accum_ = (1.0f - alpha) * ref_power_accum_ + alpha * ref_power;
    err_power_accum_ = (1.0f - alpha) * err_power_accum_ + alpha * err_power;

    stats_counter_++;
    if (stats_counter_ >= 10) {
        stats_.referencePower = ref_power_accum_;
        stats_.errorPower = err_power_accum_;

        // 降噪量 (dB): 正值 = 降噪
        if (ref_power_accum_ > 1e-10f && err_power_accum_ > 1e-10f) {
            float ratio = ref_power_accum_ / err_power_accum_;
            stats_.noiseReductionDb = 10.0f * log10f(ratio);
        } else {
            stats_.noiseReductionDb = 0.0f;
        }

        stats_.filterNorm = forward_filter_->getWeightNorm();
        stats_.isConverged = stats_.noiseReductionDb > 3.0f;
        stats_counter_ = 0;
    }
}

void AudioProcessor::generateAuxiliaryNoise(float* buffer, int len) {
    float level = 0.01f; // -40dB
    for (int i = 0; i < len; i++) {
        buffer[i] = level * (2.0f * (float)rand() / RAND_MAX - 1.0f);
    }
}

ANCStats AudioProcessor::getStats() const {
    return stats_;
}

void AudioProcessor::getRefSpectrum(float* output, int len) {
    if (ref_analyzer_) ref_analyzer_->getMagnitudeSpectrum(output, len);
}

void AudioProcessor::getErrSpectrum(float* output, int len) {
    if (err_analyzer_) err_analyzer_->getMagnitudeSpectrum(output, len);
}

void AudioProcessor::getReductionSpectrum(float* output, int len) {
    if (ref_analyzer_ && err_analyzer_) {
        ref_analyzer_->getReductionSpectrum(
            ref_buffer_.data(), err_buffer_.data(),
            std::min((int)ref_buffer_.size(), (int)err_buffer_.size()),
            output, len);
    }
}

void AudioProcessor::setStepSize(float mu) {
    config_.stepSize = mu;
    if (forward_filter_) forward_filter_->setStepSize(mu);
    if (feedback_filter_) feedback_filter_->setStepSize(mu * 0.5f);
}

void AudioProcessor::setOutputGain(float gain) {
    config_.outputGain = gain;
}

void AudioProcessor::setMode(int mode) {
    config_.mode = std::clamp(mode, 0, 2);
}

void AudioProcessor::setExternalSpeaker(bool external) {
    config_.externalSpeaker = external;
    config_.outputGain = external ? 2.0f : 1.0f;
}

void AudioProcessor::startCalibration() {
    calibrating_.store(true);
    sec_path_->reset();
}

void AudioProcessor::reset() {
    if (forward_filter_) forward_filter_->reset();
    if (feedback_filter_) feedback_filter_->reset();
    if (sec_path_) sec_path_->reset();

    std::fill(ref_buffer_.begin(), ref_buffer_.end(), 0.0f);
    std::fill(err_buffer_.begin(), err_buffer_.end(), 0.0f);
    std::fill(output_buffer_.begin(), output_buffer_.end(), 0.0f);
    std::fill(sec_output_buf_.begin(), sec_output_buf_.end(), 0.0f);
    std::fill(sec_fb_buf_.begin(), sec_fb_buf_.end(), 0.0f);

    stats_ = ANCStats{};
    ref_power_accum_ = 0.0f;
    err_power_accum_ = 0.0f;
    stats_counter_ = 0;
    sec_output_idx_ = 0;
    sec_fb_buf_idx_ = 0;
    prev_output_ = 0.0f;
}

void AudioProcessor::offlineCalibrate(const float* input, const float* output, int len) {
    // 使用互相关法估计次级路径脉冲响应
    // h[n] = Rxy[n] / Rxx[0]
    // 其中 Rxy 是输入-输出互相关, Rxx 是输入自相关
    int pathLen = config_.secondaryPathLength;
    if (len < pathLen || !sec_path_) return;

    std::vector<float> h(pathLen, 0.0f);

    // 计算自相关 Rxx[0]
    float rxx0 = 0.0f;
    for (int i = 0; i < len; i++) {
        rxx0 += input[i] * input[i];
    }
    if (rxx0 < 1e-12f) return;

    // 计算互相关 Rxy[0..pathLen-1]
    for (int lag = 0; lag < pathLen; lag++) {
        float rxy = 0.0f;
        for (int i = lag; i < len; i++) {
            rxy += input[i - lag] * output[i];
        }
        h[lag] = rxy / rxx0;
    }

    sec_path_->setPathCoeffs(h.data(), pathLen);
}

} // namespace anc
