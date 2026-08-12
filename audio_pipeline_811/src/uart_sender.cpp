#include "uart_sender.h"
#include "uart_protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <mutex>
#include <termios.h>
#include <unistd.h>

#include <iomanip>

static constexpr int      FRAME_INTERVAL = 200000;  // 帧间间隔 200ms (us)

// 串口文件描述符
static int g_uart_fd = -1;
static uint8_t g_sequence = 0;
static std::mutex g_send_mutex;
static std::atomic<uint64_t> g_send_generation{0};
static std::chrono::steady_clock::time_point g_input_start;
static uint64_t g_latency_generation = 0;
static bool g_latency_start_valid = false;
static bool g_first_mp3_reported = false;
static bool g_response_failed = false;

struct HeldMp3Tail {
    uint64_t generation = 0;
    uint16_t dsid = 1;
    std::vector<uint8_t> data;
};
static HeldMp3Tail g_held_mp3_tail;
static uint32_t g_next_mp3_dsid = 1;

// ============================================================================
// 内部辅助
// ============================================================================

static bool take_next_mp3_dsid(uint16_t& value) {
    if (g_next_mp3_dsid > 0xFFFF) return false;
    value = static_cast<uint16_t>(g_next_mp3_dsid++);
    return true;
}

//调试打印函数
static void print_uart_tx_raw(const uint8_t* data, size_t size)
{
    std::cerr
        << "\n[UART TX RAW] "
        << size
        << " bytes"
        << std::endl;

    for (size_t i = 0; i < size; ++i)
    {
        // 每16字节换一行
        if (i % 16 == 0)
        {
            std::cerr << "  ";
        }

        std::cerr
            << std::uppercase
            << std::hex
            << std::setw(2)
            << std::setfill('0')
            << static_cast<unsigned>(data[i])
            << " ";

        if ((i + 1) % 16 == 0 || i + 1 == size)
        {
            std::cerr << std::endl;
        }
    }

    // 恢复十进制，避免影响后面的日志
    std::cerr << std::dec;
}


/// 发送一帧: AA 55 + type + seq + dsid + state + len + payload + check
static bool write_all(const uint8_t* data, size_t size) {

    //调试打印
    // print_uart_tx_raw(data, size);


    size_t written = 0;
    while (written < size) {
        ssize_t n = write(g_uart_fd, data + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            perror("write uart");
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

static bool send_frame(uint8_t type, uint16_t dsid, uint8_t state,
                       const uint8_t* payload, uint16_t len) {
    if (g_uart_fd < 0) return false;

    std::vector<uint8_t> frame;
    try {
        frame = uart_protocol::encode_frame(
            type, g_sequence++, dsid, state, payload, len);
    } catch (const std::exception& e) {
        std::cerr << "[UART↓] 组帧失败: " << e.what() << std::endl;
        return false;
    }

    if (!write_all(frame.data(), frame.size())) return false;
    int drain_result = 0;
    do {
        drain_result = tcdrain(g_uart_fd);
    } while (drain_result != 0 && errno == EINTR);
    // 单元测试使用 pipe（ENOTTY）；真实 TTY 的 EIO/掉线必须上传。
    if (drain_result != 0 && errno != ENOTTY) {
        perror("tcdrain uart");
        return false;
    }
    if (type == uart_protocol::TYPE_TTS_AUDIO && g_latency_start_valid &&
        !g_first_mp3_reported &&
        g_latency_generation == g_send_generation.load()) {
        g_first_mp3_reported = true;
        const auto latency_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - g_input_start)
                .count();
        std::cerr << "[端到端延迟] UART输入首帧→首个MP3分片发送完成: "
                  << latency_ms << " ms" << std::endl;
    }
    usleep(FRAME_INTERVAL);
    return true;
}

static bool send_payload_locked(uint8_t type, uint8_t state,
                                const uint8_t* data, size_t len,
                                const char* label, uint64_t generation) {
    if (g_uart_fd < 0 || data == nullptr || len == 0) return false;
    if (type != uart_protocol::TYPE_TTS_AUDIO &&
        !uart_protocol::is_valid_utf8(data, len)) {
        std::cerr << "[UART↓] 拒绝非 UTF-8 文本: " << label << std::endl;
        return false;
    }

    size_t offset = 0;
    uint16_t dsid = 1;
    while (offset < len) {
        if (generation != g_send_generation.load()) {
            std::cerr << "[UART↓] 当前下发已中断: " << label << std::endl;
            return false;
        }
        const size_t chunk = std::min(
            len - offset, uart_protocol::MAX_CODE_SIZE);
        if (!send_frame(type, dsid, state, data + offset,
                        static_cast<uint16_t>(chunk)))
            return false;
        offset += chunk;
        if (offset < len) {
            if (dsid == 0xFFFF) {
                std::cerr << "[UART↓] DSID 已用尽: " << label << std::endl;
                return false;
            }
            ++dsid;
        }
    }
    std::cerr << "[UART↓] " << label << ": " << len << " bytes" << std::endl;
    return true;
}

static bool send_payload(uint8_t type, uint8_t state,
                         const uint8_t* data, size_t len,
                         const char* label) {
    if (g_uart_fd < 0 || data == nullptr || len == 0) return false;
    const uint64_t generation = g_send_generation.load();
    std::lock_guard<std::mutex> lk(g_send_mutex);
    return send_payload_locked(type, state, data, len, label, generation);
}

// ============================================================================
// 公开接口
// ============================================================================

void uart_sender_set_fd(int fd) {
    std::lock_guard<std::mutex> lk(g_send_mutex);
    g_uart_fd = fd;
    g_sequence = 0;
}

void uart_send_raw(const std::vector<uint8_t>& data) {
    if (g_uart_fd < 0) return;
    std::lock_guard<std::mutex> lk(g_send_mutex);
    write_all(data.data(), data.size());
}

bool uart_send_control(const std::string& json) {
    return send_payload(uart_protocol::TYPE_CONTROL, 0,
                        reinterpret_cast<const uint8_t*>(json.data()),
                        json.size(), "控制指令 TYPE=0x12");
}

bool uart_send_question_text(const std::string& text) {
    return send_payload(uart_protocol::TYPE_TEXT,
                        uart_protocol::TEXT_STATE_QUESTION,
                        reinterpret_cast<const uint8_t*>(text.data()),
                        text.size(), "提问文本 TYPE=0x13 STATE=0");
}

bool uart_send_segment_text(const std::string& text) {
    return send_payload(uart_protocol::TYPE_TEXT,
                        uart_protocol::TEXT_STATE_ANSWER,
                        reinterpret_cast<const uint8_t*>(text.data()),
                        text.size(), "回答文本 TYPE=0x13 STATE=1");
}

bool uart_send_ai_info(const std::string& json) {
    // 0x14 STATE 在协议中仍为“待定”，兼容期固定使用 0。
    return send_payload(uart_protocol::TYPE_AI_INFO, 0,
                        reinterpret_cast<const uint8_t*>(json.data()),
                        json.size(), "AI信息 TYPE=0x14");
}

bool uart_send_text(const std::string& text) {
    return uart_send_control(text);
}

void uart_send_mp3(const std::vector<uint8_t>& mp3_data) {
    if (g_uart_fd < 0 || mp3_data.empty()) return;
    const uint64_t generation = g_send_generation.load();
    std::lock_guard<std::mutex> lk(g_send_mutex);
    size_t offset = 0;
    uint16_t dsid = 1;
    while (offset < mp3_data.size()) {
        if (generation != g_send_generation.load()) return;
        const size_t chunk = std::min(
            mp3_data.size() - offset, uart_protocol::MAX_CODE_SIZE);
        const bool is_last = offset + chunk == mp3_data.size();
        if (!send_frame(uart_protocol::TYPE_TTS_AUDIO, dsid,
                        is_last ? uart_protocol::AUDIO_STATE_END
                                : uart_protocol::AUDIO_STATE_NORMAL,
                        mp3_data.data() + offset,
                        static_cast<uint16_t>(chunk)))
            return;
        offset += chunk;
        if (offset < mp3_data.size()) {
            if (dsid == 0xFFFF) {
                std::cerr << "[UART↓] MP3 DSID 已用尽" << std::endl;
                return;
            }
            ++dsid;
        }
    }
    std::cerr << "[UART↓] MP3 TYPE=0x11: " << mp3_data.size()
              << " bytes，最后一帧 STATE=1" << std::endl;
}

void uart_sender_cancel_current() {
    g_send_generation.fetch_add(1);
}

uint64_t uart_sender_current_generation() {
    return g_send_generation.load();
}

void uart_sender_set_input_start(
    std::chrono::steady_clock::time_point start) {
    std::lock_guard<std::mutex> lk(g_send_mutex);
    g_held_mp3_tail.data.clear();
    g_next_mp3_dsid = 1;
    g_response_failed = false;
    g_input_start = start;
    g_latency_generation = g_send_generation.load();
    g_latency_start_valid = true;
    g_first_mp3_reported = false;
}

bool uart_send_segment(const std::string& text,
                       const std::string& mp3_path,
                       uint64_t expected_generation) {
    if (expected_generation != UINT64_MAX &&
        expected_generation != g_send_generation.load()) {
        std::cerr << "[UART↓] 丢弃迟到的旧 TTS 结果" << std::endl;
        return false;
    }
    std::ifstream file(mp3_path, std::ios::binary);
    if (!file) {
        std::cerr << "[UART↓] 无法打开 TTS MP3: " << mp3_path << std::endl;
        std::lock_guard<std::mutex> lk(g_send_mutex);
        if (expected_generation == UINT64_MAX ||
            expected_generation == g_send_generation.load())
            g_response_failed = true;
        return false;
    }
    std::vector<uint8_t> mp3((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    if (mp3.empty()) {
        std::cerr << "[UART↓] TTS MP3 为空: " << mp3_path << std::endl;
        std::lock_guard<std::mutex> lk(g_send_mutex);
        if (expected_generation == UINT64_MAX ||
            expected_generation == g_send_generation.load())
            g_response_failed = true;
        return false;
    }
    std::lock_guard<std::mutex> lk(g_send_mutex);
    const uint64_t generation = g_send_generation.load();
    if (expected_generation != UINT64_MAX &&
        expected_generation != generation) {
        std::cerr << "[UART↓] 丢弃迟到的旧 TTS 结果" << std::endl;
        return false;
    }
    if (g_response_failed) {
        std::cerr << "[UART↓] 本轮已有分段失败，拒绝继续下发"
                  << std::endl;
        return false;
    }

    // 新的一段到来，说明上一段并非整轮最后一段。
    if (!g_held_mp3_tail.data.empty()) {
        if (g_held_mp3_tail.generation == generation) {
            if (!send_frame(uart_protocol::TYPE_TTS_AUDIO,
                            g_held_mp3_tail.dsid,
                            uart_protocol::AUDIO_STATE_NORMAL,
                            g_held_mp3_tail.data.data(),
                            static_cast<uint16_t>(g_held_mp3_tail.data.size()))) {
                g_response_failed = true;
                return false;
            }
        }
        g_held_mp3_tail.data.clear();
    }

    if (!send_payload_locked(uart_protocol::TYPE_TEXT,
            uart_protocol::TEXT_STATE_ANSWER,
            reinterpret_cast<const uint8_t*>(text.data()), text.size(),
            "回答文本 TYPE=0x13 STATE=1", generation)) {
        g_response_failed = true;
        return false;
    }

    // 除末片外立即发送。末片暂存，等下一段或整轮结束后确定 STATE。
    size_t offset = 0;
    while (mp3.size() - offset > uart_protocol::MAX_CODE_SIZE) {
        if (generation != g_send_generation.load()) {
            std::cerr << "[UART↓] 当前下发已中断: MP3 TYPE=0x11"
                      << std::endl;
            return false;
        }
        uint16_t dsid = 0;
        if (!take_next_mp3_dsid(dsid)) {
            std::cerr << "[UART↓] MP3 DSID 已用尽" << std::endl;
            g_response_failed = true;
            return false;
        }
        if (!send_frame(uart_protocol::TYPE_TTS_AUDIO, dsid,
                        uart_protocol::AUDIO_STATE_NORMAL,
                        mp3.data() + offset,
                        static_cast<uint16_t>(uart_protocol::MAX_CODE_SIZE))) {
            g_response_failed = true;
            return false;
        }
        offset += uart_protocol::MAX_CODE_SIZE;
    }
    g_held_mp3_tail.generation = generation;
    if (!take_next_mp3_dsid(g_held_mp3_tail.dsid)) {
        std::cerr << "[UART↓] MP3 DSID 已用尽" << std::endl;
        g_response_failed = true;
        return false;
    }
    g_held_mp3_tail.data.assign(mp3.begin() + offset, mp3.end());
    std::cerr << "[UART↓] MP3主体 TYPE=0x11: " << offset
              << " bytes，末片 " << g_held_mp3_tail.data.size()
              << " bytes 待确认" << std::endl;
    return true;
}

bool uart_finish_response() {
    std::lock_guard<std::mutex> lk(g_send_mutex);
    const uint64_t generation = g_send_generation.load();
    if (g_response_failed || g_held_mp3_tail.data.empty() ||
        g_held_mp3_tail.generation != generation) {
        g_held_mp3_tail.data.clear();
        g_next_mp3_dsid = 1;
        return false;
    }
    const bool sent = send_frame(
        uart_protocol::TYPE_TTS_AUDIO,
        g_held_mp3_tail.dsid,
        uart_protocol::AUDIO_STATE_END,
        g_held_mp3_tail.data.data(),
        static_cast<uint16_t>(g_held_mp3_tail.data.size()));
    if (sent) {
        std::cerr << "[UART↓] 整轮最后MP3末片: "
                  << g_held_mp3_tail.data.size()
                  << " bytes, DSID=" << g_held_mp3_tail.dsid
                  << ", STATE=1" << std::endl;
    }
    g_held_mp3_tail.data.clear();
    g_next_mp3_dsid = 1;
    g_response_failed = !sent;
    return sent;
}
