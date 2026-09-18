package com.anc.app.ui.components

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.anc.app.engine.ANCState
import com.anc.app.engine.ANCStats

// ============================================================
// 颜色常量
// ============================================================
object ANCColors {
    val DarkBg = Color(0xFF0A0E17)
    val CardBg = Color(0xFF141B2D)
    val CardBgLight = Color(0xFF1A2340)
    val Accent = Color(0xFF00E5FF)
    val AccentGreen = Color(0xFF00E676)
    val AccentOrange = Color(0xFFFF9100)
    val AccentRed = Color(0xFFFF1744)
    val TextPrimary = Color(0xFFFFFFFF)
    val TextSecondary = Color(0xFFB0BEC5)
    val WaveRef = Color(0xFF42A5F5)
    val WaveErr = Color(0xFF66BB6A)
}

// ============================================================
// 主控按钮
// ============================================================
@Composable
fun MainControlButton(
    state: ANCState,
    hasPermission: Boolean,
    onClick: () -> Unit
) {
    val isRunning = state is ANCState.Running || state is ANCState.Converging
    val isCalibrating = state is ANCState.Calibrating

    val buttonColor by animateColorAsState(
        targetValue = when {
            !hasPermission -> Color.Gray
            isCalibrating -> ANCColors.AccentOrange
            isRunning -> ANCColors.AccentRed
            else -> ANCColors.Accent
        },
        label = "buttonColor"
    )

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = ANCColors.CardBg),
        shape = RoundedCornerShape(24.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(32.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            IconButton(
                onClick = onClick,
                enabled = !isCalibrating,
                modifier = Modifier
                    .size(120.dp)
                    .clip(CircleShape)
                    .background(buttonColor)
            ) {
                if (isCalibrating) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(40.dp),
                        color = Color.White,
                        strokeWidth = 3.dp
                    )
                } else {
                    Icon(
                        imageVector = if (isRunning) Icons.Default.Stop else Icons.Default.PlayArrow,
                        contentDescription = if (isRunning) "停止降噪" else "开始降噪",
                        tint = Color.White,
                        modifier = Modifier.size(56.dp)
                    )
                }
            }

            Spacer(modifier = Modifier.height(16.dp))

            Text(
                text = when {
                    !hasPermission -> "需要麦克风权限"
                    isCalibrating -> "正在校准，请保持安静..."
                    isRunning -> "点击停止降噪"
                    else -> "点击开始降噪"
                },
                color = ANCColors.TextSecondary,
                style = MaterialTheme.typography.bodyMedium
            )
        }
    }
}

// ============================================================
// 统计面板
// ============================================================
@Composable
fun StatsPanel(stats: ANCStats) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = ANCColors.CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp)
        ) {
            Text(
                text = "实时统计",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
                color = ANCColors.TextPrimary
            )

            Spacer(modifier = Modifier.height(12.dp))

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceEvenly
            ) {
                StatItem(
                    // "实测": 由麦克风本底 P0 与当前残差 P(e) 实测对比得到,
                    // 不再用次级路径模型推算, 所以这个数可以直接信。
                    label = "降噪量(实测)",
                    value = String.format("%.1f dB", stats.noiseReductionDb),
                    color = when {
                        stats.noiseReductionDb > 10 -> ANCColors.AccentGreen
                        stats.noiseReductionDb > 3 -> ANCColors.AccentOrange
                        else -> ANCColors.AccentRed
                    }
                )
                StatItem(
                    label = "处理延迟",
                    value = String.format("%.0f μs", stats.processingTimeUs),
                    color = if (stats.processingTimeUs < 1000) ANCColors.AccentGreen
                            else ANCColors.AccentOrange
                )
                StatItem(
                    label = "收敛状态",
                    value = if (stats.isConverged) "已收敛" else "收敛中",
                    color = if (stats.isConverged) ANCColors.AccentGreen else ANCColors.AccentOrange
                )
            }

            Spacer(modifier = Modifier.height(10.dp))

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceEvenly
            ) {
                StatItem(
                    label = "噪声基频",
                    value = if (stats.tonalCount > 0) String.format("%.0f Hz", stats.tonalHz) else "—",
                    color = if (stats.tonalCount > 0) ANCColors.Accent else ANCColors.TextSecondary
                )
                StatItem(
                    label = "谐波数",
                    value = if (stats.tonalCount > 0) "${stats.tonalCount}" else "—",
                    color = if (stats.tonalCount > 0) ANCColors.Accent else ANCColors.TextSecondary
                )
                StatItem(
                    label = "回环延迟",
                    value = String.format("%.1f ms", stats.loopDelayMs),
                    color = if (stats.isCalibrated) ANCColors.AccentGreen else ANCColors.AccentOrange
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            LinearProgressIndicator(
                progress = { (stats.noiseReductionDb / 30f).coerceIn(0f, 1f) },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(8.dp)
                    .clip(RoundedCornerShape(4.dp)),
                color = ANCColors.AccentGreen,
                trackColor = ANCColors.CardBgLight
            )

            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = if (stats.isCalibrated) "次级路径已校准"
                       else "次级路径未校准 — 宽带分支不生效",
                fontSize = 11.sp,
                color = if (stats.isCalibrated) ANCColors.TextSecondary else ANCColors.AccentOrange
            )
        }
    }
}

@Composable
private fun StatItem(label: String, value: String, color: Color) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(text = value, fontWeight = FontWeight.Bold, fontSize = 18.sp, color = color)
        Text(text = label, fontSize = 12.sp, color = ANCColors.TextSecondary)
    }
}

// ============================================================
// 频谱图
// ============================================================
@Composable
fun SpectrumView(
    refSpectrum: FloatArray,
    errSpectrum: FloatArray
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = ANCColors.CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    text = "实时频谱",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    color = ANCColors.TextPrimary
                )
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    LegendDot("参考", ANCColors.WaveRef)
                    LegendDot("残差", ANCColors.WaveErr)
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            Canvas(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(150.dp)
                    .clip(RoundedCornerShape(8.dp))
                    .background(ANCColors.DarkBg)
            ) {
                val width = size.width
                val height = size.height
                val pad = 4.dp.toPx()

                // 网格
                for (i in 0..4) {
                    val y = pad + (height - 2 * pad) * i / 4
                    drawLine(Color.White.copy(alpha = 0.08f), Offset(pad, y), Offset(width - pad, y))
                }

                // 参考频谱
                drawSpectrumCurve(refSpectrum, ANCColors.WaveRef, width, height, pad)
                // 残差频谱
                drawSpectrumCurve(errSpectrum, ANCColors.WaveErr, width, height, pad)
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text("0Hz", fontSize = 10.sp, color = ANCColors.TextSecondary)
                Text("6kHz", fontSize = 10.sp, color = ANCColors.TextSecondary)
                Text("12kHz", fontSize = 10.sp, color = ANCColors.TextSecondary)
                Text("24kHz", fontSize = 10.sp, color = ANCColors.TextSecondary)
            }
        }
    }
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawSpectrumCurve(
    spectrum: FloatArray,
    color: Color,
    width: Float,
    height: Float,
    pad: Float
) {
    val path = Path()
    val len = spectrum.size.coerceAtMost(128)
    for (i in 0 until len) {
        val x = pad + (width - 2 * pad) * i / len
        val normalized = (spectrum[i] + 80f) / 80f
        val y = height - pad - (height - 2 * pad) * normalized.coerceIn(0f, 1f)
        if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
    }
    drawPath(path, color, style = Stroke(width = 2f))
}

@Composable
private fun LegendDot(label: String, color: Color) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Canvas(modifier = Modifier.size(8.dp)) { drawCircle(color) }
        Spacer(modifier = Modifier.width(4.dp))
        Text(label, fontSize = 11.sp, color = ANCColors.TextSecondary)
    }
}

// ============================================================
// 模式选择器
// ============================================================
@Composable
fun ModeSelector(
    selectedMode: Int,
    onModeChange: (Int) -> Unit,
    enabled: Boolean
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = ANCColors.CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp)
        ) {
            Text(
                text = "降噪模式",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
                color = ANCColors.TextPrimary
            )
            Spacer(modifier = Modifier.height(12.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                ModeChip("窄带", "Tonal ★", selectedMode == 0, enabled, { onModeChange(0) }, Modifier.weight(1f))
                ModeChip("宽带", "Broadband", selectedMode == 1, enabled, { onModeChange(1) }, Modifier.weight(1f))
                ModeChip("混合", "Hybrid", selectedMode == 2, enabled, { onModeChange(2) }, Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun ModeChip(
    label: String, subtitle: String,
    selected: Boolean, enabled: Boolean,
    onClick: () -> Unit, modifier: Modifier = Modifier
) {
    Surface(
        onClick = { if (enabled) onClick() },
        modifier = modifier,
        shape = RoundedCornerShape(12.dp),
        color = if (selected) ANCColors.Accent.copy(alpha = 0.2f) else ANCColors.CardBgLight,
        enabled = enabled
    ) {
        Column(
            modifier = Modifier.padding(12.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = label,
                fontWeight = FontWeight.Bold,
                color = if (selected) ANCColors.Accent else ANCColors.TextSecondary
            )
            Text(text = subtitle, fontSize = 10.sp, color = ANCColors.TextSecondary)
        }
    }
}

// ============================================================
// 参数面板
// ============================================================
@Composable
fun ParameterPanel(
    stepSize: Float, onStepSizeChange: (Float) -> Unit,
    outputGain: Float, onOutputGainChange: (Float) -> Unit,
    externalSpeaker: Boolean, onExternalSpeakerChange: (Boolean) -> Unit,
    enabled: Boolean
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = ANCColors.CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp)
        ) {
            Text(
                text = "参数调节",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
                color = ANCColors.TextPrimary
            )

            Spacer(modifier = Modifier.height(16.dp))

            // 步长
            Text(
                text = "步长 μ: ${String.format("%.4f", stepSize)}",
                color = ANCColors.TextSecondary, fontSize = 13.sp
            )
            Slider(
                value = stepSize,
                onValueChange = { if (enabled) onStepSizeChange(it) },
                valueRange = 0.001f..0.1f,
                enabled = enabled,
                colors = SliderDefaults.colors(thumbColor = ANCColors.Accent, activeTrackColor = ANCColors.Accent)
            )
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("慢(稳定)", fontSize = 11.sp, color = ANCColors.TextSecondary)
                Text("快(不稳定)", fontSize = 11.sp, color = ANCColors.TextSecondary)
            }

            Spacer(modifier = Modifier.height(12.dp))

            // 输出增益
            Text(
                text = "输出增益: ${String.format("%.1f", outputGain)}x",
                color = ANCColors.TextSecondary, fontSize = 13.sp
            )
            Slider(
                value = outputGain,
                onValueChange = { if (enabled) onOutputGainChange(it) },
                valueRange = 0.5f..4.0f,
                enabled = enabled,
                colors = SliderDefaults.colors(thumbColor = ANCColors.AccentOrange, activeTrackColor = ANCColors.AccentOrange)
            )

            Spacer(modifier = Modifier.height(16.dp))

            // 外接音箱
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("外接音箱模式", fontWeight = FontWeight.Medium, color = ANCColors.TextPrimary)
                    Text("增大输出功率，适合外接有源音箱", fontSize = 12.sp, color = ANCColors.TextSecondary)
                }
                Switch(
                    checked = externalSpeaker,
                    onCheckedChange = { if (enabled) onExternalSpeakerChange(it) },
                    enabled = enabled,
                    colors = SwitchDefaults.colors(checkedTrackColor = ANCColors.Accent)
                )
            }
        }
    }
}

// ============================================================
// 算法信息
// ============================================================
@Composable
fun AlgorithmInfoCard() {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = ANCColors.CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(16.dp)
        ) {
            Text("算法原理", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold, color = ANCColors.TextPrimary)
            Spacer(modifier = Modifier.height(12.dp))
            InfoRow("核心算法", "归一化 FxLMS")
            InfoRow("窄带分支", "谐波抵消器 (6 阶)")
            InfoRow("宽带分支", "IMC 反馈滤波 (256 阶)")
            InfoRow("次级路径", "扫频探测 + 匹配滤波")
            InfoRow("加速", "ARM NEON SIMD")
            InfoRow("音频引擎", "Oboe (AAudio)")
            InfoRow("采样率", "48kHz")
            InfoRow("有效带宽", "30Hz - 600Hz")
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                "y(n) = wᵀ·x̂(n),  w(n+1) = (1-μδ)w(n) + μ·x̂(n)·e(n)",
                fontSize = 11.sp, color = ANCColors.TextSecondary
            )
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                "单麦克风无法做宽带前馈，故窄带分支用内部合成参考（免疫回环延迟），宽带分支用 IMC 反馈。",
                fontSize = 11.sp, color = ANCColors.TextSecondary
            )
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, fontSize = 13.sp, color = ANCColors.TextSecondary)
        Text(value, fontSize = 13.sp, color = ANCColors.Accent, fontWeight = FontWeight.Medium)
    }
}
