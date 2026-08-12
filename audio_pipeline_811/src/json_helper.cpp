#include "json_helper.h"

#include <cstdint>
#include <cstring>

std::string json_escape(std::string_view s) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += HEX[(c >> 4) & 0x0F];
                out += HEX[c & 0x0F];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// ———————————————————————————————————————————
// UTF-8 解码辅助
// ———————————————————————————————————————————

namespace {

/// 读取一个 UTF-8 字符，返回 Unicode 码点和字节数
struct Utf8Char {
    char32_t cp = 0;
    int      len = 0;  // 0 = 无效/EOF
};

Utf8Char next_utf8(const char* s, size_t pos, size_t len) {
    if (pos >= len) return {};
    auto b0 = static_cast<uint8_t>(s[pos]);

    Utf8Char uc;
    if (b0 < 0x80) {
        uc.cp  = b0;
        uc.len = 1;
    } else if ((b0 & 0xE0) == 0xC0 && pos + 1 < len) {
        uc.cp  = (static_cast<char32_t>(b0 & 0x1F) << 6)
               |  (static_cast<uint8_t>(s[pos + 1]) & 0x3F);
        uc.len = 2;
    } else if ((b0 & 0xF0) == 0xE0 && pos + 2 < len) {
        uc.cp  = (static_cast<char32_t>(b0 & 0x0F) << 12)
               | (static_cast<char32_t>(static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 6)
               |  (static_cast<uint8_t>(s[pos + 2]) & 0x3F);
        uc.len = 3;
    } else if ((b0 & 0xF8) == 0xF0 && pos + 3 < len) {
        uc.cp  = (static_cast<char32_t>(b0 & 0x07) << 18)
               | (static_cast<char32_t>(static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 12)
               | (static_cast<char32_t>(static_cast<uint8_t>(s[pos + 2]) & 0x3F) << 6)
               |  (static_cast<uint8_t>(s[pos + 3]) & 0x3F);
        uc.len = 4;
    }
    return uc;
}

/// 判断码点是否为 emoji / 不可朗读字符
bool is_unpronounceable(char32_t cp) {
    // Emoji & pictographs
    if (cp >= 0x1F300 && cp <= 0x1FAFF) return true;
    // Misc symbols
    if (cp >= 0x2600  && cp <= 0x27BF)  return true;
    // Dingbats
    if (cp >= 0x2700  && cp <= 0x27BF)  return true;
    // Emoticons
    if (cp >= 0x1F600 && cp <= 0x1F64F) return true;
    // Supplemental symbols
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return true;
    // Variation selectors & modifiers
    if (cp >= 0xFE00  && cp <= 0xFE0F)  return true;
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return true;  // skin tones

    return false;
}

}  // namespace

std::string strip_emoji(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        auto uc = next_utf8(s.data(), i, s.size());
        if (uc.len <= 0) { ++i; continue; }
        if (!is_unpronounceable(uc.cp)) {
            out.append(s.data() + i, uc.len);
        }
        i += uc.len;
    }
    return out;
}

// ============================================================================
// Markdown 清洗
// ============================================================================

std::string strip_markdown(std::string_view s) {
    std::string result{s};

    // 粗体 **text**
    for (size_t pos = 0; (pos = result.find("**", pos)) != std::string::npos; ) {
        auto end = result.find("**", pos + 2);
        if (end != std::string::npos) {
            result.erase(end, 2);
            result.erase(pos, 2);
        } else {
            result.erase(pos, 2);
        }
    }

    // 行首 ### / ## / #
    for (const char* prefix : {"### ", "## ", "# "}) {
        size_t plen = std::strlen(prefix);
        for (size_t pos = 0; (pos = result.find(prefix, pos)) != std::string::npos; ) {
            if (pos == 0 || result[pos - 1] == '\n') {
                result.erase(pos, plen);
            } else {
                pos += plen;
            }
        }
    }

    // 行首 "1. " "2. " 数字列表
    {
        std::string tmp;
        tmp.reserve(result.size());
        for (size_t i = 0; i < result.size(); ) {
            if ((i == 0 || result[i - 1] == '\n') &&
                result[i] >= '0' && result[i] <= '9') {
                size_t j = i;
                while (j < result.size() && result[j] >= '0' && result[j] <= '9') ++j;
                if (j < result.size() && result[j] == '.' && j + 1 < result.size() && result[j + 1] == ' ') {
                    i = j + 2;
                    continue;
                }
            }
            tmp += result[i++];
        }
        result = std::move(tmp);
    }

    // 行首 "- " 列表标记
    {
        std::string tmp;
        tmp.reserve(result.size());
        for (size_t i = 0; i < result.size(); ) {
            if ((i == 0 || result[i - 1] == '\n') &&
                i + 1 < result.size() &&
                result[i] == '-' && result[i + 1] == ' ') {
                i += 2;
                continue;
            }
            tmp += result[i++];
        }
        result = std::move(tmp);
    }

    // 行内代码 `code`
    for (size_t pos = 0; (pos = result.find('`', pos)) != std::string::npos; ) {
        auto end = result.find('`', pos + 1);
        if (end != std::string::npos) {
            result.erase(end, 1);
            result.erase(pos, 1);
        } else {
            result.erase(pos, 1);
        }
    }

    // --- 分隔线 → 逗号
    for (size_t pos = 0; (pos = result.find("---", pos)) != std::string::npos; ) {
        result.replace(pos, 3, "，");
        pos += 3;
    }

    return result;
}
