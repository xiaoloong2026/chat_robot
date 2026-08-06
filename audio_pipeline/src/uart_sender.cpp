#include "uart_sender.h"

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

// ============================================================================
// 协议常量
// ============================================================================

static constexpr uint8_t  SYNC0          = 0xAA;
static constexpr uint8_t  SYNC1          = 0x55;
static constexpr uint8_t  TYPE_MP3       = 0x11;   // AI→MCU: TTS 播报音频
static constexpr uint8_t  TYPE_CONTROL   = 0x12;   // AI→MCU: 控制指令 JSON
static constexpr uint8_t  TYPE_SEG_TEXT  = 0x13;   // AI→MCU: 本段音频对应文本
static constexpr uint16_t MAX_PAYLOAD    = 1024;   // 单帧最大载荷
static constexpr int      FRAME_INTERVAL = 30000;  // 帧间间隔 30ms (us)

// 串口文件描述符
static int g_uart_fd = -1;
static uint8_t g_sequence = 0;
static std::mutex g_send_mutex;
static std::atomic<uint64_t> g_send_generation{0};
static std::chrono::steady_clock::time_point g_input_start;
static uint64_t g_latency_generation = 0;
static bool g_latency_start_valid = false;
static bool g_first_mp3_reported = false;

struct HeldMp3Tail {
    uint64_t generation = 0;
    std::vector<uint8_t> data;
};
static HeldMp3Tail g_held_mp3_tail;

// ============================================================================
// 内部辅助
// ============================================================================

static void write_le16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
}

static uint8_t calc_check(const std::vector<uint8_t>& data, size_t start) {
    uint8_t sum = 0;
    for (size_t i = start; i < data.size(); ++i)
        sum += data[i];
    return sum;
}

/// 发送一帧: AA 55 + type + seq + dsid + state + len + payload + check
static bool write_all(const uint8_t* data, size_t size) {
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
    frame.reserve(10 + len);
    frame.push_back(SYNC0);        // AA
    frame.push_back(SYNC1);        // 55
    frame.push_back(type);         // TYPE
    frame.push_back(g_sequence++); // SEQ: 所有下行帧全局递增并循环
    write_le16(frame, dsid);       // DSID (LE)
    frame.push_back(state);        // STATE
    write_le16(frame, len);        // LEN (LE)
    // CODE
    for (uint16_t i = 0; i < len; ++i)
        frame.push_back(payload[i]);
    // CHECK
    frame.push_back(calc_check(frame, 2));  // from TYPE to end of CODE

    if (!write_all(frame.data(), frame.size())) return false;
    tcdrain(g_uart_fd);
    if (type == TYPE_MP3 && g_latency_start_valid &&
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

static bool send_payload_locked(uint8_t type, const uint8_t* data, size_t len,
                                const char* label, uint64_t generation) {
    if (g_uart_fd < 0 || data == nullptr || len == 0) return false;

    size_t offset = 0;
    uint16_t slice = 1;
    while (offset < len) {
        if (generation != g_send_generation.load()) {
            std::cerr << "[UART↓] 当前下发已中断: " << label << std::endl;
            return false;
        }
        const size_t chunk = std::min(len - offset,
                                      static_cast<size_t>(MAX_PAYLOAD));
        const bool is_last = offset + chunk >= len;
        // 协议规定最后一片 DSID 清零；下行 STATE 尚未定义，固定为 0。
        const uint16_t dsid = is_last ? 0 : slice++;
        if (!send_frame(type, dsid, 0, data + offset,
                        static_cast<uint16_t>(chunk)))
            return false;
        offset += chunk;
    }
    std::cerr << "[UART↓] " << label << ": " << len << " bytes" << std::endl;
    return true;
}

static void send_payload(uint8_t type, const uint8_t* data, size_t len,
                         const char* label) {
    if (g_uart_fd < 0 || data == nullptr || len == 0) return;
    const uint64_t generation = g_send_generation.load();
    std::lock_guard<std::mutex> lk(g_send_mutex);
    send_payload_locked(type, data, len, label, generation);
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

void uart_send_control(const std::string& json) {
    send_payload(TYPE_CONTROL,
                 reinterpret_cast<const uint8_t*>(json.data()),
                 json.size(), "控制指令 TYPE=0x12");
}

void uart_send_segment_text(const std::string& text) {
    send_payload(TYPE_SEG_TEXT,
                 reinterpret_cast<const uint8_t*>(text.data()),
                 text.size(), "分段文本 TYPE=0x13");
}

void uart_send_text(const std::string& text) {
    uart_send_control(text);
}

void uart_send_mp3(const std::vector<uint8_t>& mp3_data) {
    if (g_uart_fd < 0 || mp3_data.empty()) return;
    const uint64_t generation = g_send_generation.load();
    std::lock_guard<std::mutex> lk(g_send_mutex);
    size_t offset = 0;
    uint16_t slice = 1;
    while (offset < mp3_data.size()) {
        if (generation != g_send_generation.load()) return;
        const size_t chunk = std::min(
            mp3_data.size() - offset, static_cast<size_t>(MAX_PAYLOAD));
        const bool is_last = offset + chunk == mp3_data.size();
        if (!send_frame(TYPE_MP3, is_last ? 0 : slice++,
                        is_last ? 1 : 0, mp3_data.data() + offset,
                        static_cast<uint16_t>(chunk)))
            return;
        offset += chunk;
    }
    std::cerr << "[UART↓] MP3 TYPE=0x11: " << mp3_data.size()
              << " bytes，最后一帧 STATE=1" << std::endl;
}

void uart_sender_cancel_current() {
    g_send_generation.fetch_add(1);
}

void uart_sender_set_input_start(
    std::chrono::steady_clock::time_point start) {
    std::lock_guard<std::mutex> lk(g_send_mutex);
    g_input_start = start;
    g_latency_generation = g_send_generation.load();
    g_latency_start_valid = true;
    g_first_mp3_reported = false;
}

void uart_send_segment(const std::string& text, const std::string& mp3_path) {
    const uint64_t generation = g_send_generation.load();
    std::ifstream file(mp3_path, std::ios::binary);
    if (!file) {
        std::cerr << "[UART↓] 无法打开 TTS MP3: " << mp3_path << std::endl;
        return;
    }
    std::vector<uint8_t> mp3((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    if (mp3.empty()) {
        std::cerr << "[UART↓] TTS MP3 为空: " << mp3_path << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lk(g_send_mutex);

    // 新的一段到来，说明上一段并非整轮最后一段。
    if (!g_held_mp3_tail.data.empty()) {
        if (g_held_mp3_tail.generation == generation) {
            if (!send_frame(TYPE_MP3, 0, 0, g_held_mp3_tail.data.data(),
                            static_cast<uint16_t>(g_held_mp3_tail.data.size())))
                return;
        }
        g_held_mp3_tail.data.clear();
    }

    if (!send_payload_locked(TYPE_SEG_TEXT,
            reinterpret_cast<const uint8_t*>(text.data()), text.size(),
            "分段文本 TYPE=0x13", generation))
        return;

    // 除末片外立即发送。末片暂存，等下一段或整轮结束后确定 STATE。
    size_t offset = 0;
    uint16_t slice = 1;
    while (mp3.size() - offset > MAX_PAYLOAD) {
        if (generation != g_send_generation.load()) {
            std::cerr << "[UART↓] 当前下发已中断: MP3 TYPE=0x11"
                      << std::endl;
            return;
        }
        if (!send_frame(TYPE_MP3, slice++, 0, mp3.data() + offset,
                        MAX_PAYLOAD))
            return;
        offset += MAX_PAYLOAD;
    }
    g_held_mp3_tail.generation = generation;
    g_held_mp3_tail.data.assign(mp3.begin() + offset, mp3.end());
    std::cerr << "[UART↓] MP3主体 TYPE=0x11: " << offset
              << " bytes，末片 " << g_held_mp3_tail.data.size()
              << " bytes 待确认" << std::endl;
}

void uart_finish_response() {
    const uint64_t generation = g_send_generation.load();
    std::lock_guard<std::mutex> lk(g_send_mutex);
    if (g_held_mp3_tail.data.empty() ||
        g_held_mp3_tail.generation != generation) {
        g_held_mp3_tail.data.clear();
        return;
    }
    if (send_frame(TYPE_MP3, 0, 1, g_held_mp3_tail.data.data(),
                   static_cast<uint16_t>(g_held_mp3_tail.data.size()))) {
        std::cerr << "[UART↓] 整轮最后MP3末片: "
                  << g_held_mp3_tail.data.size()
                  << " bytes, DSID=0, STATE=1" << std::endl;
    }
    g_held_mp3_tail.data.clear();
}
