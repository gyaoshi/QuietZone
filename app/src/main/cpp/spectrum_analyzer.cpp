/**
 * 频谱分析器 - 基于FFT的实时频谱分析
 *
 * 用于:
 *   1. 显示参考噪声频谱
 *   2. 显示残差噪声频谱
 *   3. 计算各频段降噪量
 *
 * 实现Radix-2 FFT (避免引入额外库依赖)
 */

#include "anc_engine.h"
#include <cmath>

namespace anc {

// M_PI 在某些平台上可能未定义
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void fft(float* real, float* imag, int n) {
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    // Cooley-Tukey FFT
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * (float)M_PI / len;
        float wReal = cosf(angle);
        float wImag = sinf(angle);

        for (int i = 0; i < n; i += len) {
            float curReal = 1.0f, curImag = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                float tReal = curReal * real[i + j + len/2] - curImag * imag[i + j + len/2];
                float tImag = curReal * imag[i + j + len/2] + curImag * real[i + j + len/2];

                real[i + j + len/2] = real[i + j] - tReal;
                imag[i + j + len/2] = imag[i + j] - tImag;
                real[i + j] += tReal;
                imag[i + j] += tImag;

                float newCurReal = curReal * wReal - curImag * wImag;
                float newCurImag = curReal * wImag + curImag * wReal;
                curReal = newCurReal;
                curImag = newCurImag;
            }
        }
    }
}

// ============================================================
// SpectrumAnalyzer
// ============================================================

SpectrumAnalyzer::SpectrumAnalyzer(int fftSize)
    : fft_size_(fftSize)
{
    real_part_.resize(fftSize, 0.0f);
    imag_part_.resize(fftSize, 0.0f);
    magnitude_.resize(fftSize / 2, 0.0f);

    // Hanning窗
    window_.resize(fftSize);
    for (int i = 0; i < fftSize; i++) {
        window_[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (fftSize - 1)));
    }
}

SpectrumAnalyzer::~SpectrumAnalyzer() = default;

void SpectrumAnalyzer::analyze(const float* input, int len) {
    int copyLen = std::min(len, fft_size_);

    std::fill(real_part_.begin(), real_part_.end(), 0.0f);
    std::fill(imag_part_.begin(), imag_part_.end(), 0.0f);

    // 加窗
    for (int i = 0; i < copyLen; i++) {
        real_part_[i] = input[i] * window_[i];
    }

    // FFT
    fft(real_part_.data(), imag_part_.data(), fft_size_);

    // 计算幅度谱
    for (int i = 0; i < fft_size_ / 2; i++) {
        magnitude_[i] = sqrtf(real_part_[i] * real_part_[i] + imag_part_[i] * imag_part_[i]);
    }
}

void SpectrumAnalyzer::getMagnitudeSpectrum(float* output, int outputLen) {
    int copyLen = std::min(outputLen, fft_size_ / 2);
    for (int i = 0; i < copyLen; i++) {
        // 转换为dB
        output[i] = 20.0f * log10f(magnitude_[i] + 1e-10f);
    }
}

void SpectrumAnalyzer::getReductionSpectrum(const float* ref, const float* err,
                                             int len, float* output, int outputLen) {
    // 计算参考和误差频谱
    analyze(ref, len);
    std::vector<float> ref_mag(fft_size_ / 2);
    for (int i = 0; i < fft_size_ / 2; i++) {
        ref_mag[i] = magnitude_[i];
    }

    analyze(err, len);

    int copyLen = std::min(outputLen, fft_size_ / 2);
    for (int i = 0; i < copyLen; i++) {
        // 降噪量 = ref_mag - err_mag (dB)
        float ref_db = 20.0f * log10f(ref_mag[i] + 1e-10f);
        float err_db = 20.0f * log10f(magnitude_[i] + 1e-10f);
        output[i] = ref_db - err_db;
    }
}

} // namespace anc
