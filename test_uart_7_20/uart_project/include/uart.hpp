#ifndef UART_HPP
#define UART_HPP

#include "ring_buffer.hpp"

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>
#include <termios.h>

class UART
{
public:
    UART(const std::string& dev, speed_t baud);
    ~UART();

    bool open();
    int  send(const std::vector<uint8_t>& data);
    void close();
    bool is_open() const;

    // 启动 / 停止后台接收线程（生产者）
    void start_receive();
    void stop_receive();

    // 获取环形缓冲区的引用（消费者通过它读取数据）
    RingBuffer& rx_buffer() { return rx_ring_; }

private:
    std::string device_;
    speed_t     baudrate_;
    int         fd_;

    // 4096 字节环形缓冲区（生产者写入，消费者读取）
    RingBuffer rx_ring_;

    // 后台接收线程
    std::thread         rx_thread_;
    std::atomic<bool>   rx_running_{false};

    void rx_loop();   // 生产者：串口 → 环形缓冲区
};

#endif
