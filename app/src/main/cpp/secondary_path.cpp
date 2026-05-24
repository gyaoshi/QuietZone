/**
 * 次级路径估计器实现 (v3 — 环形缓冲优化)
 *
 * 次级路径 Ŝ(z): 从降噪扬声器输出到误差麦克风的声学传递函数
 *   包括: DAC → 功放 → 扬声器 → 空间传播 → 误差麦克风 → ADC
 *
 * 估计方法:
 *   1. 离线估计: 白噪声激励 + LMS系统辨识
 *   2. 在线估计: 辅助白噪声注入 + 交叉相关法
 *
 * v3 改进:
 *   - 新增 filterSample(): 逐样本次级路径滤波，返回标量 x̂(n)
 *     供 FxLMSFilter::update(x_hat_n, error) 使用
 *   - 环形缓冲替代 rotate(): 避免每样本 O(M) 的数组移动
 *   - filterReference() 兼容保留，内部改用 filterSample() 实现
 *
 * 参考:
 *   [1] Eriksson, "Development of the filtered-U algorithm for ANC", JASA 1991
 *   [2] "Computation-efficient Online Secondary Path Modeling", arXiv 2023
 */

#include "anc_engine.h"
#include <cstdlib>
#include <algorithm>

namespace anc {

// ============================================================
// SecondaryPathEstimator
// ============================================================

SecondaryPathEstimator::SecondaryPathEstimator(int filterLength, int /*blockSize*/)
    : filter_length_(filterLength)
    , path_buf_idx_(0)
    , estimate_buf_idx_(0)
    , is_estimated_(false)
{
    path_coeffs_.resize(filter_length_, 0.0f);
    path_buffer_.resize(filter_length_, 0.0f);

    // 在线估计的自适应滤波器
    estimate_weights_.resize(filter_length_, 0.0f);
    estimate_buffer_.resize(filter_length_, 0.0f);

    // 初始化默认次级路径 (假设纯延迟)
    // 实际使用时需校准
    int delay_samples = 8; // 约0.17ms @48kHz
    if (delay_samples < filter_length_) {
        path_coeffs_[delay_samples] = 0.5f;
    }
}

SecondaryPathEstimator::~SecondaryPathEstimator() = default;

void SecondaryPathEstimator::offlineEstimate(const float* impulseResponse, int len) {
    int copyLen = std::min(len, filter_length_);
    std::fill(path_coeffs_.begin(), path_coeffs_.end(), 0.0f);
    std::copy(impulseResponse, impulseResponse + copyLen, path_coeffs_.data());
    is_estimated_ = true;
}

void SecondaryPathEstimator::setPathCoeffs(const float* coeffs, int len) {
    int copyLen = std::min(len, filter_length_);
    std::fill(path_coeffs_.begin(), path_coeffs_.end(), 0.0f);
    std::copy(coeffs, coeffs + copyLen, path_coeffs_.data());
    is_estimated_ = true;
}

void SecondaryPathEstimator::onlineUpdate(float error_sample, float auxiliary_noise) {
    if (!is_estimated_) {
        // 使用LMS在线估计次级路径
        float estimate_output = 0.0f;
        for (int i = 0; i < filter_length_; i++) {
            int idx = (estimate_buf_idx_ - i + filter_length_) % filter_length_;
            estimate_output += estimate_weights_[i] * estimate_buffer_[idx];
        }

        float est_error = error_sample - estimate_output;
        float mu = 0.005f;

        for (int i = 0; i < filter_length_; i++) {
            int idx = (estimate_buf_idx_ - i + filter_length_) % filter_length_;
            estimate_weights_[i] += mu * est_error * estimate_buffer_[idx];
        }

        // 更新缓冲 (环形写入)
        estimate_buffer_[estimate_buf_idx_] = auxiliary_noise;
        estimate_buf_idx_ = (estimate_buf_idx_ + 1) % filter_length_;
    }
}

float SecondaryPathEstimator::filterSample(float input) {
    /**
     * 逐样本次级路径 FIR 滤波: x̂(n) = Ŝ * x(n)
     *
     * 使用环形缓冲，避免 rotate() 的 O(M) 数组移动
     * 每样本复杂度: O(M) 乘加 (与 rotate 版相同, 但省去内存搬运)
     *
     * 返回标量滤波参考 x̂(n)，供 FxLMSFilter::update() 使用
     */

    // 写入新样本到环形缓冲
    path_buffer_[path_buf_idx_] = input;

    // FIR 卷积: x̂(n) = Σ_{i=0}^{M-1} Ŝ[i] · x[n-i]
    float sum = 0.0f;

#ifdef USE_NEON
    if (filter_length_ >= 8) {
        // NEON 优化: 需要连续内存，创建有序缓冲
        // 注意: 这里为了简化，先用标量实现
        // TODO: 如果 filterSample 成为性能瓶颈，可优化为 NEON 版本
        for (int i = 0; i < filter_length_; i++) {
            int idx = (path_buf_idx_ - i + filter_length_) % filter_length_;
            sum += path_coeffs_[i] * path_buffer_[idx];
        }
    } else
#endif
    {
        for (int i = 0; i < filter_length_; i++) {
            int idx = (path_buf_idx_ - i + filter_length_) % filter_length_;
            sum += path_coeffs_[i] * path_buffer_[idx];
        }
    }

    // 推进环形缓冲索引
    path_buf_idx_ = (path_buf_idx_ + 1) % filter_length_;

    return sum;
}

void SecondaryPathEstimator::filterReference(const float* input, float* output, int len) {
    /**
     * 分块滤波 (兼容接口)
     * 内部调用 filterSample()，保持状态一致性
     */
    for (int n = 0; n < len; n++) {
        output[n] = filterSample(input[n]);
    }
}

float SecondaryPathEstimator::filterSampleWithBuffer(float input, std::vector<float>& buf, int& buf_idx) const {
    /**
     * 使用独立缓冲的次级路径 FIR 滤波
     *
     * 与 filterSample() 功能相同，但使用调用方提供的独立缓冲
     * 用于 Hybrid 模式中多个次级路径滤波调用不能共享状态的场景
     *
     * 例如: Hybrid 中需要同时计算 x̂_ff = Ŝ·x(n) 和 x̂_fb = Ŝ·d̂(n)
     *       两次调用不能共用 path_buffer_，否则状态会互相污染
     */

    if ((int)buf.size() != filter_length_) {
        return 0.0f;
    }

    buf[buf_idx] = input;

    float sum = 0.0f;
    for (int i = 0; i < filter_length_; i++) {
        int idx = (buf_idx - i + filter_length_) % filter_length_;
        sum += path_coeffs_[i] * buf[idx];
    }

    buf_idx = (buf_idx + 1) % filter_length_;
    return sum;
}

void SecondaryPathEstimator::reset() {
    std::fill(path_coeffs_.begin(), path_coeffs_.end(), 0.0f);
    std::fill(path_buffer_.begin(), path_buffer_.end(), 0.0f);
    std::fill(estimate_weights_.begin(), estimate_weights_.end(), 0.0f);
    std::fill(estimate_buffer_.begin(), estimate_buffer_.end(), 0.0f);
    path_buf_idx_ = 0;
    estimate_buf_idx_ = 0;
    is_estimated_ = false;

    // 恢复默认纯延迟估计
    int delay_samples = 8;
    if (delay_samples < filter_length_) {
        path_coeffs_[delay_samples] = 0.5f;
    }
}

} // namespace anc
