package com.anc.app.engine

import android.app.Application
import android.content.Intent
import android.os.Build
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * ANC 状态机 ViewModel
 *
 * 状态转换:
 *   Idle → Calibrating → Converging → Running
 *                                       ↓
 *   Idle ← ← ← ← ← ← ← ← ← ← ← ← ←
 *
 *   任何状态 → Error
 */

sealed class ANCState {
    object Idle : ANCState()
    object Calibrating : ANCState()
    data class Converging(val progress: Float) : ANCState()
    data class Running(
        val noiseReductionDb: Float,
        val processingTimeUs: Float,
        val isConverged: Boolean
    ) : ANCState()
    data class Error(val message: String) : ANCState()
}

data class ANCUIState(
    val state: ANCState = ANCState.Idle,
    val selectedMode: Int = ANCEngine.MODE_HYBRID,
    val stepSize: Float = 0.08f,
    val outputGain: Float = 1.0f,
    val externalSpeaker: Boolean = false,
    val maxHarmonics: Int = 6,
    val stats: ANCStats = ANCStats(),
    val refSpectrum: FloatArray = FloatArray(128) { -100f },
    val errSpectrum: FloatArray = FloatArray(128) { -100f }
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is ANCUIState) return false
        return state == other.state &&
                selectedMode == other.selectedMode &&
                stepSize == other.stepSize &&
                outputGain == other.outputGain &&
                externalSpeaker == other.externalSpeaker &&
                maxHarmonics == other.maxHarmonics &&
                stats == other.stats &&
                refSpectrum.contentEquals(other.refSpectrum) &&
                errSpectrum.contentEquals(other.errSpectrum)
    }

    override fun hashCode(): Int {
        var result = state.hashCode()
        result = 31 * result + selectedMode
        result = 31 * result + stepSize.hashCode()
        result = 31 * result + outputGain.hashCode()
        result = 31 * result + externalSpeaker.hashCode()
        result = 31 * result + maxHarmonics
        result = 31 * result + stats.hashCode()
        result = 31 * result + refSpectrum.contentHashCode()
        result = 31 * result + errSpectrum.contentHashCode()
        return result
    }
}

class ANCViewModel(application: Application) : AndroidViewModel(application) {

    private val _uiState = MutableStateFlow(ANCUIState())
    val uiState: StateFlow<ANCUIState> = _uiState.asStateFlow()

    private val engine = ANCEngine.getInstance()
    private var statsJob: Job? = null

    // ===== 用户操作 =====

    fun toggleANC() {
        when (_uiState.value.state) {
            is ANCState.Idle -> startANC()
            is ANCState.Running -> stopANC()
            is ANCState.Converging -> stopANC()
            is ANCState.Calibrating -> { /* 校准中不允许操作 */ }
            is ANCState.Error -> {
                _uiState.value = _uiState.value.copy(state = ANCState.Idle)
            }
        }
    }

    fun onPermissionResult(granted: Boolean) {
        if (granted) {
            startANC()
        } else {
            _uiState.value = _uiState.value.copy(
                state = ANCState.Error("需要麦克风权限才能使用降噪功能")
            )
        }
    }

    fun setMode(mode: Int) {
        _uiState.value = _uiState.value.copy(selectedMode = mode)
        engine.setMode(mode)
        sendConfigToService()
    }

    fun setStepSize(mu: Float) {
        _uiState.value = _uiState.value.copy(stepSize = mu)
        engine.setStepSize(mu)
        sendConfigToService()
    }

    fun setOutputGain(gain: Float) {
        _uiState.value = _uiState.value.copy(outputGain = gain)
        engine.setOutputGain(gain)
        sendConfigToService()
    }

    fun setExternalSpeaker(external: Boolean) {
        _uiState.value = _uiState.value.copy(externalSpeaker = external)
        engine.setExternalSpeaker(external)
        sendConfigToService()
    }

    fun recalibrate() {
        if (_uiState.value.state is ANCState.Idle ||
            _uiState.value.state is ANCState.Error) {
            return
        }
        _uiState.value = _uiState.value.copy(state = ANCState.Calibrating)
        // nativeCalibrate() 阻塞约 0.7s, 必须放在 IO 线程
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { engine.calibrate() }
            _uiState.value = _uiState.value.copy(
                state = if (ok) ANCState.Converging(0f) else ANCState.Running(0f, 0f, false)
            )
        }
    }

    // ===== 内部实现 =====

    private fun startANC() {
        val config = ANCConfig(
            mode = _uiState.value.selectedMode,
            stepSize = _uiState.value.stepSize
        )

        if (!engine.init(config)) {
            _uiState.value = _uiState.value.copy(
                state = ANCState.Error("引擎初始化失败")
            )
            return
        }

        engine.setOutputGain(_uiState.value.outputGain)
        engine.setExternalSpeaker(_uiState.value.externalSpeaker)
        engine.setMaxHarmonics(_uiState.value.maxHarmonics)

        // 启动 ANC 前台服务
        val context = getApplication<Application>()
        val serviceIntent = Intent(context, ANCService::class.java).apply {
            action = ANCService.ACTION_START
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(serviceIntent)
        } else {
            context.startService(serviceIntent)
        }

        // 启动 Oboe 引擎
        if (!engine.start()) {
            _uiState.value = _uiState.value.copy(
                state = ANCState.Error("音频流启动失败，请检查麦克风权限")
            )
            engine.release()
            return
        }

        _uiState.value = _uiState.value.copy(state = ANCState.Calibrating)
        startStatsPolling()
        runCalibration()
    }

    private fun stopANC() {
        statsJob?.cancel()
        statsJob = null
        engine.stop()
        engine.release()

        val context = getApplication<Application>()
        val stopIntent = Intent(context, ANCService::class.java).apply {
            action = ANCService.ACTION_STOP
        }
        context.startService(stopIntent)

        _uiState.value = _uiState.value.copy(state = ANCState.Idle)
    }

    /**
     * 次级路径校准 (播放扫频探测信号 + 匹配滤波)。
     * nativeCalibrate() 是阻塞的 (约 0.7 秒)，必须放在 IO 线程。
     * 校准失败不致命: 窄带谐波分支仍可用，只是宽带分支不启用。
     */
    private fun runCalibration() {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { engine.calibrate() }
            _uiState.value = _uiState.value.copy(
                state = if (ok) ANCState.Converging(0f) else ANCState.Running(0f, 0f, false)
            )
        }
    }

    private fun startStatsPolling() {
        statsJob?.cancel()
        statsJob = viewModelScope.launch {
            while (isActive) {
                val stats = engine.getStats()
                if (stats != null) {
                    val refSpec = engine.getSpectrum(ANCEngine.SPECTRUM_REFERENCE)
                    val errSpec = engine.getSpectrum(ANCEngine.SPECTRUM_ERROR)

                    val currentState = _uiState.value.state
                    // 校准结束、引擎在跑就是 Running; isConverged 仅作为徽标展示。
                    // (宽带噪声下 NR 可能很低, 用 isConverged 当状态机会永远停在"收敛中")
                    val newState: ANCState = when {
                        currentState is ANCState.Calibrating -> currentState
                        currentState is ANCState.Error -> currentState
                        engine.isRunning() -> ANCState.Running(
                            noiseReductionDb = stats.noiseReductionDb,
                            processingTimeUs = stats.processingTimeUs,
                            isConverged = stats.isConverged
                        )
                        else -> ANCState.Idle
                    }

                    _uiState.value = _uiState.value.copy(
                        state = newState,
                        stats = stats,
                        refSpectrum = refSpec ?: _uiState.value.refSpectrum,
                        errSpectrum = errSpec ?: _uiState.value.errSpectrum
                    )
                }
                delay(100) // 10Hz 刷新
            }
        }
    }

    private fun sendConfigToService() {
        val context = getApplication<Application>()
        val intent = Intent(context, ANCService::class.java).apply {
            action = ANCService.ACTION_UPDATE_CONFIG
            putExtra(ANCService.EXTRA_MODE, _uiState.value.selectedMode)
            putExtra(ANCService.EXTRA_STEP_SIZE, _uiState.value.stepSize)
            putExtra(ANCService.EXTRA_OUTPUT_GAIN, _uiState.value.outputGain)
            putExtra(ANCService.EXTRA_EXTERNAL_SPEAKER, _uiState.value.externalSpeaker)
        }
        context.startService(intent)
    }

    override fun onCleared() {
        super.onCleared()
        statsJob?.cancel()
    }
}
