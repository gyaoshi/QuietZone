/**
 * QuietZone ANC 单元测试 (v4)
 *
 * v4 相比 v3 的测试改动:
 *   v3 的主收敛用例只断言 frameCount > 0，注释还写"只要 NR>0 就说明在工作"，
 *   而且该用例的参数让 Ŝ 全零 —— 权重一次都不会更新，测试照样 PASS。
 *   "9/9 通过"没有任何信息量。
 *
 *   v4 的做法: 在测试里搭一个虚拟次级路径，跑真正的闭环，
 *   断言端到端降噪量、输出非静音、以及未校准时不发散。
 *
 * 编译 (桌面端):
 *   g++ -std=c++17 -O2 -I app/src/main/cpp \
 *       tests/anc_unit_test.cpp \
 *       app/src/main/cpp/anc_core.cpp \
 *       app/src/main/cpp/audio_processor.cpp \
 *       app/src/main/cpp/spectrum_analyzer.cpp -o anc_unit_test
 */

#include "anc_engine.h"
#include "spectrum_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace anc;

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", msg); g_fail++; return false; } \
} while (0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
    float _a = (a), _b = (b), _tol = (tol); \
    if (std::fabs(_a - _b) > _tol) { \
        printf("  [FAIL] %s (got %.6f, expected %.6f, tol %.6f)\n", msg, _a, _b, _tol); \
        g_fail++; return false; } \
} while (0)

#define ASSERT_GT(a, b, msg) do { \
    if (!((a) > (b))) { printf("  [FAIL] %s (%.4f > %.4f)\n", msg, (float)(a), (float)(b)); g_fail++; return false; } \
} while (0)

#define RUN_TEST(fn) do { \
    printf(">> %s\n", #fn); \
    if (fn()) { printf("   PASS\n"); g_pass++; } \
} while (0)

// ============================================================
// 虚拟次级路径 (与音频回调里一致的"延迟余项"块卷积)
//
// 说明: 本类的 pending/feed 拆分依赖一个前提 ——
//   次级路径的纯延迟 D >= 块长 B。
// 此时本块第 i 个输出的来自"块内更早输出"的耦合项 h[k]*y(i-k) (k<=i<n<=B<=D)
// 因 h[0..D-1]=0 而恒为零，于是本块输出只由更早各块的 y 决定，
// pending() 就能在任何控制器运行之前精确算出。
// 测试里 D>=288、B<=256，满足该前提。
// ============================================================
class VirtualPlant {
public:
    VirtualPlant(const std::vector<float>& h, int block)
        : h_(h), M_(static_cast<int>(h.size())), B_(block) {}

    /** 取本块对象输出 (只依赖更早各块的 y) */
    void pending(float* out, int n) {
        const long n0 = static_cast<long>(yhist_.size());
        for (int i = 0; i < n; i++) {
            double acc = 0.0;
            for (int k = i + 1; k < M_; k++) {          // k > i ⇒ y(n0+i-k) 属于历史
                const long idx = n0 + i - k;
                if (idx >= 0) acc += static_cast<double>(h_[static_cast<size_t>(k)]) *
                                     yhist_[static_cast<size_t>(idx)];
            }
            out[i] = static_cast<float>(acc);
        }
    }

    /** 把本块产生的 y 记入历史 */
    void feed(const float* y, int n) {
        for (int i = 0; i < n; i++) yhist_.push_back(y[i]);
    }

    int length() const { return M_; }

private:
    std::vector<float> h_;
    std::vector<float> yhist_;
    int M_;
    int B_;
};

// 构造一个"手机式"次级路径: 纯延迟 D + 二阶低通滚降
static std::vector<float> makeSecondaryPath(int D, int tailLen = 256, float fc = 400.0f,
                                            float fs = 48000.0f) {
    std::vector<float> h(static_cast<size_t>(D + tailLen), 0.0f);
    // 二阶低通的冲激响应 (双实极点近似)
    const float a = std::exp(-2.0f * static_cast<float>(M_PI) * fc / fs);
    float v = 1.0f;
    float sum = 0.0f;
    std::vector<float> tail(static_cast<size_t>(tailLen), 0.0f);
    for (int i = 0; i < tailLen; i++) {
        tail[static_cast<size_t>(i)] = v;
        v *= a;
        sum += tail[static_cast<size_t>(i)];
    }
    for (int i = 0; i < tailLen; i++) {
        h[static_cast<size_t>(D + i)] = tail[static_cast<size_t>(i)] / (sum > 1e-9f ? sum : 1.0f);
    }
    return h;
}

static void addTone(float* buf, int n, float f, float amp, float fs) {
    for (int i = 0; i < n; i++) {
        buf[i] += amp * std::sin(2.0f * static_cast<float>(M_PI) * f * i / fs);
    }
}

static float rms(const float* x, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += static_cast<double>(x[i]) * x[i];
    return static_cast<float>(std::sqrt(s / n));
}

// ============================================================
// 测试 1: SecondaryPath 的延迟检测与滤波
// ============================================================
static bool test_secondary_path_filtering() {
    const int D = 64;
    auto h = makeSecondaryPath(D);
    SecondaryPath sp;
    sp.setImpulseResponse(h.data(), static_cast<int>(h.size()), 48000.0f);

    ASSERT_TRUE(sp.isCalibrated(), "should be calibrated after setImpulseResponse");
    ASSERT_NEAR(static_cast<float>(sp.delaySamples()), static_cast<float>(D), 8.0f,
                "detected delay should be close to D");

    // 用同一模型滤波: y 应为 x 经 h 的结果 (与直接卷积对比)
    SecPathLine line;
    line.resize(sp.ringSize());
    const int N = 400;
    std::vector<float> x(static_cast<size_t>(N), 0.0f);
    for (int i = 0; i < N; i++) x[static_cast<size_t>(i)] = std::sin(0.05f * i);

    std::vector<float> got(static_cast<size_t>(N), 0.0f);
    for (int i = 0; i < N; i++) got[static_cast<size_t>(i)] = sp.filter(line, x[static_cast<size_t>(i)]);

    float maxErr = 0.0f;
    for (int n = D + 8; n < N; n++) {
        double ref = 0.0;
        for (int k = 0; k < static_cast<int>(h.size()); k++) {
            if (n - k >= 0) ref += h[static_cast<size_t>(k)] * x[static_cast<size_t>(n - k)];
        }
        maxErr = std::max(maxErr, std::fabs(static_cast<float>(ref) - got[static_cast<size_t>(n)]));
    }
    printf("     max |Ŝ*x - h*x| = %.3e\n", maxErr);
    ASSERT_TRUE(maxErr < 1e-4f, "sparse FIR must equal direct convolution");

    // 复频响与直接 DFT 对比
    float mag = 0.0f, ph = 0.0f;
    sp.responseAt(300.0f, mag, ph);
    double re = 0.0, im = 0.0;
    for (size_t k = 0; k < h.size(); k++) {
        re += h[k] * std::cos(2.0 * M_PI * 300.0 * k / 48000.0);
        im -= h[k] * std::sin(2.0 * M_PI * 300.0 * k / 48000.0);
    }
    const float refMag = static_cast<float>(std::sqrt(re * re + im * im));
    printf("     |S(300Hz)| sparse=%.6f direct=%.6f\n", mag, refMag);
    ASSERT_NEAR(mag, refMag, 1e-3f, "responseAt must match direct DFT");
    return true;
}

// ============================================================
// 测试 2: 自适应滤波器能辨识已知 FIR
// ============================================================
static bool test_adaptive_filter_system_id() {
    const int L = 32;
    AdaptiveFilter af;
    af.init(L, 0.02f, 1.0f);          // 无泄漏

    // 目标系统: 3 抽头
    const float target[3] = {0.5f, -0.3f, 0.2f};

    std::vector<float> xhist;
    uint32_t seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return (static_cast<float>((seed >> 8) & 0xFFFF) / 32768.0f) - 1.0f;
    };

    SecPathLine dummy;   // 这里直接用 x 作为滤波参考 (S = 1)
    for (int n = 0; n < 40000; n++) {
        const float x = rnd();
        xhist.push_back(x);
        const float y = af.process(x);
        af.pushFilteredRef(x);       // Ŝ = 1
        float d = 0.0f;
        for (int k = 0; k < 3; k++) {
            if (n - k >= 0) d += target[k] * xhist[static_cast<size_t>(n - k)];
        }
        const float e = d - y * 0.0f;   // 这里只做辨识: 误差 = 目标输出 - 滤波器输出
        (void)e;
        // 系统辨识: 误差 = target*x - w*x
        af.update(d - y, 0.02f / 0.35f);
    }

    const float* w = af.weights();
    printf("     w[0..2] = %.3f %.3f %.3f (expect 0.5 -0.3 0.2)\n", w[0], w[1], w[2]);
    ASSERT_NEAR(w[0], 0.5f, 0.08f, "w[0] should converge to 0.5");
    ASSERT_NEAR(w[1], -0.3f, 0.08f, "w[1] should converge to -0.3");
    ASSERT_NEAR(w[2], 0.2f, 0.08f, "w[2] should converge to 0.2");
    return true;
}

// ============================================================
// 测试 3: 谐波抵消器在真实延迟环路下收敛 (核心)
// ============================================================
static bool test_harmonic_canceller_closed_loop() {
    const float fs = 48000.0f;
    const int D = 288;                 // 6 ms 回环延迟
    const int block = 128;
    auto h = makeSecondaryPath(D);

    SecondaryPath sp;
    sp.setImpulseResponse(h.data(), static_cast<int>(h.size()), fs);

    HarmonicCanceller hc;
    hc.init(fs, 6, 30.0f, 600.0f, 0.002f);
    hc.setSecondaryPath(&sp);
    const int nh = hc.lockTo(120.0f);
    ASSERT_TRUE(nh >= 3, "should lock at least 3 harmonics of 120Hz");
    printf("     locked harmonics: %d\n", nh);

    VirtualPlant plant(h, block);
    SecPathLine line;
    line.resize(sp.ringSize());
    (void)line;

    const int N = 3 * 48000;           // 3 秒
    std::vector<float> d(static_cast<size_t>(N), 0.0f);
    addTone(d.data(), N, 120.0f, 0.05f, fs);
    addTone(d.data(), N, 240.0f, 0.025f, fs);
    addTone(d.data(), N, 360.0f, 0.012f, fs);

    std::vector<float> e(static_cast<size_t>(N), 0.0f);
    std::vector<float> y(static_cast<size_t>(N), 0.0f);
    std::vector<float> pend(static_cast<size_t>(block), 0.0f);

    for (int n0 = 0; n0 < N; n0 += block) {
        const int n = std::min(block, N - n0);
        plant.pending(pend.data(), n);
        for (int i = 0; i < n; i++) {
            e[static_cast<size_t>(n0 + i)] = d[static_cast<size_t>(n0 + i)] + pend[static_cast<size_t>(i)];
        }
        for (int i = 0; i < n; i++) {
            y[static_cast<size_t>(n0 + i)] = hc.process(e[static_cast<size_t>(n0 + i)]);
        }
        plant.feed(&y[static_cast<size_t>(n0)], n);
    }

    const int tail = 48000;
    const float dRms = rms(&d[static_cast<size_t>(N - tail)], tail);
    const float eRms = rms(&e[static_cast<size_t>(N - tail)], tail);
    const float yRms = rms(&y[static_cast<size_t>(N - tail)], tail);
    const float nr = 20.0f * std::log10(std::max(dRms, 1e-9f) / std::max(eRms, 1e-9f));
    printf("     d_rms=%.5f e_rms=%.6f y_rms=%.5f  NR=%.1f dB\n", dRms, eRms, yRms, nr);

    ASSERT_GT(yRms, 1e-4f, "anti-noise output must NOT be silent (v3 bug)");
    ASSERT_GT(nr, 20.0f, "closed-loop narrowband NR should exceed 20 dB");
    return true;
}

// ============================================================
// 测试 4: 回环延迟变大仍然稳定 (只允许更小的步长)
// ============================================================
static bool test_harmonic_canceller_long_delay() {
    const float fs = 48000.0f;
    const int D = 960;                 // 20 ms
    const int block = 256;
    auto h = makeSecondaryPath(D);

    SecondaryPath sp;
    sp.setImpulseResponse(h.data(), static_cast<int>(h.size()), fs);

    // 长延迟下步长必须显著减小 (仿真: D=960 时临界 ~0.0025, 取 0.002)
    const float step = 0.002f;

    HarmonicCanceller hc;
    hc.init(fs, 6, 30.0f, 600.0f, step);
    hc.setSecondaryPath(&sp);
    hc.lockTo(150.0f);

    VirtualPlant plant(h, block);
    const int N = 6 * 48000;           // 6 秒 (长延迟收敛慢)
    std::vector<float> d(static_cast<size_t>(N), 0.0f);
    addTone(d.data(), N, 150.0f, 0.05f, fs);
    addTone(d.data(), N, 300.0f, 0.025f, fs);

    std::vector<float> e(static_cast<size_t>(N), 0.0f);
    std::vector<float> y(static_cast<size_t>(N), 0.0f);
    std::vector<float> pend(static_cast<size_t>(block), 0.0f);

    for (int n0 = 0; n0 < N; n0 += block) {
        const int n = std::min(block, N - n0);
        plant.pending(pend.data(), n);
        for (int i = 0; i < n; i++) {
            e[static_cast<size_t>(n0 + i)] = d[static_cast<size_t>(n0 + i)] + pend[static_cast<size_t>(i)];
        }
        for (int i = 0; i < n; i++) {
            y[static_cast<size_t>(n0 + i)] = hc.process(e[static_cast<size_t>(n0 + i)]);
        }
        plant.feed(&y[static_cast<size_t>(n0)], n);
    }

    const int tail = 48000;
    const float dRms = rms(&d[static_cast<size_t>(N - tail)], tail);
    const float eRms = rms(&e[static_cast<size_t>(N - tail)], tail);
    const float nr = 20.0f * std::log10(std::max(dRms, 1e-9f) / std::max(eRms, 1e-9f));
    printf("     D=%d (20ms) step=%.5f  NR=%.1f dB  e_rms=%.6f\n", D, step, nr, eRms);

    ASSERT_TRUE(eRms < dRms, "residual must not exceed the disturbance (no divergence)");
    ASSERT_GT(nr, 8.0f, "long-delay narrowband NR should still exceed 8 dB");
    return true;
}

// ============================================================
// 测试 5: 基频检测器
// ============================================================
static bool test_tone_detector() {
    const float fs = 48000.0f;
    ToneDetector td;
    td.init(fs, 4096, 30.0f, 900.0f, 6.0f);

    const int N = 8000;
    std::vector<float> x(static_cast<size_t>(N), 0.0f);
    addTone(x.data(), N, 137.0f, 0.1f, fs);
    addTone(x.data(), N, 274.0f, 0.05f, fs);
    // 加一点宽带
    uint32_t s = 7;
    for (int i = 0; i < N; i++) {
        s = s * 1103515245u + 12345u;
        x[static_cast<size_t>(i)] += 0.005f * ((static_cast<float>((s >> 8) & 0xFFFF) / 32768.0f) - 1.0f);
    }
    td.push(x.data(), N);
    const float f0 = td.detect();
    printf("     detected f0 = %.2f Hz (true 137 / sub-harmonic 45.7)\n", f0);
    ASSERT_TRUE(f0 > 0.0f, "should detect a narrowband component");
    // 允许检测到基频或其低次谐波
    const bool ok = std::fabs(f0 - 137.0f) < 6.0f ||
                    std::fabs(f0 - 68.5f) < 4.0f ||
                    std::fabs(f0 - 45.67f) < 3.0f;
    ASSERT_TRUE(ok, "detected frequency should match the tone or its sub-harmonic");
    return true;
}

// ============================================================
// 测试 6: 纯宽带噪声下不应发散
// ============================================================
static bool test_processor_white_noise_stability() {
    const float fs = 48000.0f;
    const int block = 128;
    auto h = makeSecondaryPath(288);

    ANCConfig cfg;
    cfg.sampleRate = static_cast<int>(fs);
    cfg.filterLength = 256;
    cfg.secondaryPathLength = 1024;
    cfg.blockSize = block;
    cfg.mode = 0;

    AudioProcessor proc;
    proc.init(cfg);
    proc.setSecondaryPathIr(h.data(), static_cast<int>(h.size()));
    proc.enable(true);

    VirtualPlant plant(h, block);
    const int N = 2 * 48000;
    std::vector<float> d(static_cast<size_t>(N), 0.0f);
    uint32_t s = 99;
    for (int i = 0; i < N; i++) {
        s = s * 1103515245u + 12345u;
        d[static_cast<size_t>(i)] = 0.05f * ((static_cast<float>((s >> 8) & 0xFFFF) / 32768.0f) - 1.0f);
    }
    std::vector<float> e(static_cast<size_t>(N), 0.0f);
    std::vector<float> y(static_cast<size_t>(N), 0.0f);
    std::vector<float> pend(static_cast<size_t>(block), 0.0f);
    std::vector<float> out(static_cast<size_t>(block), 0.0f);

    for (int n0 = 0; n0 < N; n0 += block) {
        const int n = std::min(block, N - n0);
        plant.pending(pend.data(), n);
        for (int i = 0; i < n; i++) {
            e[static_cast<size_t>(n0 + i)] = d[static_cast<size_t>(n0 + i)] + pend[static_cast<size_t>(i)];
        }
        proc.processFrame(&e[static_cast<size_t>(n0)], out.data(), n);
        for (int i = 0; i < n; i++) y[static_cast<size_t>(n0 + i)] = out[static_cast<size_t>(i)];
        plant.feed(&y[static_cast<size_t>(n0)], n);
    }

    const int tail = 24000;
    const float dRms = rms(&d[static_cast<size_t>(N - tail)], tail);
    const float eRms = rms(&e[static_cast<size_t>(N - tail)], tail);
    const float yRms = rms(&y[static_cast<size_t>(N - tail)], tail);
    printf("     white: d_rms=%.5f e_rms=%.5f y_rms=%.5f\n", dRms, eRms, yRms);

    ASSERT_TRUE(eRms < 4.0f * dRms, "must not blow up on broadband noise");
    ASSERT_TRUE(eRms > 0.2f * dRms, "must not destroy the signal either");
    return true;
}

// ============================================================
// 测试 7: 未启用时输出必须为零 (不泄漏麦克风信号到扬声器)
// ============================================================
static bool test_disabled_output_silent() {
    ANCConfig cfg;
    cfg.blockSize = 128;
    AudioProcessor proc;
    proc.init(cfg);
    proc.enable(false);

    std::vector<float> mic(128, 0.5f);
    std::vector<float> out(128, 9.9f);
    proc.processFrame(mic.data(), out.data(), 128);
    for (int i = 0; i < 128; i++) {
        ASSERT_NEAR(out[static_cast<size_t>(i)], 0.0f, 1e-9f, "disabled output must be silent");
    }
    return true;
}

// ============================================================
// 测试 8: 输出限幅
// ============================================================
static bool test_output_limiting() {
    const float fs = 48000.0f;
    auto h = makeSecondaryPath(288);
    ANCConfig cfg;
    cfg.sampleRate = static_cast<int>(fs);
    cfg.blockSize = 128;
    cfg.mode = 0;
    AudioProcessor proc;
    proc.init(cfg);
    proc.setSecondaryPathIr(h.data(), static_cast<int>(h.size()));
    proc.enable(true);

    std::vector<float> mic(128, 0.0f);
    std::vector<float> out(128, 0.0f);
    addTone(mic.data(), 128, 120.0f, 4.0f, fs);   // 极端大信号
    for (int b = 0; b < 200; b++) {
        proc.processFrame(mic.data(), out.data(), 128);
        for (int i = 0; i < 128; i++) {
            if (std::fabs(out[static_cast<size_t>(i)]) > 1.0f) {
                ASSERT_TRUE(false, "output must stay within [-1, 1]");
            }
        }
    }
    return true;
}

// ============================================================
// 测试 9: 频谱分析器基本正确性
// ============================================================
static bool test_spectrum_analyzer() {
    SpectrumAnalyzer sa(1024);
    std::vector<float> x(1024, 0.0f);
    addTone(x.data(), 1024, 48000.0f * 100.0f / 1024.0f, 1.0f, 48000.0f);
    sa.analyze(x.data(), 1024);
    std::vector<float> mag(512, 0.0f);
    sa.getLinearMagnitude(mag.data(), 512);
    int best = 0;
    for (int i = 1; i < 512; i++) if (mag[static_cast<size_t>(i)] > mag[static_cast<size_t>(best)]) best = i;
    printf("     peak bin = %d (expect 100)\n", best);
    ASSERT_TRUE(std::abs(best - 100) <= 1, "FFT peak should be at bin 100");
    return true;
}

// ============================================================
int main() {
    printf("==================================================\n");
    printf("  QuietZone ANC Unit Tests v4 (desktop)\n");
    printf("==================================================\n\n");

    RUN_TEST(test_secondary_path_filtering);
    RUN_TEST(test_adaptive_filter_system_id);
    RUN_TEST(test_harmonic_canceller_closed_loop);
    RUN_TEST(test_harmonic_canceller_long_delay);
    RUN_TEST(test_tone_detector);
    RUN_TEST(test_processor_white_noise_stability);
    RUN_TEST(test_disabled_output_silent);
    RUN_TEST(test_output_limiting);
    RUN_TEST(test_spectrum_analyzer);

    printf("\n--------------------------------------------------\n");
    printf("  Results: %d passed, %d failed, %d total\n", g_pass, g_fail, g_pass + g_fail);
    printf("--------------------------------------------------\n");
    return g_fail > 0 ? 1 : 0;
}
