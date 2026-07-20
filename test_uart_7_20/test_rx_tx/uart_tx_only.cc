#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <cstring>
#include <vector>
#include <iomanip>

#define UART_DEV "/dev/ttyTHS1"

#define BAUDRATE B460800
// #define BAUDRATE B115200

// 初始化串口
int uart_open(const char* dev)
{
    int fd = open(dev, O_WRONLY | O_NOCTTY);
    if(fd < 0)
    {
        perror("open uart");
        return -1;
    }
    return fd;
}

bool uart_config(int fd)
{
    struct termios tty{};
    if(tcgetattr(fd,&tty)!=0)
    {
        perror("tcgetattr");
        return false;
    }
    // 设置波特率
    cfsetispeed(&tty, BAUDRATE);
    cfsetospeed(&tty, BAUDRATE);
    // 8数据位
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;

    // 无校验
    tty.c_cflag &= ~PARENB;
    // 1停止位
    tty.c_cflag &= ~CSTOPB;
    // 开启发送
    tty.c_cflag |= CLOCAL;
    // 原始模式
    tty.c_lflag = 0;
    tty.c_iflag = 0;
    tty.c_oflag = 0;

    tcflush(fd, TCIOFLUSH);
    if(tcsetattr(fd,TCSANOW,&tty)!=0)
    {
        perror("tcsetattr");
        return false;
    }
    return true;
}
// 打印16进制
void print_hex(const std::vector<unsigned char>& data){
    for(auto b:data)
    {
        std::cout
        << std::hex
        << std::setw(2)
        << std::setfill('0')
        << (int)b
        << " ";
    }
    std::cout<<std::dec<<std::endl;
}

//判断是否为HEX格式
bool is_hex_string(const std::string& str)
{
    // 去除空格
    std::string s;
    for(char c : str)
    {
        if(c != ' ')
            s += c;
    }
    // 长度必须偶数
    if(s.size() % 2 != 0)
        return false;
    // 至少一个字节
    if(s.empty())
        return false;
    // 全部必须是HEX字符
    for(char c:s)
    {
        if(!std::isxdigit(c))
            return false;
    }
    return true;
}
// HEX字符串转换为真实字节
std::vector<unsigned char> hex_to_bytes(
        const std::string& input)
{
    std::vector<unsigned char> data;
    std::string hex;
    // 去掉空格
    for(char c:input)
    {
        if(c!=' ')
            hex+=c;
    }
    for(size_t i=0;i<hex.size();i+=2)
    {

        std::string byte_str =
            hex.substr(i,2);
        unsigned int value;
        sscanf(
            byte_str.c_str(),
            "%02x",
            &value
        );
        data.push_back(
            (unsigned char)value
        );
    }
    return data;
}

int main()
{
    int fd = uart_open(UART_DEV);
    if(fd < 0)
        return -1;

    if(!uart_config(fd))
    {
        close(fd);
        return -1;
    }
    std::cout<<"UART TX ready"<<std::endl;
    std::cout<<"baudrate:460800"<<std::endl;

    while(true)
    {

        std::string text;
        std::cout<<"\n请输入发送内容:";

        std::getline(
            std::cin,
            text
        );

        // UTF-8字符串本身就是字节流
        std::vector<unsigned char> data;
        if(is_hex_string(text))
        {
            std::cout<<"HEX模式"<<std::endl;
            data = hex_to_bytes(text);
        }
        else
        {
            std::cout<<"UTF-8文本模式"<<std::endl;
            data.assign(
                text.begin(),
                text.end()
            );
        }

        int send_len = data.size();
        std::cout<<"准备发送:"<<send_len<<" bytes"<<std::endl;
        std::cout<<"发送HEX:"<<std::endl;
        print_hex(data);

        
        //发送
        int ret = write(fd,data.data(),data.size()
        );
        if(ret < 0)
        {
            perror("write");
        }
        else
        {
            std::cout
            <<"写入UART驱动 "
            <<ret
            <<" 字节"
            <<std::endl;


            // 等待UART硬件发送完成
            tcdrain(fd);


            std::cout
            <<"UART实际发送完成"
            <<std::endl;
        }


    }
    close(fd);
    return 0;
}