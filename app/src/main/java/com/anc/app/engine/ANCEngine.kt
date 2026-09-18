package com.anc.app.engine

/**
 * ANC引擎 JNI 接口 v4 (Singleton)
 *
 * v4 变更:
 *   - nativeCalibrate() 改为阻塞式并返回是否成功 (内部播放扫频探测信号 + 匹配滤波)
 *   - nativeGetStats() 返回 12 个字段, 增加 基频/谐波数/回环延迟/校准状态
 *   - 新增 nativeSetMaxHarmonics()
 */
class ANCEngine private constructor() {

    companion object {
        @Volatile
        private var instance: ANCEngine? = null

        fun getInstance(): ANCEngine {
            return instance ?: synchronized(this) {
                instance ?: ANCEngine().also { instance = it }
            }
        }

        init {
            System.loadLibrary("anc_engine")
        }

        /**
         * 0 = 窄带谐波 (推荐, 对风扇/压缩机/发动机低频轰鸣)
         * 1 = 低频宽带反馈 (受回环延迟限制, 仅 30~250Hz)
         * 2 = 混合
         */
        const val MODE_NARROWBAND = 0
        const val MODE_WIDEBAND = 1
        const val MODE_HYBRID = 2

        // 兼容旧命名
        const val MODE_FEEDFORWARD = 0
        const val MODE_FEEDBACK = 1

        const val SPECTRUM_REFERENCE = 0
        const val SPECTRUM_ERROR = 1
        const val SPECTRUM_REDUCTION = 2

        private const val STATS_LEN = 12
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
    external fun nativeSetMaxHarmonics(n: Int)
    external fun nativeGetStats(): FloatArray?
    external fun nativeGetSpectrum(type: Int): FloatArray?
    external fun nativeCalibrate(): Boolean
    external fun nativeIsCalibrating(): Boolean
    external fun nativeIsRunning(): Boolean
    external fun nativeReset()
    external fun nativeRelease()

    // ===== Kotlin 封装 =====

    @Volatile
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

    fun isRunning(): Boolean = if (initialized) nativeIsRunning() else false

    fun isCalibrating(): Boolean = if (initialized) nativeIsCalibrating() else false

    fun setStepSize(mu: Float) { if (initialized) nativeSetStepSize(mu) }

    fun setMode(mode: Int) { if (initialized) nativeSetMode(mode) }

    fun setOutputGain(gain: Float) { if (initialized) nativeSetOutputGain(gain) }

    fun setExternalSpeaker(external: Boolean) {
        if (initialized) nativeSetExternalSpeaker(external)
    }

    fun setMaxHarmonics(n: Int) { if (initialized) nativeSetMaxHarmonics(n) }

    fun getStats(): ANCStats? {
        if (!initialized) return null
        val d = nativeGetStats() ?: return null
        if (d.size < STATS_LEN) return null
        return ANCStats(
            noiseReductionDb = d[0],
            processingTimeUs = d[1],
            referencePower = d[2],
            errorPower = d[3],
            outputPower = d[4],
            filterNorm = d[5],
            loopDelayMs = d[6],
            tonalHz = d[7],
            tonalCount = d[8].toInt(),
            isConverged = d[9] > 0.5f,
            isCalibrated = d[10] > 0.5f,
            frameCount = d[11].toLong()
        )
    }

    fun getSpectrum(type: Int): FloatArray? =
        if (initialized) nativeGetSpectrum(type) else null

    /** 阻塞执行次级路径校准 (必须在后台线程调用, 约 0.7 秒) */
    fun calibrate(): Boolean = if (initialized) nativeCalibrate() else false

    fun reset() { if (initialized) nativeReset() }

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
    val secondaryPathLength: Int = 1024,
    val stepSize: Float = 0.08f,
    val leakyFactor: Float = 0.9995f,
    val blockSize: Int = 128,
    val mode: Int = ANCEngine.MODE_HYBRID
)

data class ANCStats(
    val noiseReductionDb: Float = 0f,
    val processingTimeUs: Float = 0f,
    val referencePower: Float = 0f,
    val errorPower: Float = 0f,
    val outputPower: Float = 0f,
    val filterNorm: Float = 0f,
    /** 校准得到的扬声器→麦克风回环延迟 (ms) */
    val loopDelayMs: Float = 0f,
    /** 当前锁定的噪声基频 (Hz), 0 表示未检出窄带成分 */
    val tonalHz: Float = 0f,
    /** 正在抵消的谐波个数 */
    val tonalCount: Int = 0,
    val isConverged: Boolean = false,
    val isCalibrated: Boolean = false,
    val frameCount: Long = 0
)
