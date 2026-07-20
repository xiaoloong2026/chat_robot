#include <iostream>
#include <fstream>
#include <string>
#include <cctype>


int main(int argc, char* argv[])
{

    if(argc != 2)
    {
        std::cout
        << "用法: "
        << argv[0]
        << " xxx.txt"
        << std::endl;

        return -1;
    }


    std::ifstream file(argv[1]);

    if(!file.is_open())
    {
        std::cerr
        << "无法打开文件:"
        << argv[1]
        << std::endl;

        return -1;
    }


    std::string hex;


    char c;

    while(file.get(c))
    {
        // 自动忽略空格、换行、Tab
        if(!isspace((unsigned char)c))
        {
            hex += c;
        }
    }


    file.close();



    // HEX长度必须为偶数

    if(hex.size() % 2 != 0)
    {
        std::cout
        << "错误: HEX字符数量不是偶数"
        << std::endl;

        std::cout
        << "HEX字符数量:"
        << hex.size()
        << std::endl;

        return -1;
    }



    // 检查字符合法性

    for(char c:hex)
    {
        if(!isxdigit((unsigned char)c))
        {
            std::cout
            << "错误字符:"
            << c
            << std::endl;

            return -1;
        }
    }



    size_t bytes = hex.size()/2;



    std::cout
    << "===================="
    << std::endl;


    std::cout
    << "文件:"
    << argv[1]
    << std::endl;


    std::cout
    << "HEX字符数量:"
    << hex.size()
    << std::endl;


    std::cout
    << "实际字节数量:"
    << bytes
    << " bytes"
    << std::endl;


    std::cout
    << "===================="
    << std::endl;



    return 0;
}