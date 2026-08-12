#include "uart_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace uart_protocol {
namespace {

void write_le16(std::vector<uint8_t>& buffer, uint16_t value) {
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

bool read_nul_terminated_text(const uint8_t* data,
                              size_t size,
                              const char* field,
                              std::string& output,
                              std::string& error) {
    const auto* end = std::find(data, data + size, uint8_t{0});
    if (end == data + size) {
        error = std::string(field) + " 缺少 NUL 结束符";
        return false;
    }
    output.assign(reinterpret_cast<const char*>(data),
                  static_cast<size_t>(end - data));
    return true;
}

bool read_imei(const uint8_t* data,
               size_t size,
               std::string& output,
               std::string& error) {
    (void)error;
    const auto* end = std::find(data, data + size, uint8_t{0});
    output.assign(reinterpret_cast<const char*>(data),
                  static_cast<size_t>(end - data));
    return true;
}

}  // namespace

bool is_known_type(uint8_t type) {
    switch (type) {
    case TYPE_HANDSHAKE:
    case TYPE_AUDIO_QUESTION:
    case TYPE_TEXT_QUESTION:
    case TYPE_DEVICE_INFO:
    case TYPE_SLEEP_DATA:
    case TYPE_AI_STATE:
    case TYPE_AI_STATE_LEGACY:
    case TYPE_TTS_AUDIO:
    case TYPE_CONTROL:
    case TYPE_TEXT:
    case TYPE_AI_INFO:
        return true;
    default:
        return false;
    }
}

bool is_uplink_type(uint8_t type) {
    return type == TYPE_HANDSHAKE ||
           (type >= TYPE_AUDIO_QUESTION && type <= TYPE_AI_STATE_LEGACY);
}

const char* type_name(uint8_t type) {
    switch (type) {
    case TYPE_HANDSHAKE:       return "握手";
    case TYPE_AUDIO_QUESTION:  return "音频提问";
    case TYPE_TEXT_QUESTION:   return "文本提问";
    case TYPE_DEVICE_INFO:     return "设备信息";
    case TYPE_SLEEP_DATA:      return "睡眠数据";
    case TYPE_AI_STATE:        return "AI状态";
    case TYPE_AI_STATE_LEGACY: return "AI状态(兼容0x06)";
    case TYPE_TTS_AUDIO:       return "TTS音频";
    case TYPE_CONTROL:         return "控制指令";
    case TYPE_TEXT:            return "下行文本";
    case TYPE_AI_INFO:         return "AI信息";
    default:                   return "未知";
    }
}

uint8_t checksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += data[i];
    return static_cast<uint8_t>(sum & 0xFF);
}

bool is_valid_utf8(const uint8_t* data, size_t len) {
    if (len > 0 && data == nullptr) return false;
    size_t i = 0;
    while (i < len) {
        const uint8_t first = data[i++];
        if (first <= 0x7F) continue;

        size_t trailing = 0;
        uint8_t second_min = 0x80;
        uint8_t second_max = 0xBF;
        if (first >= 0xC2 && first <= 0xDF) {
            trailing = 1;
        } else if (first >= 0xE0 && first <= 0xEF) {
            trailing = 2;
            if (first == 0xE0) second_min = 0xA0;
            if (first == 0xED) second_max = 0x9F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            trailing = 3;
            if (first == 0xF0) second_min = 0x90;
            if (first == 0xF4) second_max = 0x8F;
        } else {
            return false;
        }
        if (i + trailing > len) return false;
        if (data[i] < second_min || data[i] > second_max) return false;
        ++i;
        for (size_t n = 1; n < trailing; ++n, ++i) {
            if (data[i] < 0x80 || data[i] > 0xBF) return false;
        }
    }
    return true;
}

std::vector<uint8_t> encode_frame(uint8_t type,
                                  uint8_t seq,
                                  uint16_t dsid,
                                  uint8_t state,
                                  const uint8_t* code,
                                  size_t code_len) {
    if (!is_known_type(type))
        throw std::invalid_argument("未知 UART TYPE");
    if (dsid == 0)
        throw std::invalid_argument("DSID 必须为 1..65535");
    if (code_len > MAX_CODE_SIZE)
        throw std::length_error("UART CODE 超过 1014 字节");
    if (code_len > 0 && code == nullptr)
        throw std::invalid_argument("非空 CODE 的数据指针为空");

    std::vector<uint8_t> frame;
    frame.reserve(FRAME_OVERHEAD + code_len);
    frame.push_back(SYNC0);
    frame.push_back(SYNC1);
    frame.push_back(type);
    frame.push_back(seq);
    write_le16(frame, dsid);
    frame.push_back(state);
    write_le16(frame, static_cast<uint16_t>(code_len));
    if (code_len > 0) frame.insert(frame.end(), code, code + code_len);
    frame.push_back(checksum(frame.data() + 2, frame.size() - 2));
    return frame;
}

uint8_t encrypt_handshake_byte(uint8_t plaintext, uint8_t key) {
    const uint8_t mixed = static_cast<uint8_t>(plaintext + key);
    return static_cast<uint8_t>(mixed + ((mixed & 0x01) << 1));
}

HandshakeResult parse_handshake(const std::vector<uint8_t>& code,
                                uint8_t state) {
    HandshakeResult result;
    if (state > 1) {
        result.error = "握手 STATE 只能是 0(KEY199) 或 1(KEY218)";
        return result;
    }
    if (code.size() != 48) {
        result.error = "握手 CODE 长度必须为 48 字节";
        return result;
    }

    result.info.key_index = state;
    result.info.key = state == 0 ? 199 : 218;
    std::copy_n(code.begin(), 4, result.info.plaintext.begin());
    std::copy_n(code.begin() + 4, 4, result.info.ciphertext.begin());

    for (size_t i = 0; i < result.info.plaintext.size(); ++i) {
        const uint8_t expected = encrypt_handshake_byte(
            result.info.plaintext[i], result.info.key);
        if (expected != result.info.ciphertext[i]) {
            result.error = "握手明文与密文不匹配";
            return result;
        }
    }

    if (!read_nul_terminated_text(code.data() + 8, 12, "经度",
                                  result.info.longitude, result.error) ||
        !read_nul_terminated_text(code.data() + 20, 12, "纬度",
                                  result.info.latitude, result.error) ||
        !read_imei(code.data() + 32, 16, result.info.imei, result.error)) {
        return result;
    }
    for (const auto* field : {&result.info.longitude,
                              &result.info.latitude,
                              &result.info.imei}) {
        if (!is_valid_utf8(
                reinterpret_cast<const uint8_t*>(field->data()),
                field->size())) {
            result.error = "握手文本字段不是有效 UTF-8";
            return result;
        }
    }

    result.ok = true;
    return result;
}

TextQuestionAccumulator::Result TextQuestionAccumulator::feed(
    const std::vector<uint8_t>& data,
    uint8_t state,
    uint16_t dsid) {
    Result result;
    if (state > QUESTION_STATE_END) {
        result.status = Status::Error;
        result.error = "文本提问 STATE 超出 0..3";
        reset();
        return result;
    }
    if (state == QUESTION_STATE_NULL) {
        if (data.empty()) return result;  // 安全忽略空 NULL/保活帧。
        result.status = Status::Error;
        result.error = "文本提问 STATE=0(NULL) 只允许空 CODE";
        reset();
        return result;
    }
    if (dsid == 0) {
        result.status = Status::Error;
        result.error = "文本提问 DSID 不能为 0";
        reset();
        return result;
    }

    if (state == QUESTION_STATE_WAKE) reset();
    if (!active_) {
        if (state != QUESTION_STATE_WAKE || dsid != 1) {
            result.status = Status::Error;
            result.error = "文本提问必须由 STATE=1、DSID=1 开始";
            return result;
        }
        active_ = true;
    } else {
        if (last_dsid_ == 0xFFFF) {
            result.status = Status::Error;
            result.error = "文本提问 DSID 已到 65535，协议未定义回绕";
            reset();
            return result;
        }
        const uint16_t expected = static_cast<uint16_t>(last_dsid_ + 1);
        if (dsid != expected) {
            result.status = Status::Error;
            result.error = "文本提问 DSID 不连续，期望 " +
                           std::to_string(expected) + "，实际 " +
                           std::to_string(dsid);
            reset();
            return result;
        }
    }

    if (buffer_.size() + data.size() > MAX_TEXT_SIZE) {
        result.status = Status::Error;
        result.error = "文本提问累计长度超过 64 KiB";
        reset();
        return result;
    }
    buffer_.append(reinterpret_cast<const char*>(data.data()), data.size());
    last_dsid_ = dsid;

    if (state == QUESTION_STATE_END) {
        if (!is_valid_utf8(
                reinterpret_cast<const uint8_t*>(buffer_.data()),
                buffer_.size())) {
            result.status = Status::Error;
            result.error = "文本提问 CODE 不是有效 UTF-8";
            reset();
            return result;
        }
        result.status = Status::Complete;
        result.text = std::move(buffer_);
        reset();
    }
    return result;
}

void TextQuestionAccumulator::reset() {
    buffer_.clear();
    last_dsid_ = 0;
    active_ = false;
}

}  // namespace uart_protocol
