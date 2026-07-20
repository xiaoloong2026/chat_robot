cmake_minimum_required(VERSION 3.10)


project(uart_test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 头文件路径
include_directories(
    include
)

# 生成可执行文件
add_executable(uart_test
    src/main.cpp
    src/uart.cpp
    src/parser.cpp
)

# std::thread 需要 pthread
target_link_libraries(uart_test pthread)