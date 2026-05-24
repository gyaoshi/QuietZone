package com.anc.app.ui

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import com.anc.app.engine.ANCState
import com.anc.app.engine.ANCViewModel
import com.anc.app.ui.components.*
import com.anc.app.utils.PermissionHelper

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ANCScreen(
    viewModel: ANCViewModel = viewModel()
) {
    val context = LocalContext.current
    val uiState by viewModel.uiState.collectAsState()

    // 权限管理
    var hasPermission by remember {
        mutableStateOf(PermissionHelper.hasRecordAudioPermission(context))
    }

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val audioGranted = permissions[Manifest.permission.RECORD_AUDIO] ?: false
        hasPermission = audioGranted
        viewModel.onPermissionResult(audioGranted)
    }

    // 首次启动时检查权限
    LaunchedEffect(Unit) {
        if (!hasPermission) {
            PermissionHelper.requestPermissions(context as android.app.Activity)
        }
    }

    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = ANCColors.Accent,
            surface = ANCColors.DarkBg,
            background = ANCColors.DarkBg
        )
    ) {
        Surface(
            modifier = Modifier.fillMaxSize(),
            color = ANCColors.DarkBg
        ) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                // ── 标题栏 ──
                HeaderSection(state = uiState.state)

                // ── 主控按钮 ──
                MainControlButton(
                    state = uiState.state,
                    hasPermission = hasPermission,
                    onClick = {
                        if (!hasPermission) {
                            permissionLauncher.launch(
                                PermissionHelper.REQUIRED_PERMISSIONS.toTypedArray()
                            )
                        } else {
                            viewModel.toggleANC()
                        }
                    }
                )

                // ── 统计面板 ──
                if (uiState.state !is ANCState.Idle) {
                    StatsPanel(stats = uiState.stats)
                }

                // ── 模式选择 ──
                ModeSelector(
                    selectedMode = uiState.selectedMode,
                    onModeChange = { viewModel.setMode(it) },
                    enabled = uiState.state !is ANCState.Calibrating
                )

                // ── 频谱 ──
                if (uiState.state is ANCState.Running || uiState.state is ANCState.Converging) {
                    SpectrumView(
                        refSpectrum = uiState.refSpectrum,
                        errSpectrum = uiState.errSpectrum
                    )
                }

                // ── 参数面板 ──
                ParameterPanel(
                    stepSize = uiState.stepSize,
                    onStepSizeChange = { viewModel.setStepSize(it) },
                    outputGain = uiState.outputGain,
                    onOutputGainChange = { viewModel.setOutputGain(it) },
                    externalSpeaker = uiState.externalSpeaker,
                    onExternalSpeakerChange = { viewModel.setExternalSpeaker(it) },
                    enabled = uiState.state !is ANCState.Calibrating
                )

                // ── 算法信息 ──
                AlgorithmInfoCard()

                // ── 错误提示 ──
                if (uiState.state is ANCState.Error) {
                    ErrorBanner(
                        message = (uiState.state as ANCState.Error).message,
                        onDismiss = { viewModel.toggleANC() }  // 重置到Idle
                    )
                }
            }
        }
    }
}

@Composable
private fun HeaderSection(state: ANCState) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column {
            Text(
                text = "QuietZone",
                style = MaterialTheme.typography.headlineLarge,
                fontWeight = androidx.compose.ui.text.font.FontWeight.Bold,
                color = ANCColors.TextPrimary
            )
            Text(
                text = "主动降噪 · Active Noise Cancellation",
                style = MaterialTheme.typography.bodySmall,
                color = ANCColors.TextSecondary
            )
        }

        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            androidx.compose.foundation.Canvas(modifier = Modifier.size(12.dp)) {
                drawCircle(
                    color = when (state) {
                        is ANCState.Running -> ANCColors.AccentGreen
                        is ANCState.Converging -> ANCColors.AccentOrange
                        is ANCState.Calibrating -> ANCColors.AccentOrange
                        else -> ANCColors.TextSecondary
                    },
                    radius = 6.dp.toPx()
                )
            }
            Text(
                text = when (state) {
                    is ANCState.Idle -> "待机"
                    is ANCState.Calibrating -> "校准中"
                    is ANCState.Converging -> "收敛中"
                    is ANCState.Running -> "运行中"
                    is ANCState.Error -> "错误"
                    is ANCState.RequestingPermission -> "请求权限"
                },
                color = when (state) {
                    is ANCState.Running -> ANCColors.AccentGreen
                    is ANCState.Converging, is ANCState.Calibrating -> ANCColors.AccentOrange
                    is ANCState.Error -> ANCColors.AccentRed
                    else -> ANCColors.TextSecondary
                },
                fontWeight = androidx.compose.ui.text.font.FontWeight.Medium
            )
        }
    }
}

@Composable
private fun ErrorBanner(message: String, onDismiss: () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = ANCColors.AccentRed.copy(alpha = 0.15f)),
        shape = androidx.compose.foundation.shape.RoundedCornerShape(12.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(16.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(message, color = ANCColors.AccentRed, modifier = Modifier.weight(1f))
            TextButton(onClick = onDismiss) {
                Text("重试", color = ANCColors.AccentRed)
            }
        }
    }
}
