/**
 * Oboe 低延迟音频回调引擎 — 头文件
 */

#ifndef OBOE_ENGINE_H
#define OBOE_ENGINE_H

#include "anc_engine.h"
#include "ring_buffer.h"

#include <oboe/Oboe.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace anc {

class OboeEngine : public oboe::AudioStreamCallback {
public:
    OboeEngine();
    ~OboeEngine() override;

    bool init(const ANCConfig& config);
    bool start();
    void stop();
    void release();

    // 运行时控制 (线程安全)
    void enableANC(bool on);
    void setStepSize(float mu);
    void setMode(int mode);
    void setOutputGain(float gain);
    void setExternalSpeaker(bool external);
    void startCalibration();

    // 获取数据 (供JNI读取)
    ANCStats getStats() const;
    void getRefSpectrum(float* out, int len);
    void getErrSpectrum(float* out, int len);
    void getReductionSpectrum(float* out, int len);

    bool isRunning() const { return running_.load(); }
    bool isCalibrating() const { return calibrating_.load(); }

    // Oboe 回调
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream *oboeStream,
        void *audioData,
        int numFrames) override;

    void onErrorBeforeClose(oboe::AudioStream *oboeStream, oboe::Result error) override;
    void onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) override;

private:
    std::shared_ptr<oboe::AudioStream> input_stream_;
    std::shared_ptr<oboe::AudioStream> output_stream_;
    std::unique_ptr<AudioProcessor> processor_;
    std::unique_ptr<RingBuffer<float>> mic_buffer_;

    ANCConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> calibrating_{false};
    std::atomic<bool> anc_enabled_{false};

    // 校准
    std::atomic<int> calibration_frames_remaining_{0};
    std::mutex calibration_mutex_;
    std::vector<float> calibration_input_;
    std::vector<float> calibration_output_;
    uint32_t noise_state_ = 42;

    // 统计快照 (atomic)
    std::atomic<float> stat_nr_db_{0.0f};
    std::atomic<float> stat_proc_us_{0.0f};
    std::atomic<float> stat_ref_power_{0.0f};
    std::atomic<float> stat_err_power_{0.0f};
    std::atomic<bool> stat_converged_{false};
    std::atomic<int> stat_frame_count_{0};

    // 频谱快照
    std::mutex spectrum_mutex_;
    std::vector<float> ref_spectrum_;
    std::vector<float> err_spectrum_;
    int spectrum_update_counter_ = 0;

    // 内部方法
    bool openInputStream();
    bool openOutputStream();
    void closeStreams();
    void processAudioFrame(float* inputData, float* outputData, int numFrames);
    void processCalibrationFrame(float* inputData, float* outputData, int numFrames);
    void updateStatsSnapshot(const ANCStats& stats);
};

} // namespace anc

#endif // OBOE_ENGINE_H
