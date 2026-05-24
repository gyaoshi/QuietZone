/**
 * Lock-Free 单生产者单消费者 (SPSC) 环形缓冲区
 *
 * 用于 Oboe 音频回调与 ANC 引擎之间的零拷贝数据传递:
 *   - 生产者: Oboe InputStream 回调写入麦克风数据
 *   - 消费者: Oboe OutputStream 回调读取并处理
 *
 * 特性:
 *   - 无锁 (atomic 实现)
 *   - 无内存分配 (预分配固定大小)
 *   - 缓存友好 (连续内存布局)
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <atomic>
#include <vector>
#include <cstring>
#include <algorithm>
#include <type_traits>

namespace anc {

template <typename T>
class RingBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "RingBuffer requires trivially copyable type T");
public:
    /**
     * @param capacity 缓冲区容量 (自动向上取整到2的幂)
     */
    explicit RingBuffer(size_t capacity)
        : capacity_(nextPowerOf2(capacity))
        , mask_(capacity_ - 1)
        , buffer_(capacity_, T{0})
        , write_pos_(0)
        , read_pos_(0)
    {}

    // ===== 写入 (生产者调用, 仅在一个线程中调用) =====

    /**
     * 写入数据到环形缓冲
     * @return 实际写入的元素数量
     */
    size_t write(const T* data, size_t count) {
        size_t wp = write_pos_.load(std::memory_order_relaxed);
        size_t rp = read_pos_.load(std::memory_order_acquire);
        size_t available = capacity_ - (wp - rp);  // 可写空间

        count = std::min(count, available);
        if (count == 0) return 0;

        // 分两段拷贝 (可能跨越缓冲区末尾)
        size_t write_idx = wp & mask_;
        size_t first = std::min(count, capacity_ - write_idx);

        std::memcpy(&buffer_[write_idx], data, first * sizeof(T));
        if (count > first) {
            std::memcpy(&buffer_[0], data + first, (count - first) * sizeof(T));
        }

        write_pos_.store(wp + count, std::memory_order_release);
        return count;
    }

    /** 写入单个元素 */
    bool writeOne(T value) {
        size_t wp = write_pos_.load(std::memory_order_relaxed);
        size_t rp = read_pos_.load(std::memory_order_acquire);

        if (capacity_ - (wp - rp) == 0) return false;

        buffer_[wp & mask_] = value;
        write_pos_.store(wp + 1, std::memory_order_release);
        return true;
    }

    // ===== 读取 (消费者调用, 仅在一个线程中调用) =====

    /**
     * 从环形缓冲读取数据
     * @return 实际读取的元素数量
     */
    size_t read(T* data, size_t count) {
        size_t rp = read_pos_.load(std::memory_order_relaxed);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t available = wp - rp;  // 可读数据量

        count = std::min(count, available);
        if (count == 0) return 0;

        size_t read_idx = rp & mask_;
        size_t first = std::min(count, capacity_ - read_idx);

        std::memcpy(data, &buffer_[read_idx], first * sizeof(T));
        if (count > first) {
            std::memcpy(data + first, &buffer_[0], (count - first) * sizeof(T));
        }

        read_pos_.store(rp + count, std::memory_order_release);
        return count;
    }

    /** 读取单个元素 */
    bool readOne(T& value) {
        size_t rp = read_pos_.load(std::memory_order_relaxed);
        size_t wp = write_pos_.load(std::memory_order_acquire);

        if (wp - rp == 0) return false;

        value = buffer_[rp & mask_];
        read_pos_.store(rp + 1, std::memory_order_release);
        return true;
    }

    // ===== 查询 =====

    /** 可读元素数 */
    size_t availableRead() const {
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t rp = read_pos_.load(std::memory_order_relaxed);
        return wp - rp;
    }

    /** 可写空间 */
    size_t availableWrite() const {
        size_t wp = write_pos_.load(std::memory_order_relaxed);
        size_t rp = read_pos_.load(std::memory_order_acquire);
        return capacity_ - (wp - rp);
    }

    /** 是否为空 */
    bool isEmpty() const { return availableRead() == 0; }

    /** 是否已满 */
    bool isFull() const { return availableWrite() == 0; }

    /** 缓冲区容量 */
    size_t capacity() const { return capacity_; }

    /** 重置 (非线程安全, 需暂停读写后调用) */
    void reset() {
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
        std::fill(buffer_.begin(), buffer_.end(), T{0});
    }

private:
    size_t nextPowerOf2(size_t n) {
        if (n == 0) return 1;
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;  // 支持 64-bit size_t
        return n + 1;
    }

    size_t capacity_;
    size_t mask_;
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> write_pos_;  // 避免false sharing
    alignas(64) std::atomic<size_t> read_pos_;
};

} // namespace anc

#endif // RING_BUFFER_H
