package com.anc.app.engine

/**
 * ANC引擎 JNI 接口 v2
 *
 * 变更:
 *   - nativeStart/Stop 替代旧 processFrame (Oboe C++ 回调内直接处理)
 *   - 新增 nativeIsRunning / nativeIsCalibrating 状态查询
 *   - 实时音频路径全程在 C++ 完成, JNI 仅用于参数/统计传递
 */
class ANCEngine {

    companion object {
        init {
            System.loadLibrary("anc_engine")
        }

        const val MODE_FEEDFORWARD = 0
        const val MODE_FEEDBACK = 1
        const val MODE_HYBRID = 2

        const val SPECTRUM_REFERENCE = 0
        const val SPECTRUM_ERROR = 1
        const val SPECTRUM_REDUCTION = 2
    }

    // Native 方法
    external fun nativeInit(
        sampleRate: Int, filterLength: Int, secondaryPathLength: Int,
        stepSize: Float, leakyFactor: Float, blockSize: Int, mode: Int
    ): Boolean

    external fun nativeStart(): Boolean
    external fun nativeStop()
    external fun nativeEnable(enable: Boolean)
    external fun nativeSetStepSize(mu: Float)
    external fun nativeSetMode(mode: Int)
    external fun nativeSetOutputGain(gain: Float)
    external fun nativeSetExternalSpeaker(external: Boolean)
    external fun nativeGetStats(): FloatArray?
    external fun nativeGetSpectrum(type: Int): FloatArray?
    external fun nativeCalibrate()
    external fun nativeIsCalibrating(): Boolean
    external fun nativeIsRunning(): Boolean
    external fun nativeReset()
    external fun nativeRelease()
    external fun nativeProcessFrame(micInput: FloatArray, speakerOutput: FloatArray, numSamples: Int)

    // ===== Kotlin 封装 =====

    private var initialized = false

    fun init(config: ANCConfig): Boolean {
        initialized = nativeInit(
            config.sampleRate, config.filterLength, config.secondaryPathLength,
            config.stepSize, config.leakyFactor, config.blockSize, config.mode
        )
        return initialized
    }

    fun start(): Boolean {
        if (!initialized) return false
        return nativeStart()
    }

    fun stop() {
        if (initialized) nativeStop()
    }

    fun enable(on: Boolean) {
        if (initialized) nativeEnable(on)
    }

    fun isRunning(): Boolean {
        return if (initialized) nativeIsRunning() else false
    }

    fun isCalibrating(): Boolean {
        return if (initialized) nativeIsCalibrating() else false
    }

    fun setStepSize(mu: Float) {
        if (initialized) nativeSetStepSize(mu)
    }

    fun setMode(mode: Int) {
        if (initialized) nativeSetMode(mode)
    }

    fun setOutputGain(gain: Float) {
        if (initialized) nativeSetOutputGain(gain)
    }

    fun setExternalSpeaker(external: Boolean) {
        if (initialized) nativeSetExternalSpeaker(external)
    }

    fun getStats(): ANCStats? {
        if (!initialized) return null
        val data = nativeGetStats() ?: return null
        return ANCStats(
            noiseReductionDb = data[0],
            processingTimeUs = data[1],
            referencePower = data[2],
            errorPower = data[3],
            filterNorm = data[4],
            isConverged = data[5] > 0.5f,
            frameCount = data[6].toLong()
        )
    }

    fun getSpectrum(type: Int): FloatArray? {
        if (!initialized) return null
        return nativeGetSpectrum(type)
    }

    fun calibrate() {
        if (initialized) nativeCalibrate()
    }

    fun reset() {
        if (initialized) nativeReset()
    }

    fun release() {
        if (initialized) {
            nativeRelease()
            initialized = false
        }
    }
}

data class ANCConfig(
    val sampleRate: Int = 48000,
    val filterLength: Int = 256,
    val secondaryPathLength: Int = 128,
    val stepSize: Float = 0.01f,
    val leakyFactor: Float = 0.9999f,
    val blockSize: Int = 128,
    val mode: Int = ANCEngine.MODE_HYBRID
)

data class ANCStats(
    val noiseReductionDb: Float = 0f,
    val processingTimeUs: Float = 0f,
    val referencePower: Float = 0f,
    val errorPower: Float = 0f,
    val filterNorm: Float = 0f,
    val isConverged: Boolean = false,
    val frameCount: Long = 0
)
