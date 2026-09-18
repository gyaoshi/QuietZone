/**
 * Oboe 低延迟音频回调引擎 — 实现 (v4)
 *
 * v4 相对 v3 的修复:
 *   1. 启动竞态
 *      v3: running_ 在 requestStart() 之后才置 true，输入流首个回调看到 false
 *          就返回 Stop，整个输入流被永久停掉。
 *      v4: running_ 在 requestStart() 之前置位；输出流启动前先预热 30ms，
 *          让输入环形缓冲积累到目标水位。
 *
 *   2. 时钟漂移
 *      v3: 输入/输出是两条独立时钟的流，标称 48k 也有 ppm 级漂移，
 *          缓冲迟早被填满(旧日志里的 "Mic buffer overflow")或抽干。
 *      v4: 弹性缓冲 + 目标水位；偏离水位时对整块做 1 个样本的线性时间弯曲，
 *          不产生阶跃，长跑也不会溢出。
 *
 *   3. 实时路径零分配
 *      v3: 每个输出回调 std::vector<float> micData(numFrames) 分配一次。
 *      v4: 预分配 mic_block_ / warp_block_ / 校准缓冲。
 *
 *   4. 校准链路
 *      v3: processCalibrationFrame(nullptr, ...) 第一参数硬编码 nullptr，
 *          if (inputData) 永假 ⇒ 一条数据都没采到；且 start() 从不调用校准。
 *      v4: 校准在输出回调里做(那里才有麦克风数据)，播放对数扫频探测信号，
 *          录音与探测信号做匹配滤波得到真实次级路径冲激响应。
 */

#include "oboe_engine.h"
#include "spectrum_analyzer.h"

#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#define LOG_TAG "OboeEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace anc {

namespace {
enum CalibState { kCalibIdle = 0, kCalibPlay = 1, kCalibDone = 2 };
}

OboeEngine::OboeEngine() = default;
OboeEngine::~OboeEngine() { release(); }

// ============================================================
// 初始化
// ============================================================
bool OboeEngine::init(const ANCConfig& config) {
    config_ = config;
    processor_ = std::make_unique<AudioProcessor>();
    if (!processor_->init(config)) {
        LOGE("Failed to init AudioProcessor");
        return false;
    }

    max_block_ = config.blockSize * 2;
    mic_buffer_ = std::make_unique<RingBuffer<float>>(
        static_cast<size_t>(config.blockSize) * 8u);

    // 实时路径预分配
    mic_block_.assign(static_cast<size_t>(max_block_), 0.0f);
    warp_block_.assign(static_cast<size_t>(max_block_), 0.0f);

    // 水位: 目标 2 个块，高水位 4 个块
    target_fill_ = config.blockSize * 2;
    high_water_ = config.blockSize * 4;
    low_water_ = config.blockSize / 2;

    // 校准缓冲预分配 (探测信号最大 2 秒)
    const int calibMax = config.sampleRate * 2;
    calibration_input_.assign(static_cast<size_t>(calibMax), 0.0f);
    calibration_output_.assign(static_cast<size_t>(calibMax), 0.0f);
    probe_.assign(static_cast<size_t>(calibMax), 0.0f);

    LOGI("OboeEngine init: sr=%d block=%d pathLen=%d mode=%d",
         config.sampleRate, config.blockSize, config.secondaryPathLength, config.mode);
    return true;
}

// ============================================================
// 启停
// ============================================================
bool OboeEngine::start() {
    if (running_.load(std::memory_order_acquire)) return true;

    stop_requested_.store(false, std::memory_order_release);
    // 关键: running_ 必须在 requestStart() 之前置位
    running_.store(true, std::memory_order_release);

    if (!openInputStream()) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    if (!openOutputStream()) {
        closeStreams();
        running_.store(false, std::memory_order_release);
        return false;
    }

    auto result = input_stream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input: %s", oboe::convertToText(result));
        closeStreams();
        running_.store(false, std::memory_order_release);
        return false;
    }

    // 预热: 输入流先跑一会儿，把环形缓冲垫到目标水位
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    result = output_stream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start output: %s", oboe::convertToText(result));
        input_stream_->requestStop();
        closeStreams();
        running_.store(false, std::memory_order_release);
        return false;
    }

    processor_->enable(true);
    anc_enabled_.store(true, std::memory_order_release);
    LOGI("Audio streams started (in sr=%d out sr=%d)",
         input_stream_->getSampleRate(), output_stream_->getSampleRate());
    return true;
}

void OboeEngine::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    anc_enabled_.store(false, std::memory_order_release);
    processor_->enable(false);
    running_.store(false, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    if (input_stream_) input_stream_->requestStop();
    if (output_stream_) output_stream_->requestStop();
    closeStreams();
    LOGI("Audio streams stopped");
}

void OboeEngine::release() {
    stop();
    if (processor_) { processor_->release(); processor_.reset(); }
    mic_buffer_.reset();
    LOGI("OboeEngine released");
}

// ============================================================
// 流配置
//
// 为什么不使用 Usage::VoiceCommunication:
//   通信用途会让平台音频链启用 AEC / NS / AGC:
//     - AEC 会把"我们主动播放的反噪声"当成回声去消除 —— 直接抵消我们的控制信号;
//     - NS 会试图滤掉我们要消除的低频轰鸣;
//     - AGC 会不断改变增益, 使误差信号的非线性无法被自适应滤波器拟合。
//   三者都会让 ANC 看起来"完全没用"。因此输入输出都改用媒体用途,
//   并把输入预设设为 VoiceRecognition(关闭 AGC/NS 的原始采集)。
//   若设备拒绝该组合, 自动回退到通信用途(至少保证能出声)。
// ============================================================
namespace {

struct StreamPlan {
    oboe::Usage usage;
    oboe::ContentType contentType;
    const char* name;
};

const StreamPlan kInputPlans[2] = {
    {oboe::Usage::Media, oboe::ContentType::Speech, "Media/VR"},
    {oboe::Usage::VoiceCommunication, oboe::ContentType::Speech, "Comms(legacy)"},
};

const StreamPlan kOutputPlans[2] = {
    {oboe::Usage::Media, oboe::ContentType::Music, "Media/Music"},
    {oboe::Usage::VoiceCommunication, oboe::ContentType::Speech, "Comms(legacy)"},
};

} // namespace

bool OboeEngine::openInputStream() {
    const oboe::SharingMode modes[2] = {oboe::SharingMode::Exclusive, oboe::SharingMode::Shared};
    const char* modeNames[2] = {"Exclusive", "Shared"};

    for (const StreamPlan& plan : kInputPlans) {
        for (int attempt = 0; attempt < 2; attempt++) {
            for (auto api : {oboe::AudioApi::AAudio, oboe::AudioApi::OpenSLES}) {
                oboe::AudioStreamBuilder builder;
                builder.setDirection(oboe::Direction::Input)
                       ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                       ->setSharingMode(modes[attempt])
                       ->setFormat(oboe::AudioFormat::Float)
                       ->setChannelCount(oboe::ChannelCount::Mono)
                       ->setSampleRate(config_.sampleRate)
                       ->setFramesPerCallback(config_.blockSize)
                       ->setBufferCapacityInFrames(config_.blockSize * 4)
                       ->setAudioApi(api)
                       ->setUsage(plan.usage)
                       ->setContentType(plan.contentType)
                       ->setInputPreset(oboe::InputPreset::VoiceRecognition)
                       ->setChannelConversionAllowed(true)
                       ->setFormatConversionAllowed(true)
                       ->setCallback(this);
                auto result = builder.openStream(input_stream_);
                if (result == oboe::Result::OK) {
                    LOGI("Input opened: api=%s sharing=%s usage=%s sr=%d fmt=%d ch=%d",
                         api == oboe::AudioApi::AAudio ? "AAudio" : "OpenSLES", modeNames[attempt],
                         plan.name, input_stream_->getSampleRate(),
                         (int)input_stream_->getFormat(), input_stream_->getChannelCount());
                    return true;
                }
                LOGE("Open input failed (api=%d sharing=%s usage=%s): %s",
                     (int)api, modeNames[attempt], plan.name, oboe::convertToText(result));
                input_stream_.reset();
            }
        }
    }
    return false;
}

bool OboeEngine::openOutputStream() {
    const oboe::SharingMode modes[2] = {oboe::SharingMode::Exclusive, oboe::SharingMode::Shared};
    const char* modeNames[2] = {"Exclusive", "Shared"};

    for (const StreamPlan& plan : kOutputPlans) {
        for (int attempt = 0; attempt < 2; attempt++) {
            for (auto api : {oboe::AudioApi::AAudio, oboe::AudioApi::OpenSLES}) {
                oboe::AudioStreamBuilder builder;
                builder.setDirection(oboe::Direction::Output)
                       ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                       ->setSharingMode(modes[attempt])
                       ->setFormat(oboe::AudioFormat::Float)
                       ->setChannelCount(oboe::ChannelCount::Mono)
                       ->setSampleRate(config_.sampleRate)
                       ->setFramesPerCallback(config_.blockSize)
                       ->setBufferCapacityInFrames(config_.blockSize * 4)
                       ->setAudioApi(api)
                       ->setUsage(plan.usage)
                       ->setContentType(plan.contentType)
                       ->setChannelConversionAllowed(true)
                       ->setFormatConversionAllowed(true)
                       ->setCallback(this);
                auto result = builder.openStream(output_stream_);
                if (result == oboe::Result::OK) {
                    LOGI("Output opened: api=%s sharing=%s usage=%s sr=%d fmt=%d ch=%d",
                         api == oboe::AudioApi::AAudio ? "AAudio" : "OpenSLES", modeNames[attempt],
                         plan.name, output_stream_->getSampleRate(),
                         (int)output_stream_->getFormat(), output_stream_->getChannelCount());
                    return true;
                }
                LOGE("Open output failed (api=%d sharing=%s usage=%s): %s",
                     (int)api, modeNames[attempt], plan.name, oboe::convertToText(result));
                output_stream_.reset();
            }
        }
    }
    return false;
}

void OboeEngine::closeStreams() {
    if (input_stream_) { input_stream_->close(); input_stream_.reset(); }
    if (output_stream_) { output_stream_->close(); output_stream_.reset(); }
}

// ============================================================
// 弹性缓冲: 取一个块的麦克风数据, 必要时做 1 样本时间弯曲纠偏
// ============================================================
void OboeEngine::fillMicBlock(int numFrames) {
    if (numFrames > max_block_) numFrames = max_block_;

    const int avail = static_cast<int>(mic_buffer_->availableRead());
    int slip = 0;
    if (avail < target_fill_) {                 // 水位偏低 → 拉伸
        slip = +1;
    } else if (avail - numFrames > high_water_) {  // 水位偏高 → 压缩
        slip = -1;
    }

    const int want = std::min(max_block_, numFrames + std::max(0, slip));
    int got = static_cast<int>(mic_buffer_->read(mic_block_.data(), static_cast<size_t>(want)));

    // 供不上就重复最后一个样本 (保持扰动估计连续, 好过补零)
    const float last = (got > 0) ? mic_block_[got - 1] : last_mic_sample_;
    for (int i = got; i < numFrames; i++) mic_block_[i] = last;
    if (got > 0) last_mic_sample_ = mic_block_[got - 1];

    if (slip != 0 && got >= numFrames + std::max(0, slip)) {
        // 线性时间弯曲 (numFrames + slip) → numFrames，不产生阶跃
        const float step = static_cast<float>(numFrames + slip) / static_cast<float>(numFrames);
        const int srcLen = numFrames + slip;
        for (int i = 0; i < numFrames; i++) {
            float p = static_cast<float>(i) * step;
            int i0 = static_cast<int>(p);
            if (i0 >= srcLen - 1) i0 = srcLen - 2;
            if (i0 < 0) i0 = 0;
            const float f = p - static_cast<float>(i0);
            warp_block_[i] = mic_block_[i0] * (1.0f - f) + mic_block_[i0 + 1] * f;
        }
        std::memcpy(mic_block_.data(), warp_block_.data(),
                    static_cast<size_t>(numFrames) * sizeof(float));
        drift_events_.fetch_add(1, std::memory_order_relaxed);
    }
}

// ============================================================
// Oboe 回调
// ============================================================
oboe::DataCallbackResult OboeEngine::onAudioReady(
    oboe::AudioStream* oboeStream, void* audioData, int numFrames)
{
    if (!running_.load(std::memory_order_acquire)) {
        if (oboeStream->getDirection() == oboe::Direction::Output) {
            std::memset(audioData, 0, static_cast<size_t>(numFrames) * sizeof(float));
        }
        return oboe::DataCallbackResult::Continue;
    }

    if (oboeStream->getDirection() == oboe::Direction::Input) {
        auto* in = static_cast<float*>(audioData);
        // 单声道处理: 多声道时只取第一路
        const int ch = oboeStream->getChannelCount();
        if (ch > 1) {
            for (int i = 0; i < numFrames; i++) mic_block_[i] = in[i * ch];
            mic_buffer_->write(mic_block_.data(), static_cast<size_t>(numFrames));
        } else {
            mic_buffer_->write(in, static_cast<size_t>(numFrames));
        }
        return oboe::DataCallbackResult::Continue;
    }

    auto* outputData = static_cast<float*>(audioData);

    // 校准期间: 播放探测信号并录音
    if (calibrating_.load(std::memory_order_acquire)) {
        fillMicBlock(numFrames);
        processCalibrationFrame(mic_block_.data(), outputData, numFrames);
        return oboe::DataCallbackResult::Continue;
    }

    fillMicBlock(numFrames);

    if (anc_enabled_.load(std::memory_order_acquire)) {
        processAudioFrame(mic_block_.data(), outputData, numFrames);
    } else {
        std::memset(outputData, 0, static_cast<size_t>(numFrames) * sizeof(float));
    }
    return oboe::DataCallbackResult::Continue;
}

void OboeEngine::processAudioFrame(float* inputData, float* outputData, int numFrames) {
    processor_->processFrame(inputData, outputData, numFrames);

    ANCStats stats = processor_->getStats();
    updateStatsSnapshot(stats);
}

void OboeEngine::processCalibrationFrame(float* micData, float* outputData, int numFrames) {
    const int total = calibration_frames_total_.load(std::memory_order_acquire);
    int pos = calibration_frames_done_.load(std::memory_order_relaxed);
    const int limit = std::min(total, static_cast<int>(calibration_output_.size()));

    for (int i = 0; i < numFrames; i++) {
        if (pos < limit) {
            outputData[i] = calibration_output_[static_cast<size_t>(pos)];
            calibration_input_[static_cast<size_t>(pos)] = micData[i];
            pos++;
        } else {
            outputData[i] = 0.0f;
        }
    }
    calibration_frames_done_.store(pos, std::memory_order_release);
    if (pos >= limit) {
        calibration_frames_done_.store(limit, std::memory_order_release);
        calibrating_.store(false, std::memory_order_release);
    }
}

// ============================================================
// 校准 (在调用线程阻塞执行, 由 JNI 从后台线程触发)
// ============================================================
void OboeEngine::startCalibration() {
    if (!running_.load(std::memory_order_acquire)) {
        LOGE("Calibration skipped: engine not running");
        return;
    }

    const int sr = config_.sampleRate;
    int probeLen = static_cast<int>(config_.calibProbeSeconds * sr);
    probeLen = std::min(probeLen, static_cast<int>(probe_.size()));
    if (probeLen < sr / 10) return;

    // 关闭 ANC 输出，准备探测信号
    const bool wasEnabled = anc_enabled_.load(std::memory_order_acquire);
    anc_enabled_.store(false, std::memory_order_release);
    processor_->enable(false);

    AudioProcessor::generateProbe(probe_.data(), probeLen, static_cast<float>(sr),
                                  config_.calibMinFreqHz, config_.calibMaxFreqHz,
                                  config_.calibProbeLevel);
    std::fill(calibration_input_.begin(), calibration_input_.end(), 0.0f);
    std::copy(probe_.begin(), probe_.begin() + probeLen, calibration_output_.begin());

    calibration_frames_total_.store(probeLen, std::memory_order_release);
    calibration_frames_done_.store(0, std::memory_order_release);
    calibrating_.store(true, std::memory_order_release);

    // 等采集完成 (最多 3 秒)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (calibrating_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const bool ok = !calibrating_.load(std::memory_order_acquire);
    calibrating_.store(false, std::memory_order_release);

    if (!ok) {
        LOGE("Calibration timeout");
        processor_->enable(wasEnabled);
        anc_enabled_.store(wasEnabled, std::memory_order_release);
        return;
    }

    // 匹配滤波估计冲激响应
    std::vector<float> ir(static_cast<size_t>(config_.secondaryPathLength), 0.0f);
    const int taps = AudioProcessor::estimateImpulseResponse(
        calibration_input_.data(), probe_.data(), probeLen,
        config_.secondaryPathLength, ir.data());

    if (taps > 0) {
        processor_->setSecondaryPathIr(ir.data(), taps);
        const ANCStats st = processor_->getStats();
        LOGI("Calibration done: taps=%d loopDelay=%.2f ms",
             taps, st.loopDelayMs);
    } else {
        LOGE("Calibration failed: no impulse response");
    }

    processor_->enable(wasEnabled);
    anc_enabled_.store(wasEnabled, std::memory_order_release);
}

// ============================================================
// 运行时控制
// ============================================================
void OboeEngine::enableANC(bool on) {
    anc_enabled_.store(on, std::memory_order_release);
    processor_->enable(on);
}
void OboeEngine::setStepSize(float mu) { processor_->setStepSize(mu); }
void OboeEngine::setMode(int mode) { processor_->setMode(mode); }
void OboeEngine::setOutputGain(float gain) { processor_->setOutputGain(gain); }
void OboeEngine::setExternalSpeaker(bool external) { processor_->setExternalSpeaker(external); }
void OboeEngine::setMaxHarmonics(int n) { processor_->setMaxHarmonics(n); }
void OboeEngine::resetProcessing() { processor_->reset(); }

// ============================================================
// 数据读取 (JNI 线程)
// ============================================================
ANCStats OboeEngine::getStats() const {
    ANCStats stats;
    stats.noiseReductionDb = stat_nr_db_.load(std::memory_order_relaxed);
    stats.processingTimeUs = stat_proc_us_.load(std::memory_order_relaxed);
    stats.referencePower = stat_ref_power_.load(std::memory_order_relaxed);
    stats.errorPower = stat_err_power_.load(std::memory_order_relaxed);
    stats.outputPower = stat_out_power_.load(std::memory_order_relaxed);
    stats.loopDelayMs = stat_loop_delay_ms_.load(std::memory_order_relaxed);
    stats.tonalHz = stat_tonal_hz_.load(std::memory_order_relaxed);
    stats.tonalCount = stat_tonal_count_.load(std::memory_order_relaxed);
    stats.frameCount = stat_frame_count_.load(std::memory_order_relaxed);
    stats.isConverged = stat_converged_.load(std::memory_order_relaxed);
    stats.isCalibrated = stat_calibrated_.load(std::memory_order_relaxed);
    return stats;
}

void OboeEngine::getRefSpectrum(float* out, int len) {
    processor_->getRefSpectrum(out, len);
}

void OboeEngine::getErrSpectrum(float* out, int len) {
    processor_->getErrSpectrum(out, len);
}

void OboeEngine::getReductionSpectrum(float* out, int len) {
    processor_->getReductionSpectrum(out, len);
}

void OboeEngine::updateStatsSnapshot(const ANCStats& stats) {
    stat_nr_db_.store(stats.noiseReductionDb, std::memory_order_relaxed);
    stat_proc_us_.store(stats.processingTimeUs, std::memory_order_relaxed);
    stat_ref_power_.store(stats.referencePower, std::memory_order_relaxed);
    stat_err_power_.store(stats.errorPower, std::memory_order_relaxed);
    stat_out_power_.store(stats.outputPower, std::memory_order_relaxed);
    stat_loop_delay_ms_.store(stats.loopDelayMs, std::memory_order_relaxed);
    stat_tonal_hz_.store(stats.tonalHz, std::memory_order_relaxed);
    stat_tonal_count_.store(stats.tonalCount, std::memory_order_relaxed);
    stat_frame_count_.store(stats.frameCount, std::memory_order_relaxed);
    stat_converged_.store(stats.isConverged, std::memory_order_relaxed);
    stat_calibrated_.store(stats.isCalibrated, std::memory_order_relaxed);
}

void OboeEngine::onErrorBeforeClose(oboe::AudioStream*, oboe::Result error) {
    LOGE("Audio error before close: %s", oboe::convertToText(error));
}

void OboeEngine::onErrorAfterClose(oboe::AudioStream*, oboe::Result error) {
    LOGE("Audio error after close: %s", oboe::convertToText(error));
    running_.store(false, std::memory_order_release);
}

} // namespace anc
