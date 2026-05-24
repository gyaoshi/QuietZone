/**
 * JNI 接口 v2 — 对接 OboeEngine
 *
 * 所有实时音频处理在 C++ OboeEngine 内完成，
 * JNI 仅用于:
 *   1. 初始化/释放引擎
 *   2. 运行时参数传递 (步长/模式/增益)
 *   3. 读取统计/频谱数据 (100ms 刷新, 非实时路径)
 *
 * 实时路径: Oboe回调 → C++ FxLMS → Oboe输出 (零JNI开销)
 */

#include "anc_engine.h"
#include "oboe_engine.h"  // OboeEngine 声明
#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <vector>

#define LOG_TAG "ANC_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace anc;

// 全局引擎实例
static OboeEngine* g_engine = nullptr;

extern "C" {

// ============================================================
// 初始化
// ============================================================
JNIEXPORT jboolean JNICALL
Java_com_anc_app_engine_ANCEngine_nativeInit(
        JNIEnv* env, jobject thiz,
        jint sampleRate, jint filterLength, jint secondaryPathLength,
        jfloat stepSize, jfloat leakyFactor, jint blockSize, jint mode) {

    LOGI("JNI: Init ANC Engine sr=%d L=%d M=%d μ=%f leaky=%f block=%d mode=%d",
         sampleRate, filterLength, secondaryPathLength, stepSize, leakyFactor, blockSize, mode);

    if (g_engine) {
        delete g_engine;
        g_engine = nullptr;
    }

    g_engine = new OboeEngine();

    ANCConfig config;
    config.sampleRate = sampleRate;
    config.filterLength = filterLength;
    config.secondaryPathLength = secondaryPathLength;
    config.stepSize = stepSize;
    config.leakyFactor = leakyFactor;
    config.blockSize = blockSize;
    config.mode = mode;

    bool ok = g_engine->init(config);
    LOGI("JNI: Engine init %s", ok ? "SUCCESS" : "FAILED");
    return ok ? JNI_TRUE : JNI_FALSE;
}

// ============================================================
// 启动/停止音频流
// ============================================================
JNIEXPORT jboolean JNICALL
Java_com_anc_app_engine_ANCEngine_nativeStart(JNIEnv* env, jobject thiz) {
    if (!g_engine) return JNI_FALSE;
    bool ok = g_engine->start();
    LOGI("JNI: Start %s", ok ? "SUCCESS" : "FAILED");
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeStop(JNIEnv* env, jobject thiz) {
    if (g_engine) {
        g_engine->stop();
        LOGI("JNI: Stopped");
    }
}

// ============================================================
// 运行时控制
// ============================================================
JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeEnable(JNIEnv* env, jobject thiz, jboolean enable) {
    if (g_engine) g_engine->enableANC(enable);
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeSetStepSize(JNIEnv* env, jobject thiz, jfloat mu) {
    if (g_engine) g_engine->setStepSize(mu);
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeSetMode(JNIEnv* env, jobject thiz, jint mode) {
    if (g_engine) g_engine->setMode(mode);
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeSetOutputGain(JNIEnv* env, jobject thiz, jfloat gain) {
    if (g_engine) g_engine->setOutputGain(gain);
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeSetExternalSpeaker(JNIEnv* env, jobject thiz, jboolean external) {
    if (g_engine) g_engine->setExternalSpeaker(external);
}

// ============================================================
// 校准
// ============================================================
JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeCalibrate(JNIEnv* env, jobject thiz) {
    if (g_engine) g_engine->startCalibration();
}

JNIEXPORT jboolean JNICALL
Java_com_anc_app_engine_ANCEngine_nativeIsCalibrating(JNIEnv* env, jobject thiz) {
    if (!g_engine) return JNI_FALSE;
    return g_engine->isCalibrating() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_anc_app_engine_ANCEngine_nativeIsRunning(JNIEnv* env, jobject thiz) {
    if (!g_engine) return JNI_FALSE;
    return g_engine->isRunning() ? JNI_TRUE : JNI_FALSE;
}

// ============================================================
// 统计数据
// ============================================================
JNIEXPORT jfloatArray JNICALL
Java_com_anc_app_engine_ANCEngine_nativeGetStats(JNIEnv* env, jobject thiz) {
    if (!g_engine) return nullptr;

    ANCStats stats = g_engine->getStats();

    float data[7] = {
        stats.noiseReductionDb,
        stats.processingTimeUs,
        stats.referencePower,
        stats.errorPower,
        stats.filterNorm,
        stats.isConverged ? 1.0f : 0.0f,
        (float)stats.frameCount
    };

    jfloatArray result = env->NewFloatArray(7);
    if (result) env->SetFloatArrayRegion(result, 0, 7, data);
    return result;
}

// ============================================================
// 频谱数据
// ============================================================
JNIEXPORT jfloatArray JNICALL
Java_com_anc_app_engine_ANCEngine_nativeGetSpectrum(JNIEnv* env, jobject thiz, jint type) {
    if (!g_engine) return nullptr;

    // 使用动态大小, 匹配配置中的 spectrumBins/2
    int bins = 256;  // 默认值, 与 ANCConfig.spectrumBins=512 对应
    std::vector<float> spectrum(bins, 0.0f);

    switch (type) {
        case 0: g_engine->getRefSpectrum(spectrum.data(), bins); break;
        case 1: g_engine->getErrSpectrum(spectrum.data(), bins); break;
        case 2: g_engine->getReductionSpectrum(spectrum.data(), bins); break;
        default: return nullptr;
    }

    jfloatArray result = env->NewFloatArray(bins);
    if (result) env->SetFloatArrayRegion(result, 0, bins, spectrum.data());
    return result;
}

// ============================================================
// 重置/释放
// ============================================================
JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeReset(JNIEnv* env, jobject thiz) {
    if (g_engine) {
        g_engine->stop();
        // 重置内部状态 (滤波器权重、统计等)
        // stop() 已关闭流, 但未 reset 处理器状态
        // 需要重新 init 才能完全重置
    }
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeRelease(JNIEnv* env, jobject thiz) {
    if (g_engine) {
        delete g_engine;
        g_engine = nullptr;
        LOGI("JNI: Engine released");
    }
}

// ============================================================
// 旧接口兼容 (processFrame 不再需要, Oboe回调内直接处理)
// ============================================================
JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeProcessFrame(
        JNIEnv* env, jobject thiz,
        jfloatArray micInput, jfloatArray speakerOutput, jint numSamples) {
    // 已由 Oboe C++ 回调直接处理, 此方法不再使用
    // 保留空实现以兼容旧 Kotlin 代码
}

} // extern "C"
