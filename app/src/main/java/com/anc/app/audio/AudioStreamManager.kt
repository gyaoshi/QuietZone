package com.anc.app.audio

import android.content.Context
import android.util.Log
import com.anc.app.engine.ANCEngine
import com.anc.app.engine.ANCConfig
import com.anc.app.engine.ANCStats
import com.anc.app.utils.AudioDeviceHelper

/**
 * 音频流管理器 v2 (薄包装)
 *
 * v2 变更:
 *   - 所有实时音频处理由 C++ OboeEngine 在 Oboe 回调中完成
 *   - Kotlin 层仅负责引擎生命周期管理和参数传递
 *   - 不再使用 Java AudioRecord/AudioTrack
 */
class AudioStreamManager(private val context: Context) {

    companion object {
        private const val TAG = "AudioStreamManager"
    }

    private val engine = ANCEngine()
    private var isRunning = false

    /** 初始化 */
    fun init(config: ANCConfig = ANCConfig()): Boolean {
        val ok = engine.init(config)
        if (ok) {
            Log.i(TAG, "Engine initialized: sr=${config.sampleRate}, mode=${config.mode}")

            // 检测当前音频输出设备
            val deviceType = AudioDeviceHelper.getCurrentOutputType(context)
            val isLowLatency = AudioDeviceHelper.isLowLatencyDevice(context)
            Log.i(TAG, "Audio device: $deviceType, lowLatency=$isLowLatency")

            // 如果已连接有线/USB设备，自动开启外接音箱模式
            if (isLowLatency && deviceType != AudioDeviceHelper.OutputDeviceType.BUILTIN_SPEAKER) {
                engine.setExternalSpeaker(true)
                Log.i(TAG, "Auto-enabled external speaker mode")
            }
        } else {
            Log.e(TAG, "Engine initialization failed")
        }
        return ok
    }

    /** 启动 */
    fun start(): Boolean {
        if (isRunning) return true
        val ok = engine.start()
        if (ok) {
            isRunning = true
            Log.i(TAG, "Audio started")
        } else {
            Log.e(TAG, "Audio start failed")
        }
        return ok
    }

    /** 停止 */
    fun stop() {
        if (!isRunning) return
        engine.stop()
        isRunning = false
        Log.i(TAG, "Audio stopped")
    }

    /** 释放 */
    fun release() {
        stop()
        engine.release()
    }

    // 运行时控制
    fun setStepSize(mu: Float) = engine.setStepSize(mu)
    fun setMode(mode: Int) = engine.setMode(mode)
    fun setOutputGain(gain: Float) = engine.setOutputGain(gain)
    fun setExternalSpeaker(external: Boolean) = engine.setExternalSpeaker(external)
    fun calibrate() = engine.calibrate()
    fun reset() = engine.reset()
    fun getStats() = engine.getStats()
    fun getSpectrum(type: Int) = engine.getSpectrum(type)
    fun getEngine() = engine
    fun isRunning() = isRunning
}
