/**
 * ANC Engine v4 — 单麦克风可实现的主动降噪内核
 *
 * ============================================================
 * 为什么 v3 不能工作 (结论回顾)
 * ============================================================
 *   v3 的 FxLMS 更新写成  w += μ·x̂(n)·e(n)·x(n-i)  —— x̂ 是"标量"而不是滤波参考向量。
 *   对零均值噪声，E[x̂(n)·e(n)·x(n-i)] 是三阶矩，恒为 0 ⇒ 没有梯度 ⇒ 权重永不增长
 *   ⇒ y≈0 ⇒ 输出静音 ⇒ 降噪量恒为 0。这与真机现象完全一致。
 *   另外 v3 把误差信号编造成 e = mic + anti（零延迟、单位增益），
 *   与真实声学次级路径无关，即使权重能增长也是在拟合一个不存在的东西。
 *
 * ============================================================
 * v4 的结构 (单麦克风 + 单扬声器唯一物理可行的方案)
 * ============================================================
 *   1) 次级路径 Ŝ(z) 用运行时校准真实测出来（对数扫频探测 + 互相关），
 *      不再假设"零延迟、单位增益、延迟 8 采样、增益 0.5"。
 *      内部用"纯延迟 + 有效支路"的稀疏结构，既覆盖真实回环延迟又不浪费算力。
 *
 *   2) 窄带谐波抵消器 (HarmonicCanceller) —— 主力。
 *      单麦克风违反因果性，宽带前馈原理上不可能；但**纯音的未来相位是已知的**，
 *      所以由内部振荡器合成参考信号，不受回环延迟限制。
 *      对每个谐波用复数权重 (wc,ws)，FxLMS 的滤波参考就是该谐波频率上 Ŝ 的复增益
 *      A∠φ ⇒ 参考 = A·cos(ωn+φ)。环路被精确归一到单位增益，收敛快且稳定。
 *      风扇嗡鸣、压缩机、发动机低频轰鸣、交流声都属于这一类。
 *
 *   3) 低频宽带反馈 (Internal Model Control)。
 *      d̂(n) = e(n) − Ŝ*y(n) 估计"如果没有反噪声，麦克风上会是什么"，
 *      再用 Ŝ 滤波后的 d̂ 做 FxLMS。带宽受回环延迟限制(~1/4τ)，
 *      因此只在 30~250Hz 工作，并且必须在校准成功后才启用。
 *
 *   4) 降噪量指标是真实的: NR = 10·log10(P_d̂ / P_e)。
 *      d̂ 是"没有反噪声时的扰动估计"，e 是实测残余 —— 两者都来自真实信号。
 *
 * 参考文献:
 *   [1] Kuo & Morgan, "Active Noise Control Systems", Wiley, 1996
 *   [2] Elliott, "Signal Processing for Active Control", Academic Press, 2001
 *   [3] Widrow & Stearns, "Adaptive Signal Processing", Prentice-Hall, 1985
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
    int sampleRate = 48000;            // 采样率 (Hz)
    int filterLength = 256;            // 宽带自适应滤波器长度 L
    int secondaryPathLength = 1024;    // 次级路径模型最大长度 (样本, 48k→21ms)
    float stepSize = 0.08f;            // 宽带归一化步长
    float leakyFactor = 0.9995f;       // Leaky 因子
    int blockSize = 128;               // 处理块大小
    int mode = 2;                      // 0=窄带 1=宽带 2=混合
    bool useNeon = true;
    bool externalSpeaker = false;
    float outputGain = 1.0f;
    int spectrumBins = 1024;

    // ---- v4 控制参数 ----
    float controlLowHz = 30.0f;        // 控制频段下界
    float controlHighHz = 600.0f;      // 控制频段上界 (谐波锁定范围)
    int maxHarmonics = 6;              // 最多抵消的谐波数
    float tonalStep = 2.0e-3f;         // 谐波抵消归一化步长 (运行时按回环延迟重算)
    float widebandHighHz = 250.0f;     // 宽带分支上界
    float tonalityThresholdDb = 6.0f;  // 判定"是纯音"的峰值/中位数门限

    // ---- v4 校准参数 ----
    float calibProbeLevel = 0.20f;     // 探测信号幅度
    float calibProbeSeconds = 0.6f;    // 探测时长
    float calibMinFreqHz = 40.0f;      // 扫频起点
    float calibMaxFreqHz = 6000.0f;    // 扫频终点

    // ---- v4 未校准时的兜底模型 ----
    float defaultLoopDelayMs = 8.0f;   // 假设的回环延迟
    float defaultSpkCutoffHz = 220.0f; // 假设的扬声器低频截止
    float defaultSpkGain = 0.35f;      // 假设的通带增益
};

// ============================================================
// 实时性能统计
// ============================================================
struct ANCStats {
    float noiseReductionDb = 0.0f;   // NR = 10log10(P_d̂/P_e)
    float processingTimeUs = 0.0f;   // 单帧处理耗时
    float referencePower = 0.0f;     // d̂ 功率 (扰动估计)
    float errorPower = 0.0f;         // e 功率 (实测残余)
    float outputPower = 0.0f;        // 反噪声输出功率
    float filterNorm = 0.0f;         // 宽带权重范数
    float loopDelayMs = 0.0f;        // 校准得到的回环延迟
    float tonalHz = 0.0f;            // 当前锁定的基频
    float tonalAttenDb = 0.0f;       // 基频处衰减估计
    int tonalCount = 0;              // 参与抵消的谐波数
    int frameCount = 0;
    bool isConverged = false;
    bool isCalibrated = false;
};

// ============================================================
// 无锁环形延迟线 (供次级路径滤波使用, 允许一路模型多个独立状态)
// ============================================================
struct SecPathLine {
    std::vector<float> ring;
    int idx = 0;

    void resize(int size) { ring.assign((size_t)size, 0.0f); idx = 0; }
    void reset() { std::fill(ring.begin(), ring.end(), 0.0f); idx = 0; }
    int size() const { return (int)ring.size(); }
};

// ============================================================
// 次级路径 Ŝ(z): 稀疏"纯延迟 + 有效支路"结构
// ============================================================
class SecondaryPath {
public:
    SecondaryPath();

    /** 用校准得到的冲激响应设置模型 (len 为原始样点数) */
    void setImpulseResponse(const float* h, int len, float sampleRate);

    /** 兜底模型: 纯延迟 + 2 阶扬声器低频滚降 + 一个中频谐振 */
    void setDefaultModel(float sampleRate, float loopDelayMs,
                         float spkCutoffHz, float spkGain);

    /** 逐样本 FIR: y = Ŝ * x，使用调用方提供的延迟线 (可多路并行) */
    float filter(SecPathLine& line, float x) const;

    /** 在指定频率上的复频响 Ŝ(e^{jω}) */
    void responseAt(float freqHz, float& mag, float& phaseRad) const;

    void resetLines() const {}

    bool isCalibrated() const { return calibrated_; }
    /** 延迟线长度 (调用方按此分配 SecPathLine) */
    int ringSize() const { return ring_size_; }
    /** 有效支路起点 (纯延迟样点数) */
    int delaySamples() const { return active_delay_; }
    int tapCount() const { return taps_; }
    float sampleRate() const { return fs_; }
    float delayMs() const { return fs_ > 0 ? 1000.0f * active_delay_ / fs_ : 0.0f; }
    /** 冲激响应峰值位置(样本) */
    int peakIndex() const { return peak_index_; }

    /** 有效支路长度(用于一次性打印/调试) */
    const float* taps() const { return coeff_.data(); }

private:
    void rebuild();

    float fs_ = 48000.0f;
    int delay_ = 0;          // 模型覆盖的纯延迟 (ring 大小 = delay_ + taps_)
    int active_delay_ = 0;   // 有效支路实际起点
    int taps_ = 0;
    int peak_index_ = 0;
    int ring_size_ = 0;      // 延迟线长度 (2 的幂)
    std::vector<float> coeff_;   // 有效支路系数 (从 active_delay_ 起)
    std::vector<float> full_;    // 保留完整冲激响应 (供频响计算)
    int full_len_ = 0;
    bool calibrated_ = false;
};

// ============================================================
// 归一化 FxLMS 自适应滤波器 (向量滤波参考 —— 正确实现)
// ============================================================
class AdaptiveFilter {
public:
    AdaptiveFilter() = default;
    void init(int length, float step, float leaky);
    ~AdaptiveFilter();

    /** y(n) = w^T · x(n)，同时把 x(n) 压入参考延迟线 */
    float process(float x);

    /** 把当前样本的滤波参考 x̂(n) 压入 x̂ 延迟线 (必须在 process 与 update 之间调用) */
    void pushFilteredRef(float xhat);

    /**
     * 权重更新:  w(n+1) = leaky·w(n) − μ·e(n)·x̂(n-k)
     * 其中 μ 由调用方按参考功率归一化后传入。
     */
    void update(float error, float mu);

    void reset();
    float weightNorm() const;
    int length() const { return L_; }
    const float* weights() const { return w_.data(); }
    void setStep(float s) { step_ = s; }
    float step() const { return step_; }
    void setLeaky(float l) { leaky_ = l; }

private:
    std::vector<float> w_;
    std::vector<float> xbuf_;
    std::vector<float> xhbuf_;
    int L_ = 0;
    int idx_ = 0;
    float step_ = 0.05f;
    float leaky_ = 0.9995f;
};

// ============================================================
// 双二阶级联带通 (二阶节直 II 型)
// ============================================================
class Biquad {
public:
    void setLowpass(float fc, float fs, float q = 0.70710678f);
    void setHighpass(float fc, float fs, float q = 0.70710678f);
    void reset() { z1_ = z2_ = 0.0f; }
    float process(float x) {
        float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }
    /** 复频响 */
    void complexGain(float freqHz, float fs, float& mag, float& phase) const;
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
private:
    float z1_ = 0.0f, z2_ = 0.0f;
};

class BandPass {
public:
    void setBand(float loHz, float hiHz, float fs);
    void reset() { hp_.reset(); lp_.reset(); }
    float process(float x) { return lp_.process(hp_.process(x)); }
    void complexGain(float f, float fs, float& mag, float& phase) const;
private:
    Biquad hp_, lp_;
};

// ============================================================
// 窄带谐波抵消器 (内部合成参考)
// ============================================================
class HarmonicCanceller {
public:
    void init(float sampleRate, int maxHarmonics,
              float bandLow, float bandHigh, float step);
    void setSecondaryPath(const SecondaryPath* sp) { sp_ = sp; }

    /** 锁定基频。返回实际启用的谐波数 */
    int lockTo(float f0);

    /** 逐样本: 用误差 e 自适应并返回本分支的反噪声输出 */
    float process(float e);

    void reset();
    void setStep(float s) { step_ = s; }

    float f0() const { return f0_; }
    int harmonicCount() const { return n_; }
    float attenuationDb(int k) const;

private:
    struct Harm {
        float f = 0.0f;
        float wc = 0.0f;      // cos 权重
        float ws = 0.0f;      // sin 权重
        float A = 1.0f;       // |Ŝ(f)|
        float cosPhi = 1.0f;  // cos∠Ŝ(f)
        float sinPhi = 0.0f;
        float mu = 0.0f;
        double ph = 0.0;      // 相位累积
        double dph = 0.0;     // 每样本相位增量
    };

    static constexpr int kMaxHarmonicsCapacity = 12;
    Harm h_[kMaxHarmonicsCapacity];
    int n_ = 0;
    int max_ = 6;
    float fs_ = 48000.0f;
    float bandLow_ = 30.0f;
    float bandHigh_ = 600.0f;
    float step_ = 0.06f;
    float f0_ = 0.0f;
    long long nSamp_ = 0;
    const SecondaryPath* sp_ = nullptr;
};

// ============================================================
// 基频检测器 (Hann 窗 + Goertzel 扫描 + 抛物线插值 + 次谐波判决)
// ============================================================
class ToneDetector {
public:
    void init(float sampleRate, int windowLen, float loHz, float hiHz,
              float tonalityThresholdDb);
    /** 写入历史 (由音频线程调用, 只做拷贝) */
    void push(const float* x, int n);
    /**
     * 执行检测 (有计算量, 调用方需限频)。
     * @return 检测到的基频 Hz; 0 表示未检出明显的窄带分量
     */
    float detect();

    float lastTonalityDb() const { return tonality_db_; }
    void reset();

private:
    float goertzelWindowed(float freqHz) const;

    std::vector<float> hist_;
    std::vector<float> win_;
    int wlen_ = 0;
    int pos_ = 0;
    int filled_ = 0;
    float fs_ = 48000.0f;
    float lo_ = 30.0f;
    float hi_ = 600.0f;
    float thr_db_ = 6.0f;
    float tonality_db_ = 0.0f;
    // 预分配的粗扫缓冲 (避免在音频线程分配内存)
    static constexpr int kMaxScanPoints = 256;
    float scanMag_[kMaxScanPoints] = {0.0f};
    float scanFreq_[kMaxScanPoints] = {0.0f};
    float sortBuf_[kMaxScanPoints] = {0.0f};
};

// ============================================================
// 音频处理器
// ============================================================
class AudioProcessor {
public:
    AudioProcessor();
    ~AudioProcessor();

    bool init(const ANCConfig& config);

    /** 处理一帧音频 (由 Oboe 回调调用, 实时路径, 无内存分配) */
    void processFrame(const float* mic_input, float* speaker_output, int numSamples);

    void enable(bool on) { enabled_.store(on, std::memory_order_release); }
    bool isEnabled() const { return enabled_.load(std::memory_order_acquire); }

    // 运行时控制
    void setStepSize(float mu);
    void setOutputGain(float gain);
    void setMode(int mode);
    void setExternalSpeaker(bool external);
    void setMaxHarmonics(int n);

    /** 写入校准得到的次级路径冲激响应 (线程安全, 会短暂停用控制) */
    void setSecondaryPathIr(const float* h, int len);

    /** 用探测/录音数据估计次级路径冲激响应 (在调用线程做, 不占用音频线程) */
    static int estimateImpulseResponse(const float* recorded, const float* probe,
                                       int len, int maxTaps, float* out);

    /** 生成校准探测信号 (对数扫频) */
    static void generateProbe(float* out, int len, float fs,
                              float f0, float f1, float level);

    ANCStats getStats() const;
    void getRefSpectrum(float* output, int len);
    void getErrSpectrum(float* output, int len);
    void getReductionSpectrum(float* output, int len);

    /** 供 UI 显示的当前次级路径 (dB 幅度谱, 对数频率摆放) */
    const SecondaryPath& secondaryPath() const { return sec_path_; }

    void reset();
    void release() { reset(); }

    const ANCConfig& getConfig() const { return config_; }

    /** 只更新统计/频谱的非实时快照 (JNI 线程调用) */
    void refreshAnalysis();

private:
    void processNarrowband(const float* mic, float* out, int n);
    void processWideband(const float* mic, float* out, int n);
    void processHybrid(const float* mic, float* out, int n);

    void runToneDetection();
    void updateStats(float dhatPower, float errPower, float outPower);
    void pushHistory(const float* mic, int n);
    /** 依据实测回环延迟重算谐波分支的保守步长 */
    void updateTonalStep();

    ANCConfig config_;
    std::atomic<bool> enabled_{false};

    // ---- 核心组件 ----
    SecondaryPath sec_path_;
    HarmonicCanceller harmonic_;
    AdaptiveFilter wide_filter_;
    ToneDetector tone_detector_;

    // ---- 多路次级路径延迟线 ----
    SecPathLine line_out_;    // Ŝ * y(n)       -> 扰动估计
    SecPathLine line_ref_;    // Ŝ * d̂(n)       -> 宽带滤波参考

    // ---- 控制带通 ----
    BandPass bp_ref_;         // 用于 d̂ (宽带分支参考)
    BandPass bp_err_;         // 用于 e  (宽带分支误差)
    BandPass bp_hist_;        // 用于基频检测历史

    // ---- 缓冲 (全部预分配) ----
    std::vector<float> dhat_hist_;      // d̂ 历史 (频谱)
    std::vector<float> err_hist_;       // e 历史 (频谱)
    std::vector<float> mic_hist_;       // 麦克风历史 (给 ToneDetector)
    std::vector<float> dhat_block_;
    std::vector<float> err_block_;
    std::vector<float> out_block_;
    int hist_pos_ = 0;
    int mic_hist_pos_ = 0;

    // ---- 频谱 ----
    mutable std::mutex spectrum_mutex_;
    std::vector<float> ref_spectrum_;
    std::vector<float> err_spectrum_;

    // ---- 统计 ----
    ANCStats stats_;
    float d_pow_ = 0.0f;
    float e_pow_ = 0.0f;
    float o_pow_ = 0.0f;
    float xh_pow_ = 1e-6f;
    float out_pow_slow_ = 0.0f;
    int stats_counter_ = 0;
    int detect_counter_ = 0;
    int guard_counter_ = 0;
    float guard_baseline_ = 0.0f;
    float prev_out_ = 0.0f;
    float tonal_ref_pow_ = 0.0f;
    float tonal_res_pow_ = 0.0f;
    int last_f0_lock_ms_ = 0;
    long long detect_elapsed_ms_ = 0;

    // 频谱分析用的 FFT (延迟初始化, 在 init 中分配)
    std::unique_ptr<class SpectrumAnalyzer> spec_;
};

} // namespace anc

#endif // ANC_ENGINE_H
