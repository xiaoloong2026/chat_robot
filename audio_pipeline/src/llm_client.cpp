#include "llm_client.h"

#include "config.h"
#include "json_helper.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <filesystem>

// ============================================================================
// 内部辅助
// ============================================================================

/// 构造 messages JSON 数组
static std::string build_messages_json(const std::vector<Message>& messages) {
    std::ostringstream oss;
    oss << R"([{"role":"system","content":")"
        << json_escape(std::string(LLM_SYSTEM_PROMPT))
        << R"("})";
    for (size_t i = 0; i < messages.size(); ++i) {
        oss << ",";
        oss << R"({"role":")" << messages[i].role
            << R"(","content":")" << json_escape(messages[i].content)
            << R"("})";
    }
    oss << "]";
    return oss.str();
}

/// 构造 LLM 请求 JSON 体（stream 模式）
static std::string build_request_body(const std::vector<Message>& messages,
                                      bool stream) {
    std::ostringstream body;
    body << "{"
         << R"("messages":)" << build_messages_json(messages) << ","
         << R"("max_tokens":)" << LLM_MAX_TOKENS << ","
         << R"("temperature":)" << LLM_TEMPERATURE << ","
         << R"("repeat_penalty":)" << LLM_REPEAT_PENALTY << ","
         << R"("stream":)" << (stream ? "true" : "false") << ","
         << R"("chat_template_kwargs":{"enable_thinking":false})"
         << "}";
    return body.str();
}

/// 从 SSE data 行中提取 delta.content
static std::string extract_delta_content(const std::string& json_str) {
    try {
        std::istringstream iss(json_str);
        boost::property_tree::ptree root;
        boost::property_tree::read_json(iss, root);

        const auto& choices = root.get_child("choices");
        if (choices.empty()) return "";

        const auto& first_choice = choices.front().second;
        auto delta_opt = first_choice.get_child_optional("delta");
        if (!delta_opt) return "";

        auto content_opt = delta_opt->get_child_optional("content");
        if (!content_opt) return "";

        std::string content = content_opt->data();
        // Boost 会把 JSON null 转成字符串 "null"，过滤掉
        if (content == "null") return "";
        return content;
    } catch (...) {
        return "";
    }
}

// ============================================================================
// 流式 LLM 调用（SSE）
// ============================================================================

void llm_chat_stream(const std::vector<Message>& messages,
                     DeltaCallback on_delta) {
    static std::atomic<uint64_t> request_sequence{0};
    std::string body = build_request_body(messages, /*stream=*/true);

    // 用临时文件传请求体，避免大文本在 shell 中拼接/转义问题
    std::error_code directory_error;
    std::filesystem::create_directories(
        std::string(PIPELINE_TEMP_DIR), directory_error);
    std::string tmpfile = std::string(PIPELINE_TEMP_DIR) +
        "/llm_request_" + std::to_string(getpid()) + "_" +
        std::to_string(request_sequence.fetch_add(1)) + ".json";
    struct RequestFileCleanup {
        const std::string& path;
        ~RequestFileCleanup() { std::remove(path.c_str()); }
    } request_file_cleanup{tmpfile};
    {
        std::unique_ptr<FILE, decltype(&fclose)> f(
            fopen(tmpfile.c_str(), "w"), fclose);
        if (!f) throw std::runtime_error("无法创建临时请求文件");
        fwrite(body.data(), 1, body.size(), f.get());
    }

    std::ostringstream cmd;
    cmd << "curl -s -N --max-time " << CURL_TIMEOUT_SEC << " "
        << "-X POST \"" << LLM_URL << "\" "
        << "-H \"Content-Type: application/json\" "
        << "-d @" << tmpfile;

    auto t0 = std::chrono::steady_clock::now();

    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen(cmd.str().c_str(), "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() 启动 curl 失败");
    }

    // 关键：关闭 popen 的默认全缓冲
    setvbuf(pipe.get(), nullptr, _IONBF, 0);

    std::string line;
    char buf[4096];

    while (fgets(buf, sizeof(buf), pipe.get())) {
        line = buf;
        // 去掉末尾换行
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();

        // SSE: data: {...}
        if (line.rfind("data: ", 0) == 0) {
            std::string data = line.substr(6);
            if (data == "[DONE]") break;

            std::string delta = extract_delta_content(data);
            if (!delta.empty()) {
                on_delta(delta);
            }
        }
    }

}

// ============================================================================
// 非流式调用
// ============================================================================

static std::string shell_exec(const std::string& cmd) {
    std::array<char, 4096> buffer{};
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen(cmd.c_str(), "r"), pclose);
    if (!pipe) throw std::runtime_error("popen() failed");
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

std::string llm_chat(const std::vector<Message>& messages) {
    std::string body = build_request_body(messages, /*stream=*/false);

    std::ostringstream cmd;
    cmd << "curl -s --max-time " << CURL_TIMEOUT_SEC << " "
        << "-X POST \"" << LLM_URL << "\" "
        << "-H \"Content-Type: application/json\" "
        << "-d '" << body << "'";

    std::string response = shell_exec(cmd.str());

    if (response.empty()) throw std::runtime_error("LLM 返回空响应");

    std::istringstream iss(response);
    boost::property_tree::ptree root;
    boost::property_tree::read_json(iss, root);

    const auto& choices = root.get_child("choices");
    if (choices.empty()) throw std::runtime_error("LLM 返回 choices 为空");
    return choices.front().second.get<std::string>("message.content", "");
}

std::string llm_chat(const std::string& user_input) {
    return llm_chat({{"user", user_input}});
}
