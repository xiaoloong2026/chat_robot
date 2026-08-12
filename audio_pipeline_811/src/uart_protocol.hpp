#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uart_protocol {

// 呼吸机与端侧 AI 串口协议（2026-08-07）。
inline constexpr uint8_t SYNC0 = 0xAA;
inline constexpr uint8_t SYNC1 = 0x55;
inline constexpr size_t HEADER_SIZE = 9;       // AA55 + TYPE..LEN
inline constexpr size_t FRAME_OVERHEAD = 10;  // HEADER + CHECK
inline constexpr size_t MAX_FRAME_SIZE = 1024;
inline constexpr size_t MAX_CODE_SIZE = MAX_FRAME_SIZE - FRAME_OVERHEAD;

enum Type : uint8_t {
    TYPE_HANDSHAKE = 0xAA,
    TYPE_AUDIO_QUESTION = 0x01,
    TYPE_TEXT_QUESTION = 0x02,
    TYPE_DEVICE_INFO = 0x03,
    TYPE_SLEEP_DATA = 0x04,
    TYPE_AI_STATE = 0x05,

    // 协议第 5 章误写成 0x06；仅用于兼容接收，不作为正式发送值。
    TYPE_AI_STATE_LEGACY = 0x06,

    TYPE_TTS_AUDIO = 0x11,
    TYPE_CONTROL = 0x12,
    TYPE_TEXT = 0x13,
    TYPE_AI_INFO = 0x14,
};

enum QuestionState : uint8_t {
    QUESTION_STATE_NULL = 0,
    QUESTION_STATE_WAKE = 1,
    QUESTION_STATE_STREAMING = 2,
    QUESTION_STATE_END = 3,
};

enum TextState : uint8_t {
    TEXT_STATE_QUESTION = 0,
    TEXT_STATE_ANSWER = 1,
};

enum AudioState : uint8_t {
    AUDIO_STATE_NORMAL = 0,
    AUDIO_STATE_END = 1,
};

bool is_known_type(uint8_t type);
bool is_uplink_type(uint8_t type);
const char* type_name(uint8_t type);

uint8_t checksum(const uint8_t* data, size_t len);

/// 严格验证 UTF-8（拒绝截断、过长编码、代理项和 >U+10FFFF）。
bool is_valid_utf8(const uint8_t* data, size_t len);

/// 构造完整串口帧。DSID 必须为 1..65535，CODE 最大 1014 字节。
std::vector<uint8_t> encode_frame(uint8_t type,
                                  uint8_t seq,
                                  uint16_t dsid,
                                  uint8_t state,
                                  const uint8_t* code,
                                  size_t code_len);

struct HandshakeInfo {
    std::array<uint8_t, 4> plaintext{};
    std::array<uint8_t, 4> ciphertext{};
    std::string longitude;
    std::string latitude;
    std::string imei;
    uint8_t key_index = 0;
    uint8_t key = 0;
};

struct HandshakeResult {
    bool ok = false;
    HandshakeInfo info;
    std::string error;
};

uint8_t encrypt_handshake_byte(uint8_t plaintext, uint8_t key);
HandshakeResult parse_handshake(const std::vector<uint8_t>& code,
                                uint8_t state);

class TextQuestionAccumulator {
public:
    enum class Status { Accepted, Complete, Error };

    struct Result {
        Status status = Status::Accepted;
        std::string text;
        std::string error;
    };

    Result feed(const std::vector<uint8_t>& data,
                uint8_t state,
                uint16_t dsid);
    void reset();

private:
    static constexpr size_t MAX_TEXT_SIZE = 64 * 1024;

    std::string buffer_;
    uint16_t last_dsid_ = 0;
    bool active_ = false;
};

}  // namespace uart_protocol
