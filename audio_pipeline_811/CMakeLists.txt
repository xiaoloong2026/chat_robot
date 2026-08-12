cmake_minimum_required(VERSION 3.16)
project(audio_pipeline VERSION 1.0.0 LANGUAGES CXX)

include(CTest)

# C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 优化选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# Boost (仅头文件，用于 property_tree JSON 解析)
find_package(Boost REQUIRED)

# 源文件
set(SOURCES
    src/main.cpp
    src/asr_client.cpp
    src/json_helper.cpp
    src/llm_client.cpp
    src/opus_accumulator.cpp
    src/tts_client.cpp
    src/uart.cpp
    src/uart_protocol.cpp
    src/uart_sender.cpp
    src/parser.cpp
)

set(HEADERS
    src/asr_client.h
    src/config.h
    src/json_helper.h
    src/llm_client.h
    src/opus_accumulator.h
    src/tts_client.h
    src/uart.hpp
    src/uart_protocol.hpp
    src/uart_sender.h
    src/parser.hpp
    src/ring_buffer.hpp
)

# 可执行目标
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# pthread (UART 后台线程), dl (dlopen libopus)
target_link_libraries(${PROJECT_NAME} pthread dl)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Boost_INCLUDE_DIRS}
)

# 安装
install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# 打印配置摘要
message(STATUS "Boost include: ${Boost_INCLUDE_DIRS}")
message(STATUS "Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ compiler:  ${CMAKE_CXX_COMPILER}")

if(BUILD_TESTING)
    add_executable(uart_protocol_test
        tests/uart_protocol_test.cpp
        src/parser.cpp
        src/uart.cpp
        src/uart_protocol.cpp
        src/uart_sender.cpp
    )
    target_link_libraries(uart_protocol_test pthread)
    target_include_directories(uart_protocol_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME uart_protocol COMMAND uart_protocol_test)
endif()
