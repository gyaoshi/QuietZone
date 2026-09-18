/**
 * Oboe 低延迟音频回调引擎 — 头文件 (v4)
 *
 * v4 修复:
 *   1. 启动竞态: running_ 必须在 requestStart() 之前置位。
 *      旧版在 requestStart() 之后置位，输入流首个回调会看到 running_==false
 *      而返回 Stop，整个流被永久停掉 —— 这正是真机上麦克风缓冲一直占满、
 *      处理链空转的直接原因之一。
 *   2. 漂移管理: 输入/输出是两条独立时钟的流，标称同速率但存在 ppm 级漂移。
 *      旧版只有一次溢出警告，长期运行必然把缓冲填满或抽干。
 *      新版用弹性缓冲: 目标水位 + 高低水位纠偏(带淡入淡出)，削峰填谷。
 *   3. 实时路径零分配: 麦克风暂存缓冲预分配，不再在回调里 new vector。
 *   4. 校准链路打通: 校准必须拿到麦克风数据，因此放在输出回调里做，
 *      同时把采集缓冲改为预分配 + 无锁写入。
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
    void setMaxHarmonics(int n);
    void startCalibration();
    void resetProcessing();

    // 获取数据 (供JNI读取)
    ANCStats getStats() const;
    void getRefSpectrum(float* out, int len);
    void getErrSpectrum(float* out, int len);
    void getReductionSpectrum(float* out, int len);

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    bool isCalibrating() const { return calibrating_.load(std::memory_order_acquire); }

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
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> calibrating_{false};
    std::atomic<bool> anc_enabled_{false};

    // 实时路径预分配缓冲
    std::vector<float> mic_block_;      // 输出回调内使用
    std::vector<float> warp_block_;     // 时间弯曲临时缓冲
    int max_block_ = 0;

    // 弹性缓冲水位管理
    int target_fill_ = 0;               // 目标水位 (样本)
    int high_water_ = 0;                // 高水位: 超过则做时间弯曲压缩
    int low_water_ = 0;                 // 低水位
    float last_mic_sample_ = 0.0f;
    std::atomic<int64_t> drift_events_{0};

    // 校准
    std::atomic<int> calibration_frames_total_{0};
    std::atomic<int> calibration_frames_done_{0};
    std::vector<float> calibration_input_;   // 预分配, 回调内只做下标写入
    std::vector<float> calibration_output_;
    std::vector<float> probe_;

    // 统计快照 (atomic)
    std::atomic<float> stat_nr_db_{0.0f};
    std::atomic<float> stat_proc_us_{0.0f};
    std::atomic<float> stat_ref_power_{0.0f};
    std::atomic<float> stat_err_power_{0.0f};
    std::atomic<float> stat_out_power_{0.0f};
    std::atomic<float> stat_loop_delay_ms_{0.0f};
    std::atomic<float> stat_tonal_hz_{0.0f};
    std::atomic<int> stat_tonal_count_{0};
    std::atomic<int> stat_frame_count_{0};
    std::atomic<bool> stat_converged_{false};
    std::atomic<bool> stat_calibrated_{false};

    // 内部方法
    bool openInputStream();
    bool openOutputStream();
    void closeStreams();
    void processAudioFrame(float* inputData, float* outputData, int numFrames);
    void processCalibrationFrame(float* micData, float* outputData, int numFrames);
    void fillMicBlock(int numFrames);
    void updateStatsSnapshot(const ANCStats& stats);
};

} // namespace anc

#endif // OBOE_ENGINE_H
