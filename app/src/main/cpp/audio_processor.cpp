/**
 * 音频处理器 v4 —— 单麦克风可实现的降噪控制逻辑
 *
 * 与 v3 的根本区别:
 *   v3: e = mic + (-y)           零延迟编造的误差信号；x̂ 用标量代替滤波参考向量
 *       ⇒ 梯度期望恒为 0 ⇒ 权重永不增长 ⇒ 输出静音 ⇒ 降噪量恒为 0
 *   v4: e = 麦克风实测值
 *       d̂ = e - Ŝ*y              用校准得到的真实次级路径估计扰动
 *       窄带分支用内部合成参考 + Ŝ 在谐波频率上的复增益做精确 FxLMS
 *       ⇒ 环路增益被归一到单位增益，对回环延迟免疫(延迟=相位，相位可精确补偿)
 *
 * 主要约束 (仿真结论):
 *   归一化步长 s 必须满足  s  ≲ 4π/D  (D = 回环延迟样本数)，
 *   否则延迟积分器失稳。因此步长由实测回环延迟反推，并带运行时失稳回退保护。
 */

#include "anc_engine.h"
#include "spectrum_analyzer.h"

#ifdef __ANDROID__
#include <android/log.h>
#define ANC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ANCProc", __VA_ARGS__)
#else
#define ANC_LOGI(...) do { } while (0)
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace anc {

static constexpr int kToneWindow = 4096;      // 基频检测窗 (48k → 85ms, 分辨率 ~12Hz)

// ============================================================
// 构造 / 初始化
// ============================================================
AudioProcessor::AudioProcessor() = default;
AudioProcessor::~AudioProcessor() = default;

bool AudioProcessor::init(const ANCConfig& config) {
    config_ = config;
    const float fs = static_cast<float>(config.sampleRate);

    // 次级路径: 先给兜底模型，校准成功后再替换
    sec_path_.setDefaultModel(fs, config.defaultLoopDelayMs,
                              config.defaultSpkCutoffHz, config.defaultSpkGain);

    harmonic_.init(fs, config.maxHarmonics, config.controlLowHz,
                   config.controlHighHz, config.tonalStep);
    harmonic_.setSecondaryPath(&sec_path_);

    wide_filter_.init(config.filterLength, config.stepSize, config.leakyFactor);

    tone_detector_.init(fs, kToneWindow, config.controlLowHz,
                        std::min(config.controlHighHz, 900.0f),
                        config.tonalityThresholdDb);

    bp_ref_.setBand(config.controlLowHz, config.widebandHighHz, fs);
    bp_err_.setBand(config.controlLowHz, config.widebandHighHz, fs);

    const int block = config.blockSize;
    // 延迟线按次级路径长度分配 (校准后可能变化, setSecondaryPathIr 会重建)
    line_out_.resize(std::max(64, sec_path_.ringSize()));
    line_ref_.resize(std::max(64, sec_path_.ringSize()));

    dhat_block_.assign(block, 0.0f);
    err_block_.assign(block, 0.0f);
    out_block_.assign(block, 0.0f);

    const int specBins = std::max(256, config.spectrumBins);
    dhat_hist_.assign(specBins, 0.0f);
    err_hist_.assign(specBins, 0.0f);
    ref_spectrum_.assign(specBins / 2, -100.0f);
    err_spectrum_.assign(specBins / 2, -100.0f);
    spec_ = std::make_unique<SpectrumAnalyzer>(specBins);

    hist_pos_ = 0;
    prev_out_ = 0.0f;
    d_pow_ = 0.0f;
    e_pow_ = 0.0f;
    o_pow_ = 0.0f;
    xh_pow_ = 1e-6f;
    out_pow_slow_ = 0.0f;
    stats_ = ANCStats{};
    stats_counter_ = 0;
    detect_elapsed_ms_ = 0;
    guard_counter_ = 0;
    guard_baseline_ = 0.0f;

    startBaseline();   // 每次初始化都重测本底
    updateTonalStep();
    return true;
}

void AudioProcessor::startBaseline() {
    // 控制器起来前先静音输出 0.5s, 用麦克风实测本底功率 P0。
    // 之后 NR = 10log10(P0 / P(e)), 两侧都是实测量, 不依赖次级路径模型。
    nr_base_active_ = true;
    nr_base_ready_ = false;
    nr_base_count_ = 0;
    nr_base_acc_ = 0.0;
    nr_base_pow_ = 0.0f;
}

void AudioProcessor::updateTonalStep() {
    // 仿真标定 (sim/results_step_rule_by_delay.txt): 各延迟下实测最大安全步长为
    //   D=96→0.005, D=144→0.005, D=288→0.007, D=480→0.004, D=720→更小
    // 取 1/D 并夹在 [2e-4, 2e-3] 后, 在 D<=500 时恒为 0.002，实测裕度 2.0~3.5x；
    // D 很大时按 1/D 继续收紧。切勿用 2/D (~3e-3)：D=480 时裕度仅剩 1.3x, 会发散。
    // 另有运行时失稳回退保护 (见 runToneDetection)，可再降一半。
    const float D = static_cast<float>(std::max(48, sec_path_.delaySamples()));
    config_.tonalStep = std::clamp(1.0f / D, 2e-4f, 2e-3f);
    harmonic_.setStep(config_.tonalStep);
}

void AudioProcessor::setSecondaryPathIr(const float* h, int len) {
    if (!h || len < 64) return;
    const bool wasEnabled = isEnabled();
    enabled_.store(false, std::memory_order_release);
    sec_path_.setImpulseResponse(h, len, static_cast<float>(config_.sampleRate));
    harmonic_.setSecondaryPath(&sec_path_);
    line_out_.resize(std::max(64, sec_path_.ringSize()));
    line_ref_.resize(std::max(64, sec_path_.ringSize()));
    wide_filter_.reset();
    harmonic_.reset();
    prev_out_ = 0.0f;
    updateTonalStep();
    stats_.loopDelayMs = sec_path_.delayMs();
    stats_.isCalibrated = sec_path_.isCalibrated();
    guard_reductions_ = 0;
    tonal_disabled_ = false;
    enabled_.store(wasEnabled, std::memory_order_release);
}

// ============================================================
// 校准工具 (在调用线程执行, 不占用音频线程)
// ============================================================
void AudioProcessor::generateProbe(float* out, int len, float fs,
                                   float f0, float f1, float level) {
    const double T = static_cast<double>(len) / fs;
    for (int i = 0; i < len; i++) {
        const double t = static_cast<double>(i) / fs;
        // 对数扫频 (幅度包络两端做淡入淡出，避免瞬态喀哒声)
        const double k = std::log(static_cast<double>(f1) / f0);
        const double phase = 2.0 * M_PI * f0 * T / k * (std::exp(k * t / T) - 1.0);
        double env = 1.0;
        const int fade = std::min(len / 8, static_cast<int>(0.01 * fs));
        if (fade > 1) {
            if (i < fade) env = 0.5 - 0.5 * std::cos(M_PI * i / fade);
            else if (i >= len - fade) env = 0.5 - 0.5 * std::cos(M_PI * (len - 1 - i) / fade);
        }
        out[i] = static_cast<float>(level * env * std::sin(phase));
    }
}

int AudioProcessor::estimateImpulseResponse(const float* recorded, const float* probe,
                                            int len, int maxTaps, float* out) {
    if (!recorded || !probe || !out || len < maxTaps * 2) return 0;

    // 探测信号能量
    double rpp = 0.0;
    for (int i = 0; i < len; i++) rpp += static_cast<double>(probe[i]) * probe[i];
    if (rpp < 1e-12) return 0;

    // 互相关 (匹配滤波): h[lag] = <rec(n), probe(n-lag)> / <probe, probe>
    // 只需要 lag = 0..maxTaps-1
    const int taps = std::min(maxTaps, len / 2);
    const float inv = static_cast<float>(1.0 / rpp);
    for (int lag = 0; lag < taps; lag++) {
        double acc = 0.0;
        for (int i = lag; i < len; i++) {
            acc += static_cast<double>(recorded[i]) * probe[i - lag];
        }
        out[lag] = static_cast<float>(acc * inv);
    }
    return taps;
}

// ============================================================
// 主处理入口
// ============================================================
void AudioProcessor::processFrame(const float* mic_input, float* speaker_output, int numSamples) {
    if (!enabled_.load(std::memory_order_acquire)) {
        std::memset(speaker_output, 0, numSamples * sizeof(float));
        return;
    }

    // ---- 实测基线: 最初 0.5s 输出静音, 只采麦克风本底功率 P0 ----
    // 注意这里刻意不跑控制器: 只有"没有反噪声"时的麦克风读数, 才能当分母基准。
    if (nr_base_active_) {
        double acc = 0.0;
        for (int i = 0; i < numSamples; i++) {
            acc += static_cast<double>(mic_input[i]) * mic_input[i];
        }
        nr_base_acc_ += acc;
        nr_base_count_ += numSamples;
        std::memset(speaker_output, 0, numSamples * sizeof(float));
        pushHistory(mic_input, numSamples);
        const float p_est = static_cast<float>(nr_base_acc_ /
                            static_cast<double>(nr_base_count_));
        stats_.referencePower = p_est;
        stats_.errorPower = p_est;
        stats_.noiseReductionDb = 0.0f;   // 基线阶段无所谓"降了多少"
        if (nr_base_count_ >= config_.sampleRate / 2) {   // 0.5 s
            nr_base_pow_ = p_est;
            nr_base_active_ = false;
            nr_base_ready_ = true;
        }
        return;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();

    switch (config_.mode) {
        case 0:  processNarrowband(mic_input, speaker_output, numSamples); break;
        case 1:  processWideband(mic_input, speaker_output, numSamples);   break;
        default: processHybrid(mic_input, speaker_output, numSamples);     break;
    }

    const auto t1 = std::chrono::high_resolution_clock::now();
    const float us = std::chrono::duration<float, std::micro>(t1 - t0).count();
    stats_.processingTimeUs = 0.9f * stats_.processingTimeUs + 0.1f * us;
    stats_.frameCount++;

    // 基频检测 (限频, 每 ~0.4s 一次)
    detect_elapsed_ms_ += static_cast<long long>(numSamples) * 1000 / config_.sampleRate;
    if (detect_elapsed_ms_ >= 400) {
        detect_elapsed_ms_ = 0;
        runToneDetection();
    }
}

void AudioProcessor::pushHistory(const float* mic, int n) {
    tone_detector_.push(mic, n);
}

// ============================================================
// 窄带谐波抵消模式
// ============================================================
void AudioProcessor::processNarrowband(const float* mic, float* out, int n) {
    float dp = 0.0f, ep = 0.0f, op = 0.0f;
    const int specBins = static_cast<int>(dhat_hist_.size());

    for (int i = 0; i < n; i++) {
        const float e = mic[i];

        // 扰动估计: d̂ = e - Ŝ*y(n-1)
        const float sy = sec_path_.filter(line_out_, prev_out_);
        float dhat = e - sy;
        dhat = std::clamp(dhat, -2.0f, 2.0f);

        // 谐波反噪声
        float y = harmonic_.process(e);
        if (y > 0.95f) y = 0.95f; else if (y < -0.95f) y = -0.95f;
        out[i] = y;
        prev_out_ = y;

        err_block_[i] = e;
        dhat_block_[i] = dhat;
        dhat_hist_[hist_pos_] = dhat;
        err_hist_[hist_pos_] = e;
        if (++hist_pos_ >= specBins) hist_pos_ = 0;

        dp += dhat * dhat;
        ep += e * e;
        op += y * y;
        out_pow_slow_ = 0.9995f * out_pow_slow_ + 0.0005f * (y * y);
    }

    pushHistory(mic, n);
    updateStats(dp / n, ep / n, op / n);
}

// ============================================================
// 低频宽带反馈模式 (internal model control)
// ============================================================
void AudioProcessor::processWideband(const float* mic, float* out, int n) {
    float dp = 0.0f, ep = 0.0f, op = 0.0f;
    const int specBins = static_cast<int>(dhat_hist_.size());
    const bool wbAllowed = sec_path_.isCalibrated();

    for (int i = 0; i < n; i++) {
        const float e = mic[i];

        const float sy = sec_path_.filter(line_out_, prev_out_);
        float dhat = std::clamp(e - sy, -2.0f, 2.0f);

        float y = 0.0f;
        if (wbAllowed) {
            const float d_bp = bp_ref_.process(dhat);
            const float e_bp = bp_err_.process(e);

            y = wide_filter_.process(d_bp);
            const float xh = sec_path_.filter(line_ref_, d_bp);
            wide_filter_.pushFilteredRef(xh);
            xh_pow_ = 0.9995f * xh_pow_ + 0.0005f * (xh * xh);

            // 归一化 FxLMS: xh_pow_ 是参考的"均方", 分母必须是 Σx̂² (即 均方 × 长度 L)。
            // 少乘 L 会让等效步长放大 L 倍 (L=256, 0.08 → ~20 ≫ 2), 一定发散。
            const float mu = std::min(0.1f, config_.stepSize /
                                      (static_cast<float>(wide_filter_.length()) * xh_pow_ + 1e-9f));
            wide_filter_.update(e_bp, mu);
        }

        if (y > 0.95f) y = 0.95f; else if (y < -0.95f) y = -0.95f;
        out[i] = y;
        prev_out_ = y;

        err_block_[i] = e;
        dhat_block_[i] = dhat;
        dhat_hist_[hist_pos_] = dhat;
        err_hist_[hist_pos_] = e;
        if (++hist_pos_ >= specBins) hist_pos_ = 0;

        dp += dhat * dhat;
        ep += e * e;
        op += y * y;
        out_pow_slow_ = 0.9995f * out_pow_slow_ + 0.0005f * (y * y);
    }

    pushHistory(mic, n);
    updateStats(dp / n, ep / n, op / n);
}

// ============================================================
// 混合模式: 窄带谐波 + 低频宽带
// ============================================================
void AudioProcessor::processHybrid(const float* mic, float* out, int n) {
    float dp = 0.0f, ep = 0.0f, op = 0.0f;
    const int specBins = static_cast<int>(dhat_hist_.size());
    const bool wbAllowed = sec_path_.isCalibrated();

    for (int i = 0; i < n; i++) {
        const float e = mic[i];

        // --- 1. 扰动估计 d̂ = e - Ŝ*y(n-1) ---
        const float sy = sec_path_.filter(line_out_, prev_out_);
        float dhat = std::clamp(e - sy, -2.0f, 2.0f);

        // --- 2. 低频宽带分支 ---
        float y_wb = 0.0f;
        if (wbAllowed) {
            const float d_bp = bp_ref_.process(dhat);
            const float e_bp = bp_err_.process(e);

            y_wb = wide_filter_.process(d_bp);
            const float xh = sec_path_.filter(line_ref_, d_bp);
            wide_filter_.pushFilteredRef(xh);
            xh_pow_ = 0.9995f * xh_pow_ + 0.0005f * (xh * xh);

            // 同 processWideband: 按 Σx̂² 归一化 (参考功率 × 滤波器长度)
            const float mu = std::min(0.1f, config_.stepSize /
                                      (static_cast<float>(wide_filter_.length()) * xh_pow_ + 1e-9f));
            wide_filter_.update(e_bp, mu);
        }

        // --- 3. 窄带谐波分支 (直接用实测误差做相关) ---
        const float y_h = harmonic_.process(e);

        // --- 4. 合成输出 ---
        float y = y_h + y_wb;
        if (y > 0.95f) y = 0.95f; else if (y < -0.95f) y = -0.95f;
        out[i] = y;
        prev_out_ = y;

        err_block_[i] = e;
        dhat_block_[i] = dhat;
        dhat_hist_[hist_pos_] = dhat;
        err_hist_[hist_pos_] = e;
        if (++hist_pos_ >= specBins) hist_pos_ = 0;

        dp += dhat * dhat;
        ep += e * e;
        op += y * y;
        out_pow_slow_ = 0.9995f * out_pow_slow_ + 0.0005f * (y * y);
    }

    pushHistory(mic, n);
    updateStats(dp / n, ep / n, op / n);
}

// ============================================================
// 基频检测与稳定性保护
// ============================================================
void AudioProcessor::runToneDetection() {
    const float f0 = tone_detector_.detect();
    const float prevF0 = harmonic_.f0();

    if (f0 > 0.0f) {
        const bool needLock = !tonal_disabled_ &&
                              ((harmonic_.harmonicCount() == 0) ||
                               (prevF0 <= 0.0f) ||
                               (std::fabs(f0 - prevF0) / prevF0 > 0.10f));
        if (needLock) {
            harmonic_.lockTo(f0);
        }
    } else if (harmonic_.harmonicCount() > 0) {
        // 连续 3 次检不到窄带成分就释放
        static thread_local int miss = 0;
        if (++miss >= 3) { harmonic_.lockTo(0.0f); miss = 0; }
        return;
    }

    // ---- 运行时失稳 / 过度驱动保护 ----
    guard_counter_++;
    const float outRms = std::sqrt(std::max(out_pow_slow_, 0.0f));
    const float nr = stats_.noiseReductionDb;
    if (outRms > 0.40f || nr < -3.0f) {
        guard_counter_ = 0;
        config_.tonalStep = std::max(5e-5f, config_.tonalStep * 0.5f);
        harmonic_.setStep(config_.tonalStep);
        harmonic_.reset();
        wide_filter_.reset();
        wide_filter_.setLeaky(0.999f);
        stats_.isConverged = false;
        // 归一化步长理论上稳定; 若仍然发散, 几乎总是次级路径相位估计不可信
        // (校准被拒, 或真实回环延迟超出模型窗口)。此时降步长只能减缓发散,
        // 不能改变梯度方向, 所以降够次数后必须直接停掉窄带分支, 免得持续啸叫。
        if (++guard_reductions_ >= 4 && !tonal_disabled_) {
            tonal_disabled_ = true;
            harmonic_.lockTo(0.0f);
            ANC_LOGI("Tonal branch disabled after 4 step reductions "
                     "(secondary-path phase unreliable?)");
        }
    } else if (guard_counter_ >= 12 && nr > 8.0f && outRms < 0.20f) {
        guard_counter_ = 0;
        // 缓慢回到与回环延迟匹配的标称步长 (与 updateTonalStep 同一规则)
        const float D = std::max(48.0f, static_cast<float>(std::max(1, sec_path_.delaySamples())));
        const float nominal = std::clamp(1.0f / D, 2e-4f, 2e-3f);
        if (config_.tonalStep < nominal) {
            config_.tonalStep = std::min(nominal, config_.tonalStep * 1.5f);
            harmonic_.setStep(config_.tonalStep);
        }
    }
}

// ============================================================
// 统计
// ============================================================
void AudioProcessor::updateStats(float dhatPower, float errPower, float outPower) {
    const float alpha = 0.01f;
    d_pow_ = (1.0f - alpha) * d_pow_ + alpha * dhatPower;
    e_pow_ = (1.0f - alpha) * e_pow_ + alpha * errPower;
    o_pow_ = (1.0f - alpha) * o_pow_ + alpha * outPower;

    stats_counter_++;
    if (stats_counter_ >= 8) {
        stats_counter_ = 0;
        stats_.referencePower = d_pow_;
        stats_.errorPower = e_pow_;
        stats_.outputPower = o_pow_;
        // 旧口径: 10log10(P(d̂)/P(e))。d̂ = e − Ŝ·y 是模型推算量,
        // 次级路径不准时分子会被 ΔS·y 抬高, 报出来的降噪量会虚高 → 只留作诊断。
        if (d_pow_ > 1e-12f && e_pow_ > 1e-12f) {
            nr_model_db_ = 10.0f * std::log10(d_pow_ / e_pow_);
        } else {
            nr_model_db_ = 0.0f;
        }
        // 新口径 (上报): 实测基线 P0 与实测残差 P(e) 之比。
        // 两侧都来自同一路麦克风实采, 与 Ŝ 准不准无关, 是真正的开/关对比。
        if (nr_base_ready_ && nr_base_pow_ > 1e-12f && e_pow_ > 1e-12f) {
            stats_.noiseReductionDb = 10.0f * std::log10(nr_base_pow_ / e_pow_);
        } else {
            stats_.noiseReductionDb = 0.0f;
        }
        stats_.filterNorm = wide_filter_.weightNorm();
        stats_.loopDelayMs = sec_path_.delayMs();
        stats_.tonalHz = harmonic_.f0();
        stats_.tonalCount = harmonic_.harmonicCount();
        stats_.isCalibrated = sec_path_.isCalibrated();
        stats_.isConverged = stats_.noiseReductionDb > 3.0f;
    }
}

ANCStats AudioProcessor::getStats() const { return stats_; }

void AudioProcessor::setStepSize(float mu) {
    config_.stepSize = std::clamp(mu, 1e-5f, 1.0f);
    wide_filter_.setStep(config_.stepSize);
}

void AudioProcessor::setOutputGain(float gain) {
    config_.outputGain = std::clamp(gain, 0.1f, 4.0f);
}

void AudioProcessor::setMode(int mode) {
    const int m = std::clamp(mode, 0, 2);
    if (m != config_.mode) {
        config_.mode = m;
        harmonic_.reset();
        wide_filter_.reset();
        bp_ref_.reset();
        bp_err_.reset();
        out_pow_slow_ = 0.0f;
    }
}

void AudioProcessor::setExternalSpeaker(bool external) {
    config_.externalSpeaker = external;
    config_.outputGain = external ? 2.0f : 1.0f;
}

void AudioProcessor::setMaxHarmonics(int n) {
    config_.maxHarmonics = std::clamp(n, 1, 8);
    harmonic_.init(static_cast<float>(config_.sampleRate), config_.maxHarmonics,
                   config_.controlLowHz, config_.controlHighHz, config_.tonalStep);
    harmonic_.setSecondaryPath(&sec_path_);
}

void AudioProcessor::reset() {
    wide_filter_.reset();
    harmonic_.reset();
    std::fill(dhat_hist_.begin(), dhat_hist_.end(), 0.0f);
    std::fill(err_hist_.begin(), err_hist_.end(), 0.0f);
    std::fill(out_block_.begin(), out_block_.end(), 0.0f);
    line_out_.reset();
    line_ref_.reset();
    bp_ref_.reset();
    bp_err_.reset();
    tone_detector_.reset();
    stats_ = ANCStats{};
    stats_.loopDelayMs = sec_path_.delayMs();
    stats_.isCalibrated = sec_path_.isCalibrated();
    d_pow_ = e_pow_ = o_pow_ = 0.0f;
    xh_pow_ = 1e-6f;
    out_pow_slow_ = 0.0f;
    stats_counter_ = 0;
    hist_pos_ = 0;
    prev_out_ = 0.0f;
    guard_counter_ = 0;
    detect_elapsed_ms_ = 0;
    guard_reductions_ = 0;
    tonal_disabled_ = false;
    startBaseline();   // 重启控制器 = 重新做一次开/关对比
    // 步长回到与回环延迟匹配的保守初值
    updateTonalStep();
}

// ============================================================
// 频谱 (非实时, 由 JNI 线程调用)
// ============================================================
void AudioProcessor::refreshAnalysis() {
    if (!spec_) return;
    const int specBins = static_cast<int>(dhat_hist_.size());
    std::vector<float> linear(static_cast<size_t>(specBins), 0.0f);

    // 环形缓冲线性化
    for (int i = 0; i < specBins; i++) {
        linear[i] = dhat_hist_[(hist_pos_ + i) % specBins];
    }
    spec_->analyze(linear.data(), specBins);
    const int half = specBins / 2;
    std::vector<float> tmp(static_cast<size_t>(half), 0.0f);
    spec_->getMagnitudeSpectrum(tmp.data(), half);
    {
        std::lock_guard<std::mutex> lk(spectrum_mutex_);
        ref_spectrum_ = tmp;
    }

    for (int i = 0; i < specBins; i++) {
        linear[i] = err_hist_[(hist_pos_ + i) % specBins];
    }
    spec_->analyze(linear.data(), specBins);
    spec_->getMagnitudeSpectrum(tmp.data(), half);
    {
        std::lock_guard<std::mutex> lk(spectrum_mutex_);
        err_spectrum_ = tmp;
    }
}

void AudioProcessor::getRefSpectrum(float* output, int len) {
    refreshAnalysis();
    std::lock_guard<std::mutex> lk(spectrum_mutex_);
    const int copyLen = std::min(len, static_cast<int>(ref_spectrum_.size()));
    std::memcpy(output, ref_spectrum_.data(), copyLen * sizeof(float));
}

void AudioProcessor::getErrSpectrum(float* output, int len) {
    refreshAnalysis();
    std::lock_guard<std::mutex> lk(spectrum_mutex_);
    const int copyLen = std::min(len, static_cast<int>(err_spectrum_.size()));
    std::memcpy(output, err_spectrum_.data(), copyLen * sizeof(float));
}

void AudioProcessor::getReductionSpectrum(float* output, int len) {
    refreshAnalysis();
    std::lock_guard<std::mutex> lk(spectrum_mutex_);
    const int copyLen = std::min(len, static_cast<int>(ref_spectrum_.size()));
    for (int i = 0; i < copyLen; i++) {
        output[i] = ref_spectrum_[i] - err_spectrum_[i];
    }
}

} // namespace anc
