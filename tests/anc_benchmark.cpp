/**
 * QuietZone ANC 性能基准测试
 *
 * 测量 FxLMS 核心 API 的单帧耗时:
 *   - process(): FIR 卷积
 *   - update(): 标量 FxLMS 权重更新
 *   - filterSample(): 次级路径滤波
 *   - processFrame(): 完整 ANC 帧处理
 *
 * 编译: cd tests/build && cmake .. && make
 * 运行: ./anc_benchmark
 */

#include "anc_engine.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <functional>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// 计时辅助
// ============================================================
using Clock = std::chrono::high_resolution_clock;
using Microseconds = std::chrono::duration<double, std::micro>;

struct BenchResult {
    const char* name;
    double mean_us;
    double min_us;
    double max_us;
    double p99_us;
};

static BenchResult run_benchmark(const char* name, int iterations, std::function<void()> fn) {
    std::vector<double> times;
    times.reserve(iterations);

    // 预热
    for (int i = 0; i < 100; i++) fn();

    for (int i = 0; i < iterations; i++) {
        auto t0 = Clock::now();
        fn();
        auto t1 = Clock::now();
        times.push_back(Microseconds(t1 - t0).count());
    }

    std::sort(times.begin(), times.end());

    BenchResult r;
    r.name = name;
    r.min_us = times.front();
    r.max_us = times.back();
    r.p99_us = times[(int)(iterations * 0.99)];
    r.mean_us = std::accumulate(times.begin(), times.end(), 0.0) / iterations;
    return r;
}

static void print_result(const BenchResult& r) {
    printf("  %-35s  mean=%7.2f μs  min=%7.2f  max=%7.2f  p99=%7.2f\n",
           r.name, r.mean_us, r.min_us, r.max_us, r.p99_us);
}

// ============================================================
// 基准测试
// ============================================================
void bench_fxlms_process() {
    const int L = 256;
    const int iters = 10000;
    anc::FxLMSFilter filter(L, 0.01f, 0.9999f);

    auto r = run_benchmark("FxLMS::process() [L=256]", iters, [&]() {
        filter.process(0.5f);
    });
    print_result(r);
}

void bench_fxlms_update() {
    const int L = 256;
    const int iters = 10000;
    anc::FxLMSFilter filter(L, 0.01f, 0.9999f);

    // 先做一些 process 让 x_buffer 有数据
    for (int i = 0; i < L; i++) filter.process(0.1f);

    auto r = run_benchmark("FxLMS::update() [L=256, scalar]", iters, [&]() {
        filter.update(0.5f, 0.1f);
    });
    print_result(r);
}

void bench_sec_path_filter_sample() {
    const int M = 128;
    const int iters = 10000;
    anc::SecondaryPathEstimator est(M, 128);

    auto r = run_benchmark("SecPath::filterSample() [M=128]", iters, [&]() {
        est.filterSample(0.5f);
    });
    print_result(r);
}

void bench_sec_path_filter_with_buffer() {
    const int M = 128;
    const int iters = 10000;
    anc::SecondaryPathEstimator est(M, 128);
    std::vector<float> buf(M, 0.0f);
    int idx = 0;

    auto r = run_benchmark("SecPath::filterWithBuf() [M=128]", iters, [&]() {
        est.filterSampleWithBuffer(0.5f, buf, idx);
    });
    print_result(r);
}

void bench_processor_frame_feedforward() {
    using namespace anc;

    ANCConfig config;
    config.filterLength = 256;
    config.secondaryPathLength = 128;
    config.stepSize = 0.01f;
    config.blockSize = 128;
    config.mode = 0;

    AudioProcessor proc;
    proc.init(config);
    proc.enable(true);

    std::vector<float> mic(128, 0.3f);
    std::vector<float> out(128);

    const int iters = 5000;
    auto r = run_benchmark("processFrame() FF [L=256,M=128,B=128]", iters, [&]() {
        proc.processFrame(mic.data(), out.data(), 128);
    });
    print_result(r);

    // 检查是否满足实时约束
    // 128 samples @48kHz = 2666.7 μs
    double budget_us = 128.0 / 48000.0 * 1e6;
    printf("    Real-time budget: %.1f μs → %s\n", budget_us,
           r.p99_us < budget_us ? "✅ FITS" : "❌ TOO SLOW");
}

void bench_processor_frame_hybrid() {
    using namespace anc;

    ANCConfig config;
    config.filterLength = 256;
    config.secondaryPathLength = 128;
    config.stepSize = 0.01f;
    config.blockSize = 128;
    config.mode = 2;  // Hybrid

    AudioProcessor proc;
    proc.init(config);
    proc.enable(true);

    std::vector<float> mic(128, 0.3f);
    std::vector<float> out(128);

    const int iters = 5000;
    auto r = run_benchmark("processFrame() Hybrid [L=256,M=128,B=128]", iters, [&]() {
        proc.processFrame(mic.data(), out.data(), 128);
    });
    print_result(r);

    double budget_us = 128.0 / 48000.0 * 1e6;
    printf("    Real-time budget: %.1f μs → %s\n", budget_us,
           r.p99_us < budget_us ? "✅ FITS" : "❌ TOO SLOW");
}

void bench_processor_frame_small_filter() {
    using namespace anc;

    // 小滤波器: L=64, M=32, B=64 (低功耗模式)
    ANCConfig config;
    config.filterLength = 64;
    config.secondaryPathLength = 32;
    config.stepSize = 0.01f;
    config.blockSize = 64;
    config.mode = 2;

    AudioProcessor proc;
    proc.init(config);
    proc.enable(true);

    std::vector<float> mic(64, 0.3f);
    std::vector<float> out(64);

    const int iters = 5000;
    auto r = run_benchmark("processFrame() Hybrid [L=64,M=32,B=64]", iters, [&]() {
        proc.processFrame(mic.data(), out.data(), 64);
    });
    print_result(r);

    double budget_us = 64.0 / 48000.0 * 1e6;
    printf("    Real-time budget: %.1f μs → %s\n", budget_us,
           r.p99_us < budget_us ? "✅ FITS" : "❌ TOO SLOW");
}

// ============================================================
// Main
// ============================================================
int main() {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  QuietZone ANC Performance Benchmark            ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    printf("── Core API ──────────────────────────────────────\n");
    bench_fxlms_process();
    bench_fxlms_update();
    bench_sec_path_filter_sample();
    bench_sec_path_filter_with_buffer();

    printf("\n── Frame Processing ──────────────────────────────\n");
    bench_processor_frame_feedforward();
    bench_processor_frame_hybrid();
    bench_processor_frame_small_filter();

    printf("\n══════════════════════════════════════════════════\n");
    printf("  Benchmark complete\n");
    printf("══════════════════════════════════════════════════\n");

    return 0;
}
