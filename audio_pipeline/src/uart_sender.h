#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// UART 下行发送 (AI → MCU)
// ============================================================================

/// 设置 UART 文件描述符
void uart_sender_set_fd(int fd);

/// 发送原始字节
void uart_send_raw(const std::vector<uint8_t>& data);

/// 发送控制指令 (TYPE 0x12 JSON/UTF-8)
void uart_send_control(const std::string& json);

/// 发送每段音频对应文本 (TYPE 0x13 UTF-8)
void uart_send_segment_text(const std::string& text);

/// 兼容旧调用：等价于 uart_send_control()
void uart_send_text(const std::string& text);

/// 发送 MP3 音频帧 (TYPE 0x11)，按 1024 字节切片
void uart_send_mp3(const std::vector<uint8_t>& mp3_data);

/// 立即终止当前正在分片下发的文本/MP3；后续新任务不受影响。
void uart_sender_cancel_current();

/// 设置本轮UART输入起点，用于统计首个0x11音频分片输出延迟。
void uart_sender_set_input_start(
    std::chrono::steady_clock::time_point start);

/// 原子发送一段“文本 0x13 + 对应 MP3 0x11”。
void uart_send_segment(const std::string& text, const std::string& mp3_path);

/// 发送整轮回复最后一个 MP3 的末片，并设置 STATE=1。
void uart_finish_response();
