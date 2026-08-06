#pragma once

#include <string>
#include <string_view>

// ============================================================================
// JSON 字符串转义
// ============================================================================

/**
 * 对字符串做 JSON 字符串转义（处理 " \ \n \r \t 等）。
 */
std::string json_escape(std::string_view s);

/**
 * 移除字符串中的 emoji 和不可朗读字符。
 */
std::string strip_emoji(std::string_view s);

/**
 * 清洗 Markdown 格式字符（** ### - 等），避免 TTS 朗读标记语法。
 * 保留纯文本内容。
 */
std::string strip_markdown(std::string_view s);
