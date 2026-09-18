/**
 * JNI 接口 v4 — 对接 OboeEngine
 *
 * 实时音频处理全部在 C++ OboeEngine 内完成，JNI 只用于:
 *   1. 初始化/释放引擎
 *   2. 运行时参数传递 (模式/步长/增益/谐波数)
 *   3. 读取统计/频谱 (非实时路径)
 *   4. 触发次级路径校准 (阻塞, 由后台线程调用)
 */

#include "anc_engine.h"
#include "oboe_engine.h"
#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <vector>

#define LOG_TAG "ANC_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace anc;

static OboeEngine* g_engine = nullptr;

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_anc_app_engine_ANCEngine_nativeInit(
        JNIEnv* env, jobject thiz,
        jint sampleRate, jint filterLength, jint secondaryPathLength,
        jfloat stepSize, jfloat leakyFactor, jint blockSize, jint mode) {
    (void)env; (void)thiz;

    LOGI("JNI v4: Init sr=%d L=%d M=%d mu=%f leaky=%f block=%d mode=%d",
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

    const bool ok = g_engine->init(config);
    LOGI("JNI: Engine init %s", ok ? "SUCCESS" : "FAILED");
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_anc_app_engine_ANCEngine_nativeStart(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (!g_engine) return JNI_FALSE;
    const bool ok = g_engine->start();
    LOGI("JNI: Start %s", ok ? "SUCCESS" : "FAILED");
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeStop(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (g_engine) { g_engine->stop(); LOGI("JNI: Stopped"); }
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeEnable(JNIEnv* env, jobject thiz, jboolean enable) {
    (void)env; (void)thiz;
    if (g_engine) g_engine->enableANC(enable == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeSetStepSize(JNIEnv* env, jobject thiz, jfloat mu) {
    (void)env; (void)thiz;
    if (g_engine) g_engine->setStepSize(mu);
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeSetMode(JNIEnv* env, jobject thiz, jint mode) {
    (void)env; (void)thiz;
    if (g_engine) g_engine->setMode(mode);
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeSetOutputGain(JNIEnv* env, jobject thiz, jfloat gain) {
    (void)env; (void)thiz;
    if (g_engine) g_engine->setOutputGain(gain);
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeSetExternalSpeaker(JNIEnv* env, jobject thiz, jboolean external) {
    (void)env; (void)thiz;
    if (g_engine) g_engine->setExternalSpeaker(external == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeSetMaxHarmonics(JNIEnv* env, jobject thiz, jint n) {
    (void)env; (void)thiz;
    if (g_engine) g_engine->setMaxHarmonics(n);
}

/** 校准: 阻塞直到采集+估计完成 (由后台线程调用) */
JNIEXPORT jboolean JNICALL
Java_com_anc_app_engine_ANCEngine_nativeCalibrate(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (!g_engine) return JNI_FALSE;
    g_engine->startCalibration();
    return g_engine->getStats().isCalibrated ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_anc_app_engine_ANCEngine_nativeIsCalibrating(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (!g_engine) return JNI_FALSE;
    return g_engine->isCalibrating() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_anc_app_engine_ANCEngine_nativeIsRunning(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (!g_engine) return JNI_FALSE;
    return g_engine->isRunning() ? JNI_TRUE : JNI_FALSE;
}

/** 统计: [nr, procUs, refPow, errPow, outPow, filterNorm, delayMs, tonalHz, tonalCount, converged, calibrated, frames] */
JNIEXPORT jfloatArray JNICALL
Java_com_anc_app_engine_ANCEngine_nativeGetStats(JNIEnv* env, jobject thiz) {
    (void)thiz;
    if (!g_engine) return nullptr;

    const ANCStats s = g_engine->getStats();
    const float data[12] = {
        s.noiseReductionDb,
        s.processingTimeUs,
        s.referencePower,
        s.errorPower,
        s.outputPower,
        s.filterNorm,
        s.loopDelayMs,
        s.tonalHz,
        static_cast<float>(s.tonalCount),
        s.isConverged ? 1.0f : 0.0f,
        s.isCalibrated ? 1.0f : 0.0f,
        static_cast<float>(s.frameCount)
    };

    jfloatArray result = env->NewFloatArray(12);
    if (result) env->SetFloatArrayRegion(result, 0, 12, data);
    return result;
}

JNIEXPORT jfloatArray JNICALL
Java_com_anc_app_engine_ANCEngine_nativeGetSpectrum(JNIEnv* env, jobject thiz, jint type) {
    (void)thiz;
    if (!g_engine) return nullptr;

    const int bins = 256;
    std::vector<float> spectrum(bins, -100.0f);
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

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeReset(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (g_engine) g_engine->resetProcessing();
}

JNIEXPORT void JNICALL
Java_com_anc_app_engine_ANCEngine_nativeRelease(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (g_engine) {
        delete g_engine;
        g_engine = nullptr;
        LOGI("JNI: Engine released");
    }
}

} // extern "C"
