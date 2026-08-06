#pragma once

#include <functional>
#include <string>
#include <vector>

// ============================================================================
// LLM 客户端（支持流式和非流式）
// ============================================================================

struct Message {
    std::string role;
    std::string content;
};

/// 流式回调：每收到一个 delta token 调用一次
using DeltaCallback = std::function<void(const std::string& delta)>;

/// 流式 LLM 对话：SSE 接收，每个 token 调 callback
/// @throws std::runtime_error
void llm_chat_stream(const std::vector<Message>& messages,
                     DeltaCallback on_delta);

/// 非流式便捷接口
std::string llm_chat(const std::vector<Message>& messages);
std::string llm_chat(const std::string& user_input);
