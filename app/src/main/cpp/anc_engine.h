/**
 * ANC Engine - 主动降噪核心引擎 (v3 — 标量 FxLMS 修正版)
 *
 * 算法架构:
 *   1. Feedforward FxLMS (宽带前馈)
 *   2. Feedback FxLMS (窄带反馈)
 *   3. Hybrid ANC (混合模式)
 *   4. Leaky FxLMS (防权重漂移)
 *   5. 在线次级路径估计
 *
 * v3 关键修正:
 *   - FxLMS 更新使用标量滤波参考: x̂(n) = Ŝ * x(n) (标量)
 *   - 权重更新: w(n+1) = leaky·w(n) + μ·x̂(n)·e(n)·x_buf(n)
 *   - 逐样本 process+update 交错，保证 x_buffer_ 对齐正确
 *   - 反馈路径使用输出次级路径滤波: Ŝ·y(n-1) 避免 NaN
 *
 * 参考文献:
 *   [1] Kuo & Morgan, "Active Noise Control Systems", Wiley, 1996
 *   [2] "Latent FxLMS: Accelerating ANC with Neural Adaptive Filters", arXiv 2025
 *   [3] "Computation-efficient Online Secondary Path Modeling for Modified FXLMS", arXiv 2023
 *   [4] "Deep ANC: A Deep Learning Approach to Active Noise Control", NeurIPS 2021
 *   [5] "Frequency-Domain Filtered-x LMS Algorithms for ANC", Applied Sciences 2018
 */

#ifndef ANC_ENGINE_H
#define ANC_ENGINE_H

#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>

namespace anc {

// ============================================================
// 配置参数
// ============================================================
struct ANCConfig {
    int sampleRate = 48000;          // 采样率 (Hz)
    int filterLength = 256;          // 自适应滤波器长度 L
    int secondaryPathLength = 128;   // 次级路径估计长度 M
    float stepSize = 0.01f;          // FxLMS步长 μ
    float leakyFactor = 0.9999f;     // Leaky因子 (1-μδ), 防权重漂移
    int blockSize = 128;             // 每次处理的采样块大小
    int mode = 0;                    // 0=Feedforward, 1=Feedback, 2=Hybrid
    bool useNeon = true;             // 是否启用NEON指令加速
    bool externalSpeaker = false;    // 外接音箱模式
    float outputGain = 1.0f;         // 输出增益 (外接音箱时增大)
    int spectrumBins = 512;          // 频谱分析FFT点数
};

// ============================================================
// 实时性能统计
// ============================================================
struct ANCStats {
    float noiseReductionDb = 0.0f;   // 实时降噪量 (dB)
    float convergenceTime = 0.0f;    // 收敛时间 (ms)
    float processingTimeUs = 0.0f;   // 单帧处理时间 (μs)
    float referencePower = 0.0f;     // 参考信号功率
    float errorPower = 0.0f;         // 误差信号功率
    float filterNorm = 0.0f;         // 滤波器权重范数
    bool isConverged = false;        // 是否已收敛
    int frameCount = 0;              // 已处理帧数
};

// ============================================================
// FxLMS 自适应滤波器 (v3 — 标量更新修正版)
// ============================================================
class FxLMSFilter {
public:
    FxLMSFilter(int length, float stepSize, float leakyFactor = 0.9999f);
    ~FxLMSFilter();

    // 滤波器输出: y(n) = w^T * x_buf
    float process(float x_sample);

    /**
     * FxLMS 权重更新 (标量版本 — 正确实现)
     *
     * 核心公式:
     *   x̂(n) = Ŝ * x(n)    [标量滤波参考信号]
     *   w(n+1) = leaky·w(n) + μ·x̂(n)·e(n)·x_buf(n)
     *
     * 注意: x̂(n) 是标量，乘以整个参考缓冲向量 x_buf
     *       而非逐元素乘以滤波参考向量 (那是旧版的 bug)
     *
     * @param x_hat_n  标量滤波参考信号 x̂(n) = Ŝ * x(n)
     * @param error    误差信号 e(n)
     */
    void update(float x_hat_n, float error);

    // 重置滤波器
    void reset();

    // 获取权重 (用于调试/可视化)
    const float* getWeights() const { return weights_.data(); }
    int getLength() const { return length_; }

    // 设置步长 (运行时动态调整)
    void setStepSize(float mu) { stepSize_ = mu; }
    float getStepSize() const { return stepSize_; }

    // 设置Leaky因子
    void setLeakyFactor(float lf) { leakyFactor_ = lf; }

    // 获取权重范数
    float getWeightNorm() const;

    // 获取参考缓冲 (调试用)
    const float* getXBuffer() const { return x_buffer_.data(); }
    int getBufferIndex() const { return buffer_index_; }

private:
    int length_;
    float stepSize_;
    float leakyFactor_;
    std::vector<float> weights_;
    std::vector<float> x_buffer_;  // 参考信号环形缓冲
    int buffer_index_;
};

// ============================================================
// 次级路径估计器 (v3 — 环形缓冲优化)
// ============================================================
class SecondaryPathEstimator {
public:
    SecondaryPathEstimator(int filterLength, int blockSize);
    ~SecondaryPathEstimator();

    // 离线估计: 使用白噪声脉冲响应法
    void offlineEstimate(const float* impulseResponse, int len);

    // 在线估计: 使用辅助白噪声注入法
    void onlineUpdate(float error_sample, float auxiliary_noise);

    /**
     * 逐样本次级路径滤波: x̂(n) = Ŝ * x(n)
     * 返回标量滤波参考信号，供 FxLMSFilter::update() 使用
     *
     * 使用环形缓冲替代 rotate()，每样本 O(M) 复杂度
     *
     * @param input  当前参考信号样本
     * @return       标量滤波参考 x̂(n)
     */
    float filterSample(float input);

    // 分块滤波 (兼容接口, 内部调用 filterSample)
    void filterReference(const float* input, float* output, int len);

    // 重置
    void reset();

    // 获取估计的次级路径
    const float* getPathCoeffs() const { return path_coeffs_.data(); }
    int getPathLength() const { return filter_length_; }

    // 设置次级路径系数 (校准后设置)
    void setPathCoeffs(const float* coeffs, int len);

    /**
     * 使用独立缓冲的次级路径滤波
     * 用于 Hybrid 模式中反馈分支的 x̂_fb = Ŝ·d̂(n)
     * 与 filterSample() 的 path_buffer_ 完全独立，不会互相干扰
     *
     * @param input     输入样本
     * @param buf       独立的滤波环形缓冲 (调用方维护)
     * @param buf_idx   缓冲写入位置引用 (调用方维护)
     * @return          标量滤波输出
     */
    float filterSampleWithBuffer(float input, std::vector<float>& buf, int& buf_idx) const;

private:
    int filter_length_;
    int block_size_;
    std::vector<float> path_coeffs_;     // Ŝ(z)系数
    std::vector<float> path_buffer_;     // 滤波环形缓冲
    int path_buf_idx_;                   // 环形缓冲写入位置
    std::vector<float> estimate_weights_; // 在线估计自适应权重
    std::vector<float> estimate_buffer_;
    int estimate_buf_idx_;               // 估计缓冲写入位置
    bool is_estimated_;
};

// ============================================================
// 频谱分析器 (FFT)
// ============================================================
class SpectrumAnalyzer {
public:
    SpectrumAnalyzer(int fftSize);
    ~SpectrumAnalyzer();

    // 分析频谱
    void analyze(const float* input, int len);

    // 获取幅度谱 (dB)
    void getMagnitudeSpectrum(float* output, int outputLen);

    // 获取降噪频谱 (参考 vs 误差)
    void getReductionSpectrum(const float* ref, const float* err,
                              int len, float* output, int outputLen);

    int getFFTSize() const { return fft_size_; }

private:
    int fft_size_;
    std::vector<float> window_;
    std::vector<float> real_part_;
    std::vector<float> imag_part_;
    std::vector<float> magnitude_;
};

// ============================================================
// 音频处理器 (v3 — 标量 FxLMS 修正版)
// ============================================================
class AudioProcessor {
public:
    AudioProcessor();
    ~AudioProcessor();

    // 初始化
    bool init(const ANCConfig& config);

    // 处理一帧音频 (由Oboe回调调用)
    void processFrame(const float* mic_input, float* speaker_output, int numSamples);

    // 运行时控制
    void enable(bool on) { enabled_.store(on); }
    bool isEnabled() const { return enabled_.load(); }

    // 参数动态调整
    void setStepSize(float mu);
    void setOutputGain(float gain);
    void setMode(int mode);  // 0=Feedforward, 1=Feedback, 2=Hybrid
    void setExternalSpeaker(bool external);

    // 获取统计
    ANCStats getStats() const;

    // 获取频谱
    void getRefSpectrum(float* output, int len);
    void getErrSpectrum(float* output, int len);
    void getReductionSpectrum(float* output, int len);

    // 次级路径校准
    void startCalibration();
    bool isCalibrating() const { return calibrating_.load(); }

    // 重置
    void reset();

    // 释放资源
    void release() { reset(); }

    const ANCConfig& getConfig() const { return config_; }

private:
    ANCConfig config_;
    std::atomic<bool> enabled_;
    std::atomic<bool> calibrating_;

    // 核心组件
    std::unique_ptr<FxLMSFilter> forward_filter_;     // 前馈自适应滤波器
    std::unique_ptr<FxLMSFilter> feedback_filter_;     // 反馈自适应滤波器
    std::unique_ptr<SecondaryPathEstimator> sec_path_; // 次级路径估计

    // 信号缓冲 (用于频谱分析和统计)
    std::vector<float> ref_buffer_;          // 参考信号缓冲
    std::vector<float> err_buffer_;          // 误差信号缓冲
    std::vector<float> output_buffer_;       // 输出缓冲

    // 输出次级路径滤波缓冲 (用于反馈 ANC: 计算 Ŝ * y(n))
    // 与 sec_path_ 的 path_buffer_ 独立，因为两者同时使用
    std::vector<float> sec_output_buf_;      // Ŝ * y(n) 的 FIR 缓冲
    int sec_output_idx_;                     // 环形缓冲写入位置

    // 反馈分支独立次级路径缓冲 (用于 Hybrid: x̂_fb = Ŝ·d̂(n))
    // 与 sec_path_->filterSample(x_n) 的前馈调用独立
    std::vector<float> sec_fb_buf_;          // Ŝ·d̂(n) 的 FIR 缓冲
    int sec_fb_buf_idx_;                     // 环形缓冲写入位置

    // 前一帧输出 (用于反馈 ANC 延迟估计)
    float prev_output_;

    // 频谱分析
    std::unique_ptr<SpectrumAnalyzer> ref_analyzer_;
    std::unique_ptr<SpectrumAnalyzer> err_analyzer_;

    // 统计
    ANCStats stats_;
    float ref_power_accum_;
    float err_power_accum_;
    int stats_counter_;

    // 辅助噪声 (在线次级路径估计)
    std::vector<float> auxiliary_noise_;

    // 内部方法
    void processFeedforward(const float* mic, float* out, int n);
    void processFeedback(const float* mic, float* out, int n);
    void processHybrid(const float* mic, float* out, int n);
    void updateStats(float ref_power, float err_power);
    void generateAuxiliaryNoise(float* buffer, int len);

    /**
     * 对输出信号做次级路径滤波: Ŝ * y(n)
     * 用于反馈 ANC 估计原始噪声: d̂(n) = e(n) - Ŝ * y(n)
     *
     * 使用独立的环形缓冲，不与 sec_path_ 的内部缓冲冲突
     */
    float filterOutputSecondaryPath(float y_n);
};

} // namespace anc

#endif // ANC_ENGINE_H
