#include "uart.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

//调试用
#include <iostream>
#include <iomanip>

// ============================================================
//  构造 / 析构 / 基本操作
// ============================================================

UART::UART(const std::string& dev, speed_t baud)
    : device_(dev), baudrate_(baud), fd_(-1)
{
}

UART::~UART()
{
    stop_receive();
    close();
}

bool UART::open()
{
    fd_ = ::open(device_.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0)
    {
        perror("open uart");
        return false;
    }

    struct termios tty{};
    if (tcgetattr(fd_, &tty) != 0)
    {
        perror("tcgetattr");
        return false;
    }

    cfsetispeed(&tty, baudrate_);
    cfsetospeed(&tty, baudrate_);

    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;          // 8 数据位
    tty.c_cflag &= ~PARENB;      // 无校验
    tty.c_cflag &= ~CSTOPB;      // 1 停止位
    tty.c_cflag |= CREAD;        // 启用接收
    tty.c_cflag |= CLOCAL;       // 忽略调制解调器控制线
    tty.c_cflag &= ~CRTSCTS;     // 无硬件流控

    tty.c_lflag = 0;             // 非规范模式
    tty.c_iflag = 0;             // 原始输入
    tty.c_oflag = 0;             // 原始输出

    tty.c_cc[VMIN]  = 1;         // 至少 1 字节即返回
    tty.c_cc[VTIME] = 0;         // 无超时（有数据立即返回）

    tcflush(fd_, TCIOFLUSH);
    return tcsetattr(fd_, TCSANOW, &tty) == 0;
}

int UART::send(const std::vector<uint8_t>& data)
{
    int total = 0;
    while (total < static_cast<int>(data.size()))
    {
        int n = write(fd_, data.data() + total, data.size() - total);
        if (n < 0)
        {
            perror("write");
            return -1;
        }
        total += n;
    }
    tcdrain(fd_);
    return total;
}

void UART::close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UART::is_open() const
{
    return fd_ >= 0;
}

// ============================================================
//  生产者：后台线程持续读串口 → 写入环形缓冲区
// ============================================================

void UART::start_receive()
{
    if (rx_running_)
        return;

    rx_ring_.clear();
    rx_running_ = true;
    rx_thread_  = std::thread(&UART::rx_loop, this);
}

void UART::stop_receive()
{
    rx_running_ = false;
    if (rx_thread_.joinable())
        rx_thread_.join();
}
//读线程一直读
void UART::rx_loop()
{
    uint8_t buf[256];

    while (rx_running_)
    {
        int n = read(fd_, buf, sizeof(buf));



        if (n > 0)
        {
            // 调试打印
            // std::cerr
            //     << "[UART RAW RX] "
            //     << n
            //     << " bytes: ";

            // for (int i = 0; i < n; ++i)
            // {
            //     std::cerr
            //         << std::hex
            //         << std::setw(2)
            //         << std::setfill('0')
            //         << static_cast<unsigned>(buf[i])
            //         << " ";
            // }

            // std::cerr
            //     << std::dec
            //     << std::endl;

            // 生产者：将收到的每个字节写入环形缓冲区
            rx_ring_.write(buf, static_cast<size_t>(n));
        }
        else if (n < 0)
        {
            // 读错误，短暂等待后重试
            usleep(1000);
        }
    }
}
