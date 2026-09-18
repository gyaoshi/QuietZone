/**
 * 频谱分析器 — 基于 Radix-2 FFT
 *
 * 用于 UI 显示参考/残余频谱与分频段降噪量。
 * 实时路径不使用本模块。
 */

#ifndef SPECTRUM_ANALYZER_H
#define SPECTRUM_ANALYZER_H

#include <vector>

namespace anc {

class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(int fftSize);
    ~SpectrumAnalyzer();

    /** 分析一段时域数据 (超出 fftSize 的部分被截断) */
    void analyze(const float* input, int len);

    /** 幅度谱 (dB) */
    void getMagnitudeSpectrum(float* output, int outputLen);

    /** 线性幅度谱 */
    void getLinearMagnitude(float* output, int outputLen);

    /** 两段信号的幅度差谱 (dB) */
    void getReductionSpectrum(const float* ref, const float* err,
                              int len, float* output, int outputLen);

    int getFFTSize() const { return fft_size_; }

    // 供其它模块复用的静态工具
    static void fft(float* real, float* imag, int n);

private:
    int fft_size_;
    std::vector<float> window_;
    std::vector<float> real_part_;
    std::vector<float> imag_part_;
    std::vector<float> magnitude_;
    std::vector<float> scratch_;
};

} // namespace anc

#endif // SPECTRUM_ANALYZER_H
