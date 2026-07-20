#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <cstring>
#include <iomanip>

#define UART_DEV "/dev/ttyTHS1"
#define BAUDRATE B460800
// #define BAUDRATE B115200

// 打开串口（只读）
int uart_open(const char* dev)
{
    int fd = open(dev,O_RDONLY | O_NOCTTY | O_SYNC);
    if(fd < 0){
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
    // 波特率
    cfsetispeed(&tty,BAUDRATE);

    cfsetospeed(&tty,BAUDRATE);
    // 8 数据位
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    // 无校验
    tty.c_cflag &= ~PARENB;
    // 1停止位
    tty.c_cflag &= ~CSTOPB;
    // 允许接收
    tty.c_cflag |= CREAD;
    // 本地模式
    tty.c_cflag |= CLOCAL;

    // 关闭硬件流控
    tty.c_cflag &= ~CRTSCTS;

    // 原始模式
    tty.c_lflag &= ~(ICANON |ECHO |ECHOE |ISIG);

    tty.c_iflag &= ~(IXON |IXOFF |IXANY);

    tty.c_oflag &= ~OPOST;

    // read阻塞设置
    // VMIN=1 表示至少收到1字节才返回
    tty.c_cc[VMIN]=1;
    tty.c_cc[VTIME]=2; 

    tcflush(fd,TCIFLUSH);

    if(tcsetattr(fd,TCSANOW,&tty)!=0){
        perror("tcsetattr");
        return false;
    }
    return true;
}
// HEX打印
void print_hex(unsigned char* buf,int len)
{
    for(int i=0;i<len;i++)
    {
        std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)buf[i]<<" ";
        // 每16字节换行
        if((i+1)%16==0)
            std::cout<<std::endl;
    }
    std::cout<<std::dec<<std::endl;
}

int main()
{
    int fd = uart_open(UART_DEV);

    if(fd<0)
        return -1;

    if(!uart_config(fd))
    {
        close(fd);
        return -1;
    }

    std::cout<<"UART RX ONLY READY"<<std::endl;

    std::cout<<"device:"<<UART_DEV<<std::endl;

    std::cout<<"baudrate:460800"<<std::endl;


    const int EXPECT_SIZE = 1050;

    unsigned char all_buf[4096];
    int total = 0;

    std::cout
    <<"waiting data..."
    <<std::endl;

    while(total < EXPECT_SIZE)
    {

        int n = read(fd,all_buf + total,sizeof(all_buf)-total);
        if(n > 0)
        {
            total += n;
            std::cout<<"receive "<<total<<" bytes"<<std::endl;
        }else{
            break;
        }
    }

    std::cout
    <<"\n===================="
    <<std::endl;


    std::cout
    <<"receive complete"
    <<std::endl;


    std::cout
    <<"total bytes:"
    <<total
    <<std::endl;



    std::cout
    <<"===================="
    <<std::endl;


    print_hex(all_buf,total);

    close(fd);

    return 0;
}
