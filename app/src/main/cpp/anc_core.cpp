/**
 * ANC 内核基础组件实现 (v4)
 *
 *   SecondaryPath    — 稀疏"纯延迟 + 有效支路"次级路径模型
 *   AdaptiveFilter   — 归一化 FxLMS (向量滤波参考, 正确实现)
 *   HarmonicCanceller— 窄带谐波抵消器 (内部合成参考, 不受因果性限制)
 *   ToneDetector     — 基频检测 (Hann + Goertzel + 抛物线插值 + 次谐波判决)
 *   Biquad/BandPass  — 双二阶级联滤波器
 */

#include "anc_engine.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace anc {

static constexpr float kTwoPi = 6.283185307179586f;

// ============================================================
// Biquad / BandPass
// ============================================================
static void biquadDesign(bool lowpass, float fc, float fs, float q,
                         float& b0, float& b1, float& b2,
                         float& a1, float& a2) {
    const float K = std::tan(static_cast<float>(M_PI) * fc / fs);
    const float norm = 1.0f / (1.0f + K / q + K * K);
    if (lowpass) {
        b0 = K * K * norm;
        b1 = 2.0f * b0;
        b2 = b0;
    } else {
        b0 = norm;
        b1 = -2.0f * norm;
        b2 = norm;
    }
    a1 = 2.0f * (K * K - 1.0f) * norm;
    a2 = (1.0f - K / q + K * K) * norm;
}

void Biquad::setLowpass(float fc, float fs, float q) {
    biquadDesign(true, fc, fs, q, b0_, b1_, b2_, a1_, a2_);
    reset();
}

void Biquad::setHighpass(float fc, float fs, float q) {
    biquadDesign(false, fc, fs, q, b0_, b1_, b2_, a1_, a2_);
    reset();
}

void Biquad::complexGain(float freqHz, float fs, float& mag, float& phase) const {
    const float w = kTwoPi * freqHz / fs;
    const float cw = std::cos(w), sw = std::sin(w);
    const float c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);
    const float numRe = b0_ + b1_ * cw + b2_ * c2;
    const float numIm = -(b1_ * sw + b2_ * s2);
    const float denRe = 1.0f + a1_ * cw + a2_ * c2;
    const float denIm = -(a1_ * sw + a2_ * s2);
    const float dd = denRe * denRe + denIm * denIm;
    if (dd < 1e-20f) { mag = 0.0f; phase = 0.0f; return; }
    const float re = (numRe * denRe + numIm * denIm) / dd;
    const float im = (numIm * denRe - numRe * denIm) / dd;
    mag = std::sqrt(re * re + im * im);
    phase = std::atan2(im, re);
}

void BandPass::setBand(float loHz, float hiHz, float fs) {
    hp_.setHighpass(loHz, fs);
    lp_.setLowpass(hiHz, fs);
}

void BandPass::complexGain(float f, float fs, float& mag, float& phase) const {
    float m1, p1, m2, p2;
    hp_.complexGain(f, fs, m1, p1);
    lp_.complexGain(f, fs, m2, p2);
    mag = m1 * m2;
    phase = p1 + p2;
}

// ============================================================
// SecondaryPath
// ============================================================
SecondaryPath::SecondaryPath() {
    setDefaultModel(48000.0f, 8.0f, 220.0f, 0.35f);
}

void SecondaryPath::setDefaultModel(float sampleRate, float loopDelayMs,
                                    float spkCutoffHz, float spkGain) {
    fs_ = sampleRate;
    const int delay = std::max(1, static_cast<int>(std::lround(loopDelayMs * 1e-3f * sampleRate)));
    const int tailLen = 512;

    // 用双二阶高通 + 谐振的冲激响应构造扬声器支路
    Biquad spk, res;
    spk.setHighpass(spkCutoffHz, sampleRate, 0.7071f);
    res.setLowpass(std::min(sampleRate * 0.45f, 1200.0f), sampleRate, 0.5f);

    std::vector<float> tail(tailLen, 0.0f);
    for (int i = 0; i < tailLen; i++) {
        const float imp = (i == 0) ? 1.0f : 0.0f;
        tail[i] = (spkGain * 0.75f) * spk.process(imp) + (spkGain * 0.25f) * res.process(imp);
    }

    std::vector<float> h(static_cast<size_t>(delay) + tailLen, 0.0f);
    for (int i = 0; i < tailLen; i++) h[delay + i] = tail[i];

    setImpulseResponse(h.data(), static_cast<int>(h.size()), sampleRate);
    calibrated_ = false;
}

void SecondaryPath::setImpulseResponse(const float* h, int len, float sampleRate) {
    fs_ = sampleRate;
    full_len_ = len;
    full_.assign(h, h + len);

    // 找峰值
    int peak = 0;
    float peakAbs = 0.0f;
    for (int i = 0; i < len; i++) {
        const float a = std::fabs(h[i]);
        if (a > peakAbs) { peakAbs = a; peak = i; }
    }
    peak_index_ = peak;

    if (peakAbs < 1e-12f) {
        // 无效校准 → 给一个最小可用的"纯延迟 + 单位增益"模型 (不再递归调用)
        active_delay_ = 48;
        taps_ = 1;
        coeff_.assign(1, 0.3f);
        delay_ = active_delay_;
        ring_size_ = 128;
        calibrated_ = false;
        return;
    }

    // 有效支路: 从峰值前 4 个样本开始
    const int start = std::max(0, peak - 4);
    // 尾部截止: 连续 32 个样本低于峰值 1% 就截断
    const float thr = peakAbs * 0.01f;
    int run = 0;
    int end = len;
    for (int i = peak; i < len; i++) {
        if (std::fabs(h[i]) < thr) {
            if (++run >= 32) { end = i - 31; break; }
        } else {
            run = 0;
        }
    }
    if (end - start < 16) end = std::min(len, start + 64);

    active_delay_ = start;
    taps_ = end - start;
    coeff_.assign(h + start, h + end);

    delay_ = active_delay_;
    // 延迟线长度 = 需要的最大历史长度
    // (至少 2 的幂，便于用掩码回绕，且不小于 active_delay_+taps_)
    int need = active_delay_ + taps_;
    int size = 64;
    while (size < need) size <<= 1;
    ring_size_ = size;
    calibrated_ = true;
}

float SecondaryPath::filter(SecPathLine& line, float x) const {
    if (ring_size_ <= 0 || taps_ <= 0) return 0.0f;
    if (static_cast<int>(line.ring.size()) != ring_size_) {
        line.resize(ring_size_);
    }
    line.ring[line.idx] = x;

    float acc = 0.0f;
    int j = line.idx - active_delay_;
    if (j < 0) j += ring_size_;
    const float* c = coeff_.data();
    for (int k = 0; k < taps_; k++) {
        acc += c[k] * line.ring[j];
        if (--j < 0) j = ring_size_ - 1;
    }

    if (++line.idx >= ring_size_) line.idx = 0;
    return acc;
}

void SecondaryPath::responseAt(float freqHz, float& mag, float& phase) const {
    if (full_.empty() || freqHz <= 0.0f || freqHz >= fs_ * 0.5f) {
        mag = 0.0f;
        phase = 0.0f;
        return;
    }
    const double w = 2.0 * M_PI * freqHz / fs_;
    double re = 0.0, im = 0.0;
    const int from = active_delay_;
    const int to = std::min(full_len_, active_delay_ + taps_);
    for (int n = from; n < to; n++) {
        const double a = w * n;
        re += full_[n] * std::cos(a);
        im -= full_[n] * std::sin(a);
    }
    mag = static_cast<float>(std::sqrt(re * re + im * im));
    phase = static_cast<float>(std::atan2(im, re));
}

// ============================================================
// AdaptiveFilter
// ============================================================
void AdaptiveFilter::init(int length, float step, float leaky) {
    L_ = std::max(4, length);
    w_.assign(L_, 0.0f);
    xbuf_.assign(L_, 0.0f);
    xhbuf_.assign(L_, 0.0f);
    idx_ = 0;
    step_ = step;
    leaky_ = leaky;
}

AdaptiveFilter::~AdaptiveFilter() = default;

float AdaptiveFilter::process(float x) {
    xbuf_[idx_] = x;
    float y = 0.0f;
    int j = idx_;
    const float* w = w_.data();
    const float* b = xbuf_.data();
    for (int k = 0; k < L_; k++) {
        y += w[k] * b[j];
        if (--j < 0) j = L_ - 1;
    }
    return y;
}

void AdaptiveFilter::pushFilteredRef(float xhat) {
    xhbuf_[idx_] = xhat;
}

void AdaptiveFilter::update(float error, float mu) {
    const float g = mu * error;
    int j = idx_;
    float* w = w_.data();
    const float* h = xhbuf_.data();
    for (int k = 0; k < L_; k++) {
        w[k] = leaky_ * w[k] - g * h[j];
        if (--j < 0) j = L_ - 1;
    }
    if (++idx_ >= L_) idx_ = 0;

    // 权重限幅 (安全网)
    const float lim = 64.0f;
    for (int k = 0; k < L_; k++) {
        if (w[k] > lim) w[k] = lim;
        else if (w[k] < -lim) w[k] = -lim;
    }
}

void AdaptiveFilter::reset() {
    std::fill(w_.begin(), w_.end(), 0.0f);
    std::fill(xbuf_.begin(), xbuf_.end(), 0.0f);
    std::fill(xhbuf_.begin(), xhbuf_.end(), 0.0f);
    idx_ = 0;
}

float AdaptiveFilter::weightNorm() const {
    float s = 0.0f;
    for (int k = 0; k < L_; k++) s += w_[k] * w_[k];
    return std::sqrt(s);
}

// ============================================================
// HarmonicCanceller
// ============================================================
void HarmonicCanceller::init(float sampleRate, int maxHarmonics,
                             float bandLow, float bandHigh, float step) {
    fs_ = sampleRate;
    max_ = std::min(maxHarmonics, kMaxHarmonicsCapacity);
    bandLow_ = bandLow;
    bandHigh_ = bandHigh;
    step_ = step;
    n_ = 0;
    f0_ = 0.0f;
    nSamp_ = 0;
    for (int k = 0; k < kMaxHarmonicsCapacity; k++) {
        h_[k].wc = 0.0f;
        h_[k].ws = 0.0f;
        h_[k].ph = 0.0;
    }
}

int HarmonicCanceller::lockTo(float f0) {
    if (f0 < bandLow_ * 0.9f) { n_ = 0; f0_ = 0.0f; return 0; }
    f0_ = f0;
    n_ = 0;
    for (int k = 1; k <= max_; k++) {
        const float f = f0 * static_cast<float>(k);
        if (f > bandHigh_) break;
        Harm& h = h_[n_];
        h.f = f;
        h.wc = 0.0f;
        h.ws = 0.0f;
        h.ph = 0.0;
        h.dph = 2.0 * M_PI * static_cast<double>(f) / static_cast<double>(fs_);
        if (sp_) {
            float mag = 0.0f, phase = 0.0f;
            sp_->responseAt(f, mag, phase);
            if (mag < 1e-4f) mag = 1e-4f;
            h.A = mag;
            h.cosPhi = std::cos(phase);
            h.sinPhi = std::sin(phase);
        } else {
            h.A = 1.0f;
            h.cosPhi = 1.0f;
            h.sinPhi = 0.0f;
        }
        h.mu = step_ / (h.A * h.A);
        n_++;
    }
    return n_;
}

float HarmonicCanceller::process(float e) {
    if (n_ == 0) return 0.0f;

    float y = 0.0f;
    for (int k = 0; k < n_; k++) {
        Harm& h = h_[k];
        const float ph = static_cast<float>(h.ph);
        const float c = std::cos(ph);
        const float s = std::sin(ph);

        y += h.wc * c + h.ws * s;

        // 滤波参考: A·cos(ωn+φ), A·sin(ωn+φ)
        const float cf = h.A * (c * h.cosPhi - s * h.sinPhi);
        const float sf = h.A * (c * h.sinPhi + s * h.cosPhi);

        const float g = h.mu * e;
        h.wc -= g * cf;
        h.ws -= g * sf;

        // 轻微泄漏，防止在残差很小时权重无约束漂移
        h.wc *= 0.99998f;
        h.ws *= 0.99998f;

        const float lim = 64.0f;
        if (h.wc > lim) h.wc = lim; else if (h.wc < -lim) h.wc = -lim;
        if (h.ws > lim) h.ws = lim; else if (h.ws < -lim) h.ws = -lim;

        h.ph += h.dph;
        if (h.ph >= 2.0 * M_PI) h.ph -= 2.0 * M_PI;
    }
    nSamp_++;
    return y;
}

void HarmonicCanceller::reset() {
    for (int k = 0; k < kMaxHarmonicsCapacity; k++) {
        h_[k].wc = 0.0f;
        h_[k].ws = 0.0f;
        h_[k].ph = 0.0;
    }
    nSamp_ = 0;
}

float HarmonicCanceller::attenuationDb(int k) const {
    if (k < 0 || k >= n_) return 0.0f;
    const Harm& h = h_[k];
    const float mag = std::sqrt(h.wc * h.wc + h.ws * h.ws);
    if (mag < 1e-9f) return 0.0f;
    return 20.0f * std::log10(std::max(mag * h.A, 1e-9f));
}

// ============================================================
// ToneDetector
// ============================================================
void ToneDetector::init(float sampleRate, int windowLen, float loHz, float hiHz,
                        float tonalityThresholdDb) {
    fs_ = sampleRate;
    wlen_ = windowLen;
    hist_.assign(wlen_, 0.0f);
    win_.resize(wlen_);
    for (int i = 0; i < wlen_; i++) {
        win_[i] = 0.5f * (1.0f - std::cos(kTwoPi * i / (wlen_ - 1)));
    }
    lo_ = loHz;
    hi_ = hiHz;
    thr_db_ = tonalityThresholdDb;
    pos_ = 0;
    filled_ = 0;
    tonality_db_ = 0.0f;
}

void ToneDetector::reset() {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    pos_ = 0;
    filled_ = 0;
}

void ToneDetector::push(const float* x, int n) {
    for (int i = 0; i < n; i++) {
        hist_[pos_] = x[i];
        if (++pos_ >= wlen_) pos_ = 0;
        if (filled_ < wlen_) filled_++;
    }
}

float ToneDetector::goertzelWindowed(float freqHz) const {
    const double w = 2.0 * M_PI * static_cast<double>(freqHz) / static_cast<double>(fs_);
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    // 从最旧到最新遍历环形历史
    int idx = pos_;
    for (int i = 0; i < wlen_; i++) {
        const double v = static_cast<double>(hist_[idx]) * static_cast<double>(win_[i]);
        const double s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
        if (++idx >= wlen_) idx = 0;
    }
    const double re = s1 - cw * s2;
    const double im = std::sin(w) * s2;
    return static_cast<float>(re * re + im * im);
}

float ToneDetector::detect() {
    if (filled_ < wlen_) { tonality_db_ = 0.0f; return 0.0f; }

    // 频率网格: 分辨率取窗长对应的频格
    float df = fs_ / static_cast<float>(wlen_);
    if (df < 2.0f) df = 2.0f;
    int nPts = 0;
    for (float f = lo_; f < hi_ && nPts < kMaxScanPoints; f += df) {
        scanFreq_[nPts] = f;
        scanMag_[nPts] = goertzelWindowed(f);
        nPts++;
    }
    if (nPts < 4) { tonality_db_ = 0.0f; return 0.0f; }

    // 峰值
    int best = 0;
    for (int i = 1; i < nPts; i++) {
        if (scanMag_[i] > scanMag_[best]) best = i;
    }
    const float bestMag = scanMag_[best];
    if (bestMag <= 1e-20f) { tonality_db_ = 0.0f; return 0.0f; }

    // 中位数 (只对实际扫描过的点求, 否则未访问的 0 会把中位数拉到 0)
    std::copy(scanMag_, scanMag_ + nPts, sortBuf_);
    std::sort(sortBuf_, sortBuf_ + nPts);
    const float med = sortBuf_[nPts / 2];
    tonality_db_ = (med > 1e-20f)
        ? 10.0f * std::log10((bestMag + 1e-30f) / med)
        : 100.0f;
    if (tonality_db_ < thr_db_) return 0.0f;

    // 抛物线插值细化
    float refined = scanFreq_[best];
    if (best > 0 && best < nPts - 1) {
        const float m0 = scanMag_[best - 1];
        const float m1 = scanMag_[best];
        const float m2 = scanMag_[best + 1];
        const float denom = m0 - 2.0f * m1 + m2;
        if (std::fabs(denom) > 1e-24f) {
            const float d = 0.5f * (m0 - m2) / denom;
            if (d > -1.0f && d < 1.0f) refined = scanFreq_[best] + d * df;
        }
    }

    // 次谐波判决: 若 f/2, f/3, f/4 有足够能量，则把基频下移
    float f0 = refined;
    for (int k = 4; k >= 2; k--) {
        const float sub = refined / static_cast<float>(k);
        if (sub < lo_) continue;
        const float m = goertzelWindowed(sub);
        if (m > 0.10f * bestMag) { f0 = sub; break; }
    }
    if (f0 < lo_ || f0 > hi_) return 0.0f;
    return f0;
}

} // namespace anc
