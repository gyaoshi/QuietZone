package com.anc.app.engine

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.anc.app.ui.MainActivity

/**
 * ANC 前台服务 v3
 *
 * 使用 ANCEngine 单例, 不再创建独立引擎实例。
 * 仅负责前台通知生命周期, 引擎操作由 ViewModel 通过单例控制。
 */
class ANCService : Service() {

    companion object {
        const val CHANNEL_ID = "anc_service_channel"
        const val NOTIFICATION_ID = 1001

        const val ACTION_START = "com.anc.app.ACTION_START"
        const val ACTION_STOP = "com.anc.app.ACTION_STOP"
        const val ACTION_UPDATE_CONFIG = "com.anc.app.ACTION_UPDATE_CONFIG"

        const val EXTRA_MODE = "mode"
        const val EXTRA_STEP_SIZE = "step_size"
        const val EXTRA_OUTPUT_GAIN = "output_gain"
        const val EXTRA_EXTERNAL_SPEAKER = "external_speaker"
    }

    private val engine = ANCEngine.getInstance()
    private var isRunning = false

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                startForeground(NOTIFICATION_ID, createNotification("启动中..."))
                isRunning = true
                startStatsUpdater()
            }
            ACTION_STOP -> {
                isRunning = false
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                    stopForeground(STOP_FOREGROUND_REMOVE)
                } else {
                    @Suppress("DEPRECATION")
                    stopForeground(true)
                }
                stopSelf()
            }
            ACTION_UPDATE_CONFIG -> {
                updateConfig(intent)
            }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        isRunning = false
        super.onDestroy()
    }

    private fun startStatsUpdater() {
        Thread({
            while (isRunning && engine.isRunning()) {
                val stats = engine.getStats()
                if (stats != null) {
                    val msg = String.format("降噪 %.1f dB | 延迟 %.0f μs",
                        stats.noiseReductionDb, stats.processingTimeUs)
                    updateNotification(msg)
                }
                try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
            }
        }, "ANC-StatsUpdater").start()
    }

    private fun updateConfig(intent: Intent) {
        if (!isRunning) return
        if (intent.hasExtra(EXTRA_MODE)) {
            engine.setMode(intent.getIntExtra(EXTRA_MODE, 2))
        }
        if (intent.hasExtra(EXTRA_STEP_SIZE)) {
            engine.setStepSize(intent.getFloatExtra(EXTRA_STEP_SIZE, 0.01f))
        }
        if (intent.hasExtra(EXTRA_OUTPUT_GAIN)) {
            engine.setOutputGain(intent.getFloatExtra(EXTRA_OUTPUT_GAIN, 1.0f))
        }
        if (intent.hasExtra(EXTRA_EXTERNAL_SPEAKER)) {
            val external = intent.getBooleanExtra(EXTRA_EXTERNAL_SPEAKER, false)
            engine.setExternalSpeaker(external)
            if (external) {
                engine.calibrate()
            }
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID, "ANC降噪服务", NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "主动降噪后台运行"
                setShowBadge(false)
            }
            getSystemService(NotificationManager::class.java)
                .createNotificationChannel(channel)
        }
    }

    private fun createNotification(text: String): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val stopIntent = PendingIntent.getService(
            this, 1,
            Intent(this, ANCService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("QuietZone 主动降噪")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .setContentIntent(pendingIntent)
            .addAction(android.R.drawable.ic_media_pause, "停止", stopIntent)
            .setOngoing(true)
            .setSilent(true)
            .build()
    }

    private fun updateNotification(text: String) {
        val notification = createNotification(text)
        val manager = getSystemService(NotificationManager::class.java)
        manager.notify(NOTIFICATION_ID, notification)
    }
}
