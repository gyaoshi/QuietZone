package com.anc.app.utils

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build

/**
 * 音频设备辅助工具
 *
 * 检测外接音频设备 (有线音箱/USB-C/蓝牙)
 */
object AudioDeviceHelper {

    enum class OutputDeviceType {
        BUILTIN_SPEAKER,   // 手机内置扬声器
        WIRED_HEADSET,     // 3.5mm 有线耳机/音箱
        USB_AUDIO,         // USB-C 音频设备
        BLUETOOTH_A2DP,    // 蓝牙 A2DP (延迟大, 不适合ANC)
        BLUETOOTH_LE,      // 蓝牙 LE Audio (可能可用)
        UNKNOWN
    }

    /** 优先级: 有线/USB > BLE > A2DP > 内置扬声器 */
    private val DEVICE_PRIORITY = mapOf(
        AudioDeviceInfo.TYPE_WIRED_HEADSET to 1,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES to 1,
        AudioDeviceInfo.TYPE_USB_DEVICE to 1,
        AudioDeviceInfo.TYPE_USB_ACCESSORY to 1,
        AudioDeviceInfo.TYPE_USB_HEADSET to 1,
        AudioDeviceInfo.TYPE_BLE_SPEAKER to 2,
        AudioDeviceInfo.TYPE_BLE_HEADSET to 2,
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP to 3,
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO to 3,
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER to 10
    )

    /** 获取当前音频输出设备类型 (按优先级选择最佳设备) */
    fun getCurrentOutputType(context: Context): OutputDeviceType {
        val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
            ?: return OutputDeviceType.UNKNOWN

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            val devices = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            // 按优先级排序, 取最高优先级设备
            val bestDevice = devices.minByOrNull { DEVICE_PRIORITY[it.type] ?: 99 }
            if (bestDevice != null) {
                return when (bestDevice.type) {
                    AudioDeviceInfo.TYPE_WIRED_HEADSET,
                    AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> OutputDeviceType.WIRED_HEADSET

                    AudioDeviceInfo.TYPE_USB_DEVICE,
                    AudioDeviceInfo.TYPE_USB_ACCESSORY,
                    AudioDeviceInfo.TYPE_USB_HEADSET -> OutputDeviceType.USB_AUDIO

                    AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                    AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> OutputDeviceType.BLUETOOTH_A2DP

                    AudioDeviceInfo.TYPE_BLE_SPEAKER,
                    AudioDeviceInfo.TYPE_BLE_HEADSET -> OutputDeviceType.BLUETOOTH_LE

                    AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> OutputDeviceType.BUILTIN_SPEAKER

                    else -> OutputDeviceType.UNKNOWN
                }
            }
        }

        return OutputDeviceType.BUILTIN_SPEAKER
    }

    /** 是否是低延迟输出设备 (适合ANC) */
    fun isLowLatencyDevice(context: Context): Boolean {
        return when (getCurrentOutputType(context)) {
            OutputDeviceType.WIRED_HEADSET,
            OutputDeviceType.USB_AUDIO -> true
            else -> false
        }
    }

    /** 是否是蓝牙设备 (延迟大, 不适合ANC) */
    fun isBluetoothDevice(context: Context): Boolean {
        return when (getCurrentOutputType(context)) {
            OutputDeviceType.BLUETOOTH_A2DP,
            OutputDeviceType.BLUETOOTH_LE -> true
            else -> false
        }
    }

    /** 获取设备描述文字 */
    fun getDeviceDescription(context: Context): String {
        return when (getCurrentOutputType(context)) {
            OutputDeviceType.BUILTIN_SPEAKER -> "手机扬声器 (低频输出有限)"
            OutputDeviceType.WIRED_HEADSET -> "有线设备 (推荐, 延迟最低)"
            OutputDeviceType.USB_AUDIO -> "USB音频 (推荐, 延迟最低)"
            OutputDeviceType.BLUETOOTH_A2DP -> "蓝牙设备 (延迟过大, 降噪效果受限)"
            OutputDeviceType.BLUETOOTH_LE -> "LE Audio (延迟较低)"
            OutputDeviceType.UNKNOWN -> "未知设备"
        }
    }
}
