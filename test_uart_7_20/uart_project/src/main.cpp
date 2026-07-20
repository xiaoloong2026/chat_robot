#include "uart.hpp"
#include "parser.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>

static UART* g_uart = nullptr;

// ----------------------------------------------------------
//  信号处理
// ----------------------------------------------------------
void signal_handler(int)
{
    std::cout << "\nstopping..." << std::endl;
    if (g_uart)
        g_uart->stop_receive();
    exit(0);
}

// ----------------------------------------------------------
//  main
// ----------------------------------------------------------
int main()
{
    signal(SIGINT, signal_handler);

    // 1. 打开串口
    UART uart("/dev/ttyUSB0", B460800);
    g_uart = &uart;

    if (!uart.open())
    {
        std::cerr << "Failed to open UART" << std::endl;
        return -1;
    }

    // 2. 启动生产者（后台线程 → 串口数据写入环形缓冲区）
    uart.start_receive();
    std::cout << "UART rx thread started. Ring buffer: "
              << RingBuffer::SIZE << " bytes" << std::endl;

    // 3. 解析器（从环形缓冲区读数据 → 解析 → 应答）
    Parser parser(uart.rx_buffer(), uart);

    // 4. 主循环：30ms 轮询一次
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        parser.process();
    }

    return 0;
}
