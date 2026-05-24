/**
 * Oboe 低延迟音频回调引擎 — 实现
 */

#include "oboe_engine.h"
#include <android/log.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>

#define LOG_TAG "OboeEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace anc {

OboeEngine::OboeEngine() = default;
OboeEngine::~OboeEngine() { release(); }

bool OboeEngine::init(const ANCConfig& config) {
    config_ = config;
    processor_ = std::make_unique<AudioProcessor>();
    if (!processor_->init(config)) {
        LOGE("Failed to init AudioProcessor");
        return false;
    }
    mic_buffer_ = std::make_unique<RingBuffer<float>>(config.blockSize * 4);
    ref_spectrum_.resize(config.spectrumBins / 2, -100.0f);
    err_spectrum_.resize(config.spectrumBins / 2, -100.0f);
    LOGI("OboeEngine initialized: sr=%d, blockSize=%d", config.sampleRate, config.blockSize);
    return true;
}

bool OboeEngine::start() {
    if (running_.load()) return true;
    if (!openInputStream()) return false;
    if (!openOutputStream()) { closeStreams(); return false; }

    auto result = input_stream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input: %s", oboe::convertToText(result));
        closeStreams(); return false;
    }
    result = output_stream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start output: %s", oboe::convertToText(result));
        input_stream_->requestStop(); closeStreams(); return false;
    }

    running_.store(true);
    anc_enabled_.store(true);
    processor_->enable(true);
    LOGI("Audio streams started");
    return true;
}

void OboeEngine::stop() {
    if (!running_.load()) return;
    anc_enabled_.store(false);
    processor_->enable(false);
    running_.store(false);
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

bool OboeEngine::openInputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(oboe::ChannelCount::Mono)
           ->setSampleRate(config_.sampleRate)
           ->setFramesPerCallback(config_.blockSize)
           ->setBufferCapacityInFrames(config_.blockSize * 2)
           ->setAudioApi(oboe::AudioApi::AAudio)
           ->setUsage(oboe::Usage::VoiceCommunication)
           ->setContentType(oboe::ContentType::Speech)
           ->setCallback(this);

    auto result = builder.openStream(input_stream_);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input (AAudio): %s", oboe::convertToText(result));
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        result = builder.openStream(input_stream_);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open input (OpenSL ES): %s", oboe::convertToText(result));
            return false;
        }
        LOGW("Using OpenSL ES fallback for input");
    }
    LOGI("Input stream opened");
    return true;
}

bool OboeEngine::openOutputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(oboe::ChannelCount::Mono)
           ->setSampleRate(config_.sampleRate)
           ->setFramesPerCallback(config_.blockSize)
           ->setBufferCapacityInFrames(config_.blockSize * 2)
           ->setAudioApi(oboe::AudioApi::AAudio)
           ->setUsage(oboe::Usage::VoiceCommunication)
           ->setContentType(oboe::ContentType::Speech)
           ->setCallback(this);

    auto result = builder.openStream(output_stream_);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output (AAudio): %s", oboe::convertToText(result));
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        result = builder.openStream(output_stream_);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open output (OpenSL ES): %s", oboe::convertToText(result));
            return false;
        }
        LOGW("Using OpenSL ES fallback for output");
    }
    LOGI("Output stream opened");
    return true;
}

void OboeEngine::closeStreams() {
    if (input_stream_) { input_stream_->close(); input_stream_.reset(); }
    if (output_stream_) { output_stream_->close(); output_stream_.reset(); }
}

// ===== Oboe 回调 =====

oboe::DataCallbackResult OboeEngine::onAudioReady(
    oboe::AudioStream *oboeStream, void *audioData, int numFrames)
{
    if (!running_.load()) return oboe::DataCallbackResult::Stop;

    if (oboeStream->getDirection() == oboe::Direction::Input) {
        auto* inputData = static_cast<float*>(audioData);
        size_t written = mic_buffer_->write(inputData, numFrames);
        if (written < (size_t)numFrames) {
            LOGW("Mic buffer overflow: %zu/%d", written, numFrames);
        }
        return oboe::DataCallbackResult::Continue;
    }

    if (oboeStream->getDirection() == oboe::Direction::Output) {
        auto* outputData = static_cast<float*>(audioData);

        if (calibrating_.load()) {
            processCalibrationFrame(nullptr, outputData, numFrames);
            return oboe::DataCallbackResult::Continue;
        }

        std::vector<float> micData(numFrames, 0.0f);
        size_t toRead = std::min(mic_buffer_->availableRead(), (size_t)numFrames);
        if (toRead > 0) mic_buffer_->read(micData.data(), toRead);

        if (anc_enabled_.load()) {
            processAudioFrame(micData.data(), outputData, numFrames);
        } else {
            memset(outputData, 0, numFrames * sizeof(float));
        }
        return oboe::DataCallbackResult::Continue;
    }

    return oboe::DataCallbackResult::Continue;
}

void OboeEngine::processAudioFrame(float* inputData, float* outputData, int numFrames) {
    auto t0 = std::chrono::high_resolution_clock::now();
    processor_->processFrame(inputData, outputData, numFrames);
    auto t1 = std::chrono::high_resolution_clock::now();
    float procUs = std::chrono::duration<float, std::micro>(t1 - t0).count();

    ANCStats stats = processor_->getStats();
    stats.processingTimeUs = procUs;
    updateStatsSnapshot(stats);

    spectrum_update_counter_++;
    if (spectrum_update_counter_ >= 8) {
        spectrum_update_counter_ = 0;
        std::lock_guard<std::mutex> lock(spectrum_mutex_);
        processor_->getRefSpectrum(ref_spectrum_.data(), ref_spectrum_.size());
        processor_->getErrSpectrum(err_spectrum_.data(), err_spectrum_.size());
    }
}

void OboeEngine::processCalibrationFrame(float* inputData, float* outputData, int numFrames) {
    int remaining = calibration_frames_remaining_.load();
    for (int i = 0; i < numFrames; i++) {
        if (remaining > 0) {
            // 使用简单的线性同余生成器替代 rand() (线程安全)
            noise_state_ = noise_state_ * 1103515245u + 12345u;
            float noise = 0.0316f * (2.0f * (float)(noise_state_ & 0x7FFFFFFF) / 0x7FFFFFFF - 1.0f);
            outputData[i] = noise;
            if (inputData) {
                std::lock_guard<std::mutex> lock(calibration_mutex_);
                calibration_input_.push_back(noise);
                calibration_output_.push_back(inputData[i]);
            }
            remaining--;
            calibration_frames_remaining_.store(remaining);
        } else {
            outputData[i] = 0.0f;
        }
    }
    if (remaining <= 0 && calibrating_.load()) {
        calibrating_.store(false);
        // 使用校准数据更新次级路径估计
        {
            std::lock_guard<std::mutex> lock(calibration_mutex_);
            if (!calibration_input_.empty()) {
                processor_->offlineCalibrate(calibration_input_.data(),
                                              calibration_output_.data(),
                                              (int)calibration_input_.size());
                LOGI("Calibration complete: %zu frames, secondary path updated",
                     calibration_input_.size());
            }
        }
        anc_enabled_.store(true);
        processor_->enable(true);
    }
}

void OboeEngine::startCalibration() {
    // 先初始化所有校准状态, 再设置 calibrating 标志 (避免与音频回调竞争)
    anc_enabled_.store(false);
    processor_->enable(false);
    calibration_frames_remaining_.store(config_.sampleRate / 2);
    {
        std::lock_guard<std::mutex> lock(calibration_mutex_);
        calibration_input_.clear();
        calibration_output_.clear();
        calibration_input_.reserve(calibration_frames_remaining_.load());
        calibration_output_.reserve(calibration_frames_remaining_.load());
    }
    calibrating_.store(true);
    LOGI("Calibration started: %d frames", calibration_frames_remaining_.load());
}

void OboeEngine::enableANC(bool on) { anc_enabled_.store(on); processor_->enable(on); }
void OboeEngine::setStepSize(float mu) { processor_->setStepSize(mu); }
void OboeEngine::setMode(int mode) { processor_->setMode(mode); }
void OboeEngine::setOutputGain(float gain) { processor_->setOutputGain(gain); }
void OboeEngine::setExternalSpeaker(bool external) { processor_->setExternalSpeaker(external); }

ANCStats OboeEngine::getStats() const {
    ANCStats stats;
    stats.noiseReductionDb = stat_nr_db_.load(std::memory_order_relaxed);
    stats.processingTimeUs = stat_proc_us_.load(std::memory_order_relaxed);
    stats.referencePower = stat_ref_power_.load(std::memory_order_relaxed);
    stats.errorPower = stat_err_power_.load(std::memory_order_relaxed);
    stats.isConverged = stat_converged_.load(std::memory_order_relaxed);
    stats.frameCount = stat_frame_count_.load(std::memory_order_relaxed);
    return stats;
}

void OboeEngine::getRefSpectrum(float* out, int len) {
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    int copyLen = std::min(len, (int)ref_spectrum_.size());
    std::memcpy(out, ref_spectrum_.data(), copyLen * sizeof(float));
}

void OboeEngine::getErrSpectrum(float* out, int len) {
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    int copyLen = std::min(len, (int)err_spectrum_.size());
    std::memcpy(out, err_spectrum_.data(), copyLen * sizeof(float));
}

void OboeEngine::getReductionSpectrum(float* out, int len) {
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    int copyLen = std::min(len, (int)ref_spectrum_.size());
    for (int i = 0; i < copyLen; i++) {
        out[i] = ref_spectrum_[i] - err_spectrum_[i];
    }
}

void OboeEngine::updateStatsSnapshot(const ANCStats& stats) {
    stat_nr_db_.store(stats.noiseReductionDb, std::memory_order_relaxed);
    stat_proc_us_.store(stats.processingTimeUs, std::memory_order_relaxed);
    stat_ref_power_.store(stats.referencePower, std::memory_order_relaxed);
    stat_err_power_.store(stats.errorPower, std::memory_order_relaxed);
    stat_converged_.store(stats.isConverged, std::memory_order_relaxed);
    stat_frame_count_.store(stats.frameCount, std::memory_order_relaxed);
}

void OboeEngine::onErrorBeforeClose(oboe::AudioStream*, oboe::Result error) {
    LOGE("Audio error before close: %s", oboe::convertToText(error));
}

void OboeEngine::onErrorAfterClose(oboe::AudioStream*, oboe::Result error) {
    LOGE("Audio error after close: %s", oboe::convertToText(error));
    running_.store(false);
}

} // namespace anc
