#ifndef RING_BUFFER_HPP
#define RING_BUFFER_HPP

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// 环形缓冲区：4096 字节连续流，双指针（head写 / tail读），写满覆盖起始
// 约定：永远保留 1 字节不用，head==tail 表示"空"
class RingBuffer
{
public:
    static constexpr size_t SIZE      = 4096;
    static constexpr size_t MASK      = SIZE - 1;   // 位与代替取模
    static constexpr size_t MAX_BYTES = SIZE - 1;   // 最多存 4095 字节（保留1字节防回绕混淆）

    RingBuffer() : head_(0), tail_(0) {}

    // ================================================================
    //  生产者：从 head 位置写入
    // ================================================================

    // 返回实际写入字节数
    size_t write(const uint8_t* data, size_t len)
    {
        if (len == 0) return 0;
        // 加锁
        std::lock_guard<std::mutex> lock(mtx_);

        // 最多写 SIZE-1 字节，保证 head 不会追上 tail
        size_t space = available_for_write_unlocked();
        if (len > space)
        {
            // 写不下：覆盖旧数据，推进 tail 腾空间
            size_t overwrite = len - space;
            tail_ = (tail_ + overwrite) & MASK;
            space = available_for_write_unlocked();  // 重新计算
        }

        size_t n = (len < space) ? len : space;
        for (size_t i = 0; i < n; ++i)
        {
            buf_[head_] = data[i];
            head_ = (head_ + 1) & MASK;
        }
        return n;
    }

    // ================================================================
    //  消费者
    // ================================================================

    // 可读字节数
    size_t available()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return available_for_read_unlocked();
    }

    // 窥视：读取但不消费（tail 不动），用于先看数据再决定是否消费
    size_t peek(uint8_t* out, size_t max_len)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t avail = available_for_read_unlocked();
        if (avail == 0 || max_len == 0) return 0;

        size_t n = (max_len < avail) ? max_len : avail;
        size_t first_chunk = SIZE - tail_;
        if (n <= first_chunk)
        {
            memcpy(out, buf_ + tail_, n);
        }
        else
        {
            memcpy(out, buf_ + tail_, first_chunk);
            memcpy(out + first_chunk, buf_, n - first_chunk);
        }
        return n;
    }

    // 读取并消费（tail 推进 n 字节）
    size_t read(uint8_t* out, size_t max_len)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t avail = available_for_read_unlocked();
        if (avail == 0 || max_len == 0) return 0;

        size_t n = (max_len < avail) ? max_len : avail;
        size_t first_chunk = SIZE - tail_;
        if (n <= first_chunk)
        {
            memcpy(out, buf_ + tail_, n);
        }
        else
        {
            memcpy(out, buf_ + tail_, first_chunk);
            memcpy(out + first_chunk, buf_, n - first_chunk);
        }
        tail_ = (tail_ + n) & MASK;
        return n;
    }

    // 跳过（消费）len 字节，不读出
    size_t skip(size_t len)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t avail = available_for_read_unlocked();
        if (avail == 0 || len == 0) return 0;

        size_t n = (len < avail) ? len : avail;
        tail_ = (tail_ + n) & MASK;
        return n;
    }

    // 读走全部并消费
    size_t read_all(std::vector<uint8_t>& out)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t avail = available_for_read_unlocked();
        if (avail == 0) return 0;

        out.resize(avail);
        size_t first_chunk = SIZE - tail_;
        if (avail <= first_chunk)
        {
            memcpy(out.data(), buf_ + tail_, avail);
        }
        else
        {
            memcpy(out.data(), buf_ + tail_, first_chunk);
            memcpy(out.data() + first_chunk, buf_, avail - first_chunk);
        }
        tail_ = (tail_ + avail) & MASK;
        return avail;
    }

    // 调试
    size_t head_pos() const { return head_; }
    size_t tail_pos() const { return tail_; }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        head_ = 0;
        tail_ = 0;
    }

private:
    //计算剩余空间
    size_t available_for_write_unlocked() const
    {
        // 保留 1 字节，head==tail 永远表示"空"
        return (tail_ - head_ - 1) & MASK;
    }

    size_t available_for_read_unlocked() const
    {
        return (head_ - tail_) & MASK;
    }

    uint8_t    buf_[SIZE];
    size_t     head_;   // 生产者下一写入位置
    size_t     tail_;   // 消费者下一读取位置
    std::mutex mtx_;
};

#endif
