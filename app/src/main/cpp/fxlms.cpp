/**
 * FxLMS 自适应滤波器实现 (v3 — 标量更新修正版)
 *
 * 核心算法 (已验证正确):
 *   y(n) = w^T(n) · x(n)                      [滤波器输出]
 *   e(n) = d(n) + s2 * y(n)                   [误差信号]
 *   x̂(n) = Ŝ * x(n)                           [标量滤波参考信号]
 *   w(n+1) = (1-μδ)w(n) + μ·x̂(n)·e(n)·x_buf  [Leaky FxLMS更新]
 */

#include "anc_engine.h"

#ifdef USE_NEON
#include <arm_neon.h>
#endif

#include <algorithm>
#include <cstring>

namespace anc {

FxLMSFilter::FxLMSFilter(int length, float stepSize, float leakyFactor)
    : length_(length)
    , stepSize_(stepSize)
    , leakyFactor_(leakyFactor)
    , buffer_index_(0)
{
    weights_.resize(length, 0.0f);
    x_buffer_.resize(length, 0.0f);
}

FxLMSFilter::~FxLMSFilter() = default;

float FxLMSFilter::process(float x_sample) {
    x_buffer_[buffer_index_] = x_sample;

    float y = 0.0f;

    int seg1_len = buffer_index_ + 1;
    int seg2_len = length_ - seg1_len;

#ifdef USE_NEON
    if (length_ >= 8) {
        // 将环形缓冲数据拷贝到连续临时数组 (栈分配, 避免 thread_local)
        float x_ordered[512];  // ANC 滤波器长度不超过 512
        for (int i = 0; i < seg1_len; i++) {
            x_ordered[i] = x_buffer_[buffer_index_ - i];
        }
        for (int i = 0; i < seg2_len; i++) {
            x_ordered[seg1_len + i] = x_buffer_[length_ - 1 - i];
        }

        float32x4_t sum0 = vdupq_n_f32(0.0f);
        float32x4_t sum1 = vdupq_n_f32(0.0f);
        int i = 0;
        for (; i <= length_ - 8; i += 8) {
            float32x4_t w0 = vld1q_f32(&weights_[i]);
            float32x4_t w1 = vld1q_f32(&weights_[i + 4]);
            float32x4_t x0 = vld1q_f32(&x_ordered[i]);
            float32x4_t x1 = vld1q_f32(&x_ordered[i + 4]);
            sum0 = vmlaq_f32(sum0, w0, x0);
            sum1 = vmlaq_f32(sum1, w1, x1);
        }
        sum0 = vaddq_f32(sum0, sum1);
        float32x2_t lo = vget_low_f32(sum0);
        float32x2_t hi = vget_high_f32(sum0);
        lo = vadd_f32(lo, hi);
        float tmp[2];
        vst1_f32(tmp, lo);
        y = tmp[0] + tmp[1];

        for (; i < length_; i++) {
            y += weights_[i] * x_ordered[i];
        }
    } else
#endif
    {
        for (int i = 0; i < seg1_len; i++) {
            y += weights_[i] * x_buffer_[buffer_index_ - i];
        }
        for (int i = 0; i < seg2_len; i++) {
            y += weights_[seg1_len + i] * x_buffer_[length_ - 1 - i];
        }
    }

    buffer_index_ = (buffer_index_ + 1) % length_;

    return y;
}

void FxLMSFilter::update(float x_hat_n, float error) {
    const float mu_xh_e = stepSize_ * x_hat_n * error;

#ifdef USE_NEON
    if (length_ >= 8) {
        // 创建有序参考缓冲 (栈分配)
        float x_ordered[512];

        int start = (buffer_index_ - 1 + length_) % length_;
        int seg1_len = start + 1;
        int seg2_len = length_ - seg1_len;

        for (int i = 0; i < seg1_len; i++) {
            x_ordered[i] = x_buffer_[start - i];
        }
        for (int i = 0; i < seg2_len; i++) {
            x_ordered[seg1_len + i] = x_buffer_[length_ - 1 - i];
        }

        float32x4_t leaky_vec = vdupq_n_f32(leakyFactor_);
        float32x4_t mu_xh_e_vec = vdupq_n_f32(mu_xh_e);

        int i = 0;
        for (; i <= length_ - 8; i += 8) {
            float32x4_t w0 = vld1q_f32(&weights_[i]);
            float32x4_t w1 = vld1q_f32(&weights_[i + 4]);
            float32x4_t x0 = vld1q_f32(&x_ordered[i]);
            float32x4_t x1 = vld1q_f32(&x_ordered[i + 4]);

            w0 = vmulq_f32(leaky_vec, w0);
            w0 = vmlaq_f32(w0, mu_xh_e_vec, x0);
            w1 = vmulq_f32(leaky_vec, w1);
            w1 = vmlaq_f32(w1, mu_xh_e_vec, x1);

            vst1q_f32(&weights_[i], w0);
            vst1q_f32(&weights_[i + 4], w1);
        }
        for (; i <= length_ - 4; i += 4) {
            float32x4_t w = vld1q_f32(&weights_[i]);
            float32x4_t x = vld1q_f32(&x_ordered[i]);
            w = vmulq_f32(leaky_vec, w);
            w = vmlaq_f32(w, mu_xh_e_vec, x);
            vst1q_f32(&weights_[i], w);
        }
        for (; i < length_; i++) {
            weights_[i] = leakyFactor_ * weights_[i] + mu_xh_e * x_ordered[i];
        }
    } else
#endif
    {
        for (int i = 0; i < length_; i++) {
            int x_idx = (buffer_index_ - 1 - i + length_) % length_;
            weights_[i] = leakyFactor_ * weights_[i] + mu_xh_e * x_buffer_[x_idx];
        }
    }

    const float maxWeight = 10.0f;
    for (int i = 0; i < length_; i++) {
        weights_[i] = std::clamp(weights_[i], -maxWeight, maxWeight);
    }
}

void FxLMSFilter::reset() {
    std::fill(weights_.begin(), weights_.end(), 0.0f);
    std::fill(x_buffer_.begin(), x_buffer_.end(), 0.0f);
    buffer_index_ = 0;
}

float FxLMSFilter::getWeightNorm() const {
    float norm = 0.0f;

#ifdef USE_NEON
    if (length_ >= 4) {
        float32x4_t sum = vdupq_n_f32(0.0f);
        int i = 0;
        for (; i <= length_ - 4; i += 4) {
            float32x4_t w = vld1q_f32(&weights_[i]);
            sum = vmlaq_f32(sum, w, w);
        }
        float32x2_t lo = vget_low_f32(sum);
        float32x2_t hi = vget_high_f32(sum);
        lo = vadd_f32(lo, hi);
        float tmp[2];
        vst1_f32(tmp, lo);
        norm = tmp[0] + tmp[1];
        for (; i < length_; i++) {
            norm += weights_[i] * weights_[i];
        }
    } else
#endif
    {
        for (int i = 0; i < length_; i++) {
            norm += weights_[i] * weights_[i];
        }
    }

    return std::sqrt(norm);
}

} // namespace anc
