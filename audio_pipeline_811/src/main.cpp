/**
 * audio_pipeline — ASR → LLM → TTS 音频对话调度（交互式多轮对话）
 *
 * 构建：
 *   mkdir build && cd build
 *   cmake .. -DCMAKE_BUILD_TYPE=Release
 *   make -j$(nproc)
 *
 * 运行：
 *   ./audio_pipeline            # 交互式多轮
 *   ./audio_pipeline "你好"     # 单发
 */

#include "asr_client.h"
#include "config.h"
#include "json_helper.h"
#include "llm_client.h"
#include "opus_accumulator.h"
#include "parser.hpp"
#include "ring_buffer.hpp"
#include "tts_client.h"
#include "uart.hpp"
#include "uart_protocol.hpp"
#include "uart_sender.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

//自定义异常类，用于表示任务被取消或中断的情况
//继承标准异常类 std::exception,把TaskCancelled 类当作标准异常处理，重写了 what() 方法，返回一个描述异常的字符串
class TaskCancelled : public std::exception {
public:
    const char* what() const noexcept override { return "任务已被新指令中断"; }
};

//临时文件管理类：保存临时音频文件路径，并在对象销毁时删除该文件（除非 DEBUG_SAVE_AUDIO 为 true）。
class TemporaryAudioFile {
public:
    //explicit 关键字用于防止构造函数被隐式调用，确保只能通过显式传递路径来创建对象。
    //move函数转移字符串内部指针、长度和容量，不需要重新复制全部字符。
    explicit TemporaryAudioFile(std::string path) : path_(std::move(path)) {}
    ~TemporaryAudioFile() {
        if (!DEBUG_SAVE_AUDIO && !path_.empty())
            std::remove(path_.c_str());
    }
private:
    std::string path_;
};

// ============================================================================
// 线程安全句子队列
// ============================================================================

class SentenceQueue {
public:
    // 线程安全地将句子推入队列，并通知等待的消费者线程。
    void push(std::string sentence) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(std::move(sentence));
        }
        cv_.notify_one();// 唤醒一个等待的消费者线程
    }

    //取出句子
    std::string pop() {
        std::unique_lock<std::mutex> lk(mtx_);
        //队列不为空或队列已经关闭
        cv_.wait(lk, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return "";
        auto s = std::move(queue_.front());
        queue_.pop();
        return s;
    }

    void close() {
        {   //修改close_时必须持锁，因为消费者线程也会读取它，不加会产生数据竞争
            std::lock_guard<std::mutex> lk(mtx_);
            closed_ = true;
        }
        cv_.notify_all();//唤醒所有等待者线程，检查clased_状态，等待退出。
    }

private:
    mutable std::mutex mtx_; // 互斥锁，用于保护队列的访问，确保线程安全
    std::condition_variable cv_;  //条件变量，用来让消费者线程在没有数据时休眠，而不是反复轮询
    std::queue<std::string> queue_; //队列
    bool closed_ = false; //判断队列是否关闭
};

// ============================================================================
// 句子切分器
// ============================================================================

class SentenceSplitter {
public:
    // delta表示本次接收的一小段流式文本
    std::vector<std::string> feed(const std::string& delta) {
        //创建返回的结果，用于保存本次切分出的完整句子
        std::vector<std::string> sentences;

        buf_ += delta;
        //查找最早出现的分隔符位置，按顺序查找句号、感叹号、问号、中文逗号和双换行符
        while (true) {
            // best_pos表示最早出现的分隔符位置，best_len表示该分隔符的长度(占多少字节)
            //std::string::npos表示未找到分隔符，初始化为最大值
            size_t best_pos = std::string::npos;
            int    best_len = 0;
            //查找最靠前的分隔符
            for (const auto& d : delimiters_) {

                auto pos = buf_.find(d);
                if (pos != std::string::npos && pos < best_pos) {
                    best_pos = pos;
                    best_len = static_cast<int>(d.size());
                }
            }

            // 如果前 3 句还没切完，允许按中文逗号切分
            if (comma_left_ > 0) {
                auto pos = buf_.find(u8"，");
                if (pos != std::string::npos && pos < best_pos) {
                    best_pos = pos; best_len = std::string(u8"，").size();
                }
            }

            //查找连续两个换行符，作为段落分隔符
            auto nlpos = buf_.find("\n\n");
            if (nlpos != std::string::npos && nlpos < best_pos) {
                best_pos = nlpos; best_len = 2;
            }
            if (best_pos == std::string::npos) break; // 没有找到分隔符，退出循环

            //提取完整句子，将分隔符保留在句子中
            //trim函数用于去除首位空白
            std::string sentence = trim(buf_.substr(0, best_pos + best_len));
            //提取之后就擦掉
            buf_.erase(0, best_pos + best_len);
            // 过滤，、。、！、？，sentence.size()返回的是UTF-8字节数，不是字符数量
            if (sentence.size() >= min_sentence_len) {
                if (comma_left_ > 0) --comma_left_;
                sentences.push_back(std::move(sentence));
            }
        }
        return sentences;
    }
    //流式结束，最后一段文本可能没有标点，清空缓冲区
    std::string flush() {
        auto s = trim(buf_);
        buf_.clear();
        return s;
    }

private:
    //分隔符
    const std::vector<std::string> delimiters_ = {
        u8"。", u8"！", u8"？"
    };

    static constexpr size_t min_sentence_len = 4;

    // 前 3 句额外按逗号切分
    static constexpr int kCommaSplitCount = 3;
    int comma_left_ = kCommaSplitCount;

    //去掉字符串两端的空格、\t\n\r
    static std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\n\r");
        return s.substr(start, end - start + 1);
    }

    std::string buf_;   //用于保存没有形成完整句子的文本
};

// ============================================================================
// 界面
// ============================================================================
//匿名命名空间

namespace {

//打印程序启动界面
void print_banner() {

    //标准错误输出流， 用于运行状态显示
    std::cerr << "\n";
    std::cerr << "╔════════════════════════════════╗\n";
    std::cerr << "║  ASR → LLM → TTS  对话调度    ║\n";
    std::cerr << "║  :v 语音输入 | 文字 文本输入  ║\n";
    std::cerr << "║  exit 退出                    ║\n";
    std::cerr << "╚════════════════════════════════╝\n\n";
}
// 判断用户输入是否为退出命令
bool is_exit_command(const std::string& s) {
    //提前预留与输入相同的容量空间
    std::string lower;
    lower.reserve(s.size());
    //逐字符转为小写
    for (char c : s)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    //去除首尾空白
    auto start = lower.find_first_not_of(" \t\n\r");
    auto end   = lower.find_last_not_of(" \t\n\r");

    if (start == std::string::npos) return false;

    auto t = lower.substr(start, end - start + 1);
    return t == "exit" || t == "quit" || t == "bye";
}

/// 录音并返回识别文本
std::string record_and_transcribe() {
    std::string tmp_wav = std::string(PIPELINE_TEMP_DIR) +
        "/asr_input_" + std::to_string(getpid()) + ".wav";
    TemporaryAudioFile tmp_cleanup(tmp_wav);
    //打印录音状态
    std::cerr << "🎤 录音中 (5秒)... " << std::flush;
    //构造arecord命令
    std::ostringstream cmd;
    //创建字符串流输出，用于拼接shell命令
    cmd << "arecord -q -D " << MIC_DEVICE
        << " -f S16_LE -r " << MIC_SAMPLE_RATE
        << " -c 1 -d 5 " << tmp_wav
        << " 2>/dev/null";
    //执行录音命令
    //cmd.str() -- 得到完整字符串
    //.c_str() -- 转换成const char*
    const int record_status = system(cmd.str().c_str());
    if (record_status == -1 || !WIFEXITED(record_status) ||
        WEXITSTATUS(record_status) != 0) {
        throw std::runtime_error("arecord 录音失败");
    }

    std::cerr << "识别中..." << std::flush;
    //调用ASR服务
    std::string text = asr_transcribe(tmp_wav);
    return text;
}

}  // namespace


// ============================================================================
// 统一流式 LLM → 标点切句 → TTS
// ============================================================================

/// 流式接收 LLM token，按 SentenceSplitter 的规则逐句提交给 TTS。
/// on_delta 用于需要实时显示 token 的模式；返回完整回复供历史记录/UART 下发。

// ============================================================================
//  函数：stream_llm_to_tts
//  功能：LLM->TTS
//  参数：messages：传给 LLM 的对话历史
//       on_delta: 用于接收 LLM 每次返回的增量文本
//       is_cancelled: 取消状态检查函数
//       tts_generation: 传递给 TTS 的任务代数或任务编号
// ============================================================================


static std::string stream_llm_to_tts(
    const std::vector<Message>& messages,
    const DeltaCallback& on_delta = {},
    const std::function<bool()>& is_cancelled = {},
    uint64_t tts_generation = UINT64_MAX,
    uint64_t delivery_generation = UINT64_MAX) {
    //创建分句器和完整回复
    SentenceSplitter splitter;
    std::string full_reply;
    
    //llm流式接口：接收消息列表，并在每次拿到增量文本时调用 Lambda
    // & 按引用捕获外部变量
    llm_chat_stream(messages, [&](const std::string& delta) {
        //收到每个delta后先检查是否取消
        if (is_cancelled && is_cancelled()) throw TaskCancelled();
        //累积完整回复
        full_reply += delta;
        //实时通知文字输出端
        if (on_delta) on_delta(delta);
        // 得到完整句子
        auto sentences = splitter.feed(delta);

        //调试打印
        // std::cerr
        //     << "\n[TTS DEBUG] delta="
        //     << delta
        //     << " sentences="
        //     << sentences.size()
        //     << std::endl;
        // 处理每一个完整的句子
        for (auto& sentence : sentences) {

            if (is_cancelled && is_cancelled()) throw TaskCancelled();
            // 清洗TTS文本
            std::string clean = strip_emoji(strip_markdown(sentence));
            //调试打印
                // std::cerr
                // << "[TTS DEBUG] sentence=["
                // << sentence
                // << "] clean=["
                // << clean
                // << "]"
                // << std::endl;
            // 提交给TTS
            if (!clean.empty()) {
                //调试打印
                // std::cerr
                // << "[TTS DEBUG] submit:"
                // << " generation="
                // << tts_generation
                // << " delivery_generation="
                // << delivery_generation
                // << std::endl;


                tts_speak_async(
                    clean, tts_generation, delivery_generation);
                //调试打印
                // std::cerr
                // << "[TTS DEBUG] submit returned"
                // << std::endl;
            }
        }
    });

    //调试打印
        // std::cerr
        // << "\n[LLM DEBUG] llm_chat_stream returned"
        // << std::endl;


    // LLM 结束时，末尾没有句号的内容也必须送入 TTS。
    std::string remaining = splitter.flush();

    //调试打印
    // std::cerr
    // << "[TTS DEBUG] flush remaining=["
    // << remaining
    // << "]"
    // << std::endl;

    if (is_cancelled && is_cancelled()) throw TaskCancelled();
    std::string clean = strip_emoji(strip_markdown(remaining));
    if (!clean.empty()) {
        tts_speak_async(clean, tts_generation, delivery_generation);
    }
    // 返回完整的回复
    return full_reply;
}

// ============================================================================
// 设备控制意图 → 协议 TYPE 0x12 JSON
// ============================================================================

// text 中是否包含给定关键字列表里的任意一个关键字
static bool contains_any(
    const std::string& text,
    std::initializer_list<std::string_view> keywords) {
    for (const auto keyword : keywords) {
        if (text.find(keyword) != std::string::npos) return true;
    }
    return false;
}

// 仅映射明确、低歧义的设备控制指令；普通问答不产生 0x12。
// ============================================================================
//  函数：build_control_json
//  功能：根据用户的自然语言指令，识别呼吸机、湿化器或管路温度控制命令，并生成对应的 JSON 字符串；无法识别时返回空值。
//  参数：user_text：用户的自然语言指令
// ============================================================================
static std::optional<std::string> build_control_json(
    const std::string& user_text) {
    if (user_text.find(u8"呼吸机") != std::string::npos) {
        if (contains_any(user_text, {u8"打开", u8"启动", u8"开机"})) {

            //返回JSON字符串
            return R"({"target":"呼吸机开关","action":"set","value":"启动"})";
        }
        if (contains_any(user_text, {u8"关闭", u8"停止", u8"关机"})) {
            return R"({"target":"呼吸机开关","action":"set","value":"关闭"})";
        }
    }

    if (contains_any(user_text, {u8"湿化器", u8"加湿器"})) {
        if (contains_any(user_text, {u8"自动", u8"自动档"})) {
            return R"({"target":"湿化器","action":"set","value":"自动"})";
        }
        if (contains_any(user_text, {u8"打开", u8"启动", u8"开启"})) {
            return R"({"target":"湿化器","action":"set","value":"启动"})";
        }
        if (contains_any(user_text, {u8"关闭", u8"停止"})) {
            return R"({"target":"湿化器","action":"set","value":"关闭"})";
        }
    }

    if (user_text.find(u8"管路温度") != std::string::npos) {
        //使用正则表达式查找数字
        std::smatch match;
        //捕获连续出现的数字
        if (std::regex_search(user_text, match, std::regex(R"((\d{1,2}))"))) {
            // 将字符串转换成整数
            const int temperature = std::stoi(match[1].str());
            if (temperature >= 5 && temperature <= 40) {
                //构造温度JSON
                return std::string(
                           R"({"target":"管路温度","action":"set","value":)") +
                       std::to_string(temperature) + "}";
            }
        }
    }
    return std::nullopt;
}

// 把“自然语言控制指令”转换成 JSON，并通过 UART 发送出去
static bool uart_send_control_if_present(const std::string& user_text) {
    auto control = build_control_json(user_text);
    //没有控制指令直接退出
    if (!control) return true;
    std::cerr << "[UART↓] 控制JSON: " << *control << std::endl;
    return uart_send_control(*control);
}

static std::string make_ai_info_json(const std::string& run,
                                     const std::string& error = "") {
    return std::string("{\"version\":\"") +
           json_escape(std::string(AI_VERSION)) +
           "\",\"run\":\"" + json_escape(run) +
           "\",\"error\":\"" + json_escape(error) + "\"}";
}

static bool validate_json_object(const std::vector<uint8_t>& data,
                                 std::string& json,
                                 std::string& error) {
    if (data.empty()) {
        error = "JSON CODE 不能为空";
        return false;
    }
    if (!uart_protocol::is_valid_utf8(data.data(), data.size())) {
        error = "JSON CODE 不是有效 UTF-8";
        return false;
    }
    json.assign(reinterpret_cast<const char*>(data.data()), data.size());
    const auto first = json.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || json[first] != '{') {
        error = "CODE 不是 JSON 对象";
        return false;
    }
    try {
        std::istringstream input(json);
        boost::property_tree::ptree tree;
        boost::property_tree::read_json(input, tree);
        return true;
    } catch (const std::exception& e) {
        error = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
}

enum class AiRuntimeState { Running, Sleeping, EnergySaving };

static const char* ai_runtime_state_text(AiRuntimeState state) {
    switch (state) {
    case AiRuntimeState::Running:      return "正常";
    case AiRuntimeState::Sleeping:     return "休眠";
    case AiRuntimeState::EnergySaving: return "节能";
    }
    return "异常";
}

// ============================================================================
// 交互式多轮对话
// ============================================================================

static int run_interactive() {
    print_banner();
    tts_init();  //初始化TTS模块

    // 创建对话历史：用于保存用户与助手的全部上下文
    std::vector<Message> history;
    //对哈轮数
    int turn = 0;

    while (true) {
        ++turn;
        std::cerr << "[" << turn << "] 你: " << std::flush;
        //读取用户输入
        std::string user_input;
        std::getline(std::cin, user_input);
        //空输入时直接跳过本次
        if (user_input.empty()) { --turn; continue; }
        // 语音输入，直接返回文本作为输入
        if (user_input == ":v") {
            user_input = record_and_transcribe();
            if (user_input.empty()) { --turn; continue; }
            std::cerr << "识别: " << user_input << std::endl;
        }
        // 判断是否有退出意思
        if (is_exit_command(user_input)) {
            std::cerr << "再见！共对话 " << (turn - 1) << " 轮。" << std::endl;
            break;
        }

        // 记录请求开始时间（首音延迟计时起点）
        auto request_start = std::chrono::steady_clock::now();
        //把时间传给TTS模块
        tts_set_start_time(request_start);

        history.push_back({"user", user_input});
        // 创建完整回复
        std::string full_reply;

        try {
            full_reply = stream_llm_to_tts(history);
        } catch (const std::exception& e) {
            std::cerr << "\n[LLM 错误] " << e.what() << std::endl;
        }
        //等待所有TTS完成
        tts_wait_all();
        //打印完整回复
        std::cerr << full_reply << std::endl << std::endl;

        if (!full_reply.empty())
            history.push_back({"assistant", full_reply});
        else
        //删除刚加入的用户消息
            history.pop_back();
    }
    // 退出循环后清除TTS
    tts_cleanup();
    return EXIT_SUCCESS;
}

// ============================================================================
// 单发模式
// ============================================================================

static int run_once(const std::string& user_input) {
    auto t0 = std::chrono::steady_clock::now();
    tts_init();
    tts_set_start_time(t0);

    try {
        std::string reply = stream_llm_to_tts(
            {{"user", user_input}},
            [](const std::string& delta) {
                std::cout << delta << std::flush;
            });
        std::cout << std::endl;

        if (reply.empty()) {
            std::cerr << "[错误] LLM 返回空" << std::endl;
            tts_cleanup();
            return EXIT_FAILURE;
        }

        tts_cleanup();

        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        tts_cleanup();
        std::cerr << "[错误] " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}

// ============================================================================
// 文件处理模式：音频文件 → ASR → LLM → TTS → MP3
// ============================================================================

static int run_file_mode(const std::string& input_file, const std::string& output_prefix) {
    auto t_total  = std::chrono::steady_clock::now();
    auto t_stage  = t_total;
    long cvt_ms = 0, asr_ms = 0;

    std::cerr << "音频文件 → ASR → LLM → TTS → MP3 (逐句)" << std::endl;

    // 1. 转成 WAV
    std::string wav_path = input_file;
    if (input_file.find(".wav") == std::string::npos) {
        wav_path = std::string(PIPELINE_TEMP_DIR) +
                   "/asr_input_" + std::to_string(getpid()) + ".wav";
        std::ostringstream cmd;
        cmd << "ffmpeg -y -i \"" << input_file
            << "\" -ar 16000 -ac 1 -f wav " << wav_path << " 2>/dev/null";
        if (system(cmd.str().c_str()) != 0) {
            std::cerr << "❌ 音频转换失败" << std::endl;
            return EXIT_FAILURE;
        }
        cvt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t_stage).count();
    }
    t_stage = std::chrono::steady_clock::now();

    // 2. ASR
    std::string text = asr_transcribe(wav_path);
    if (wav_path != input_file) std::remove(wav_path.c_str());
    asr_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - t_stage).count();

    if (text.empty()) {
        std::cerr << "❌ ASR 识别为空" << std::endl;
        return EXIT_FAILURE;
    }
    std::cerr << "  识别: " << text << std::endl;

    // 3. LLM 流式 + 逐句 TTS → MP3
    std::cerr << "  回复: " << std::flush;
    auto t_llm_start = std::chrono::steady_clock::now();

    SentenceSplitter splitter;
    std::string full_reply;
    int file_index = 0;
    long first_mp3_ms = 0;

    std::vector<long> seg_times;
    pid_t last_play_pid = -1;

    llm_chat_stream({{"user", text}}, [&](const std::string& delta) {
        std::cout << delta << std::flush;
        full_reply += delta;
        auto sentences = splitter.feed(delta);
        for (auto& s : sentences) {
            std::string clean = strip_emoji(strip_markdown(s));
            if (clean.empty()) continue;

            ++file_index;
            std::string fname = (output_prefix.empty() ? "output" : output_prefix)
                              + "_" + std::to_string(file_index) + ".mp3";
            tts_to_file(clean, fname);
            // 可选：通过开发板本机声卡播放；默认仅保存 MP3。
            if (TTS_PLAY_AUDIO) {
                if (last_play_pid > 0) {
                    int status;
                    waitpid(last_play_pid, &status, 0);
                }
                std::ostringstream pcmd;
                pcmd << "ffmpeg -y -i \"" << fname << "\" -f wav - 2>/dev/null"
                     << " | aplay -q -D " << AUDIO_DEVICE;
                auto cmd_str = pcmd.str();
                last_play_pid = fork();
                if (last_play_pid == 0) {
                    execlp("sh", "sh", "-c", cmd_str.c_str(), nullptr);
                    _exit(1);
                }
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t_total).count();
            seg_times.push_back(ms);
            if (!first_mp3_ms) first_mp3_ms = ms;
        }
    });
    std::cout << std::endl;

    // 剩余
    auto remaining = splitter.flush();
    std::string clean_remaining = strip_emoji(strip_markdown(remaining));
    if (!clean_remaining.empty()) {
        ++file_index;
        std::string fname = (output_prefix.empty() ? "output" : output_prefix)
                          + "_" + std::to_string(file_index) + ".mp3";
        tts_to_file(clean_remaining, fname);
    }

    auto llm_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t_llm_start).count();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t_total).count();

    std::cerr << "\n═══════════════════════════════════" << std::endl;
    std::cerr << "  耗时统计" << std::endl;
    std::cerr << "───────────────────────────────────" << std::endl;
    if (cvt_ms) std::cerr << "  音频转换:    " << cvt_ms << " ms" << std::endl;
    std::cerr << "  ASR 识别:    " << asr_ms << " ms" << std::endl;
    std::cerr << "  LLM+TTS 流式:" << llm_total_ms << " ms" << std::endl;
    if (first_mp3_ms) std::cerr << "  ★ 首句 MP3:  " << first_mp3_ms << " ms (输入→第一个MP3完成)" << std::endl;
    for (size_t i = 0; i < seg_times.size(); ++i) {
        std::cerr << "     段" << (i + 1) << ":      " << seg_times[i] << " ms"
                  << "  (" << (output_prefix.empty() ? "output" : output_prefix)
                  << "_" << (i + 1) << ".mp3)" << std::endl;
    }
    std::cerr << "  ─────────────────────────────" << std::endl;
    std::cerr << "  总耗时:      " << total_ms << " ms" << std::endl;
    std::cerr << "  输出文件:    " << file_index << " 个 MP3" << std::endl;
    std::cerr << "═══════════════════════════════════" << std::endl;

    return EXIT_SUCCESS;
}

// ============================================================================
// UART 串口模式：串口数据 → LLM → TTS
// ============================================================================

static int run_uart_mode(bool test_opus_only = false,
                          const std::string& test_text = "",
                          const std::string& test_mp3 = "") {
    bool test_downlink = !test_text.empty() || !test_mp3.empty();
    std::cerr << "UART 模式: " << UART_DEVICE << " " << UART_BAUD_RATE << " baud"
              << (test_opus_only ? " [仅测试OPUS]" : "")
              << (test_downlink ? " [下行测试]" : "") << std::endl;

    UART uart(std::string(UART_DEVICE), UART_BAUD);
    if (!uart.open()) {
        std::cerr << "❌ 打开串口失败" << std::endl;
        return EXIT_FAILURE;
    }

    uart.start_receive();
    uart.rx_buffer().clear();
    uart_sender_set_fd(uart.fd());
    std::cerr << "串口 RX 线程已启动, 等待数据..." << std::endl;

    // -- 下行测试模式 --
    if (test_downlink) {
        std::cerr << "\n===== UART 下行测试 =====\n" << std::endl;
        sleep(1);

        if (!test_text.empty()) {
            std::ifstream f(test_text);
            if (f) {
                std::string content((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                std::cerr << "发送文本(" << content.size() << "B): " << content << std::endl;
                for (int i = 0; i < 3; ++i) {  // 发 3 遍方便测试
                    uart_send_segment_text(content);
                    sleep(1);
                }
            } else {
                std::cerr << "无法打开: " << test_text << std::endl;
            }
        }

        if (!test_mp3.empty()) {
            std::ifstream f(test_mp3, std::ios::binary);
            if (f) {
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                           std::istreambuf_iterator<char>());
                std::cerr << "发送MP3(" << data.size() << "B)" << std::endl;
                uart_send_mp3(data);
            } else {
                std::cerr << "无法打开: " << test_mp3 << std::endl;
            }
        }

        std::cerr << "===== 下行测试结束 =====\n" << std::endl;
    }

    if (!test_opus_only) {
        tts_init();
        tts_set_segment_ready_callback(
            [](const std::string& text,
               const std::string& mp3_path,
               uint64_t delivery_generation) {
                // 协议 6.4/6.5：每段先发对应文本 0x13，再发 MP3 0x11。
                uart_send_segment(text, mp3_path, delivery_generation);
            });
    }

    Parser parser(uart.rx_buffer(), uart);
    OpusAccumulator opus_acc;
    uart_protocol::TextQuestionAccumulator text_acc;
    std::optional<std::chrono::steady_clock::time_point> uart_audio_start;
    std::atomic<uint64_t> active_task{0};
    std::mutex task_state_mutex;

    //添加任务状态
    enum class TaskPhase {
    Idle,        // 无任务
    Capturing,   // 正在接收 OPUS
    Processing   // ASR / LLM / TTS / UART下发
    };

    TaskPhase task_phase = TaskPhase::Idle;


    uint64_t capturing_task = 0;
    uint64_t capturing_tts_generation = 0;
    uint64_t capturing_uart_generation = 0;
    uint64_t text_task = 0;
    uint64_t text_tts_generation = 0;
    uint64_t text_uart_generation = 0;
    bool capturing_audio = false;
    bool handshake_ok = false;
    bool missing_handshake_reported = false;
    std::atomic<AiRuntimeState> ai_state{AiRuntimeState::Running};
    uart_protocol::HandshakeInfo handshake_info;
    std::string latest_device_info;
    std::string latest_sleep_data;

    auto report_ai_error = [&](const std::string& error) {
        std::cerr << "[协议异常] " << error << std::endl;
        if (!uart_send_ai_info(make_ai_info_json(
                ai_runtime_state_text(ai_state.load()), error))) {
            std::cerr << "[协议异常] 0x14 错误回报下发失败"
                      << std::endl;
        }
    };

    // auto cancel_all_tasks = [&]() {
    //     std::lock_guard<std::mutex> task_lock(task_state_mutex);
    //     active_task.fetch_add(1);
    //     uart_sender_cancel_current();
    //     opus_acc.reset();
    //     text_acc.reset();
    //     capturing_audio = false;
    //     uart_audio_start.reset();
    //     if (!test_opus_only) tts_cancel_current();
    // };

    //增加日志
    auto cancel_all_tasks = [&](const char* reason) {

        uint64_t old_task = 0;
        bool had_task = false;
        //先让旧任务逻辑失效
        {
            std::lock_guard<std::mutex> lock(task_state_mutex);
            old_task = active_task.load();

            had_task = (task_phase != TaskPhase::Idle);

            // generation +1：
            // 所有旧 ASR/LLM 回调看到 task_id 不一致后立即退出
            active_task.fetch_add(1);

            task_phase = TaskPhase::Idle;

            capturing_audio = false;
            capturing_task = 0;
            uart_audio_start.reset();

        }
    if (had_task) {
        std::cerr
            << "[中断] 取消任务 "
            << old_task
            << "，原因: "
            << reason
            << std::endl;
    }

    /*
     * 这些不要放在 task_state_mutex 里面。
     * 否则 UART/TTS 如果阻塞，会导致新的 STATE=1
     * 无法及时抢到 task_state_mutex。
     */

        // 停止旧任务后续 UART 下发
        uart_sender_cancel_current();

        // 停止旧 TTS
        if (!test_opus_only) {
            tts_cancel_current();
        }

        // 清空旧输入
        opus_acc.reset();
        text_acc.reset();
    };

    // auto start_new_task = [&](std::chrono::steady_clock::time_point start) {
    //     std::lock_guard<std::mutex> task_lock(task_state_mutex);
    //     const uint64_t task_id = active_task.fetch_add(1) + 1;
    //     uart_sender_cancel_current();
    //     opus_acc.reset();
    //     text_acc.reset();
    //     if (!test_opus_only) {
    //         uart_sender_set_input_start(start);
    //         tts_set_start_time(start);
    //     }
    //     return task_id;
    // };

auto start_new_task = [&](std::chrono::steady_clock::time_point start, bool interrupt_old) {

    uint64_t old_task = 0;
    uint64_t task_id = 0;
    bool had_old_task = false;

    {
        std::lock_guard<std::mutex> lock(task_state_mutex);
        old_task = active_task.load();

        had_old_task =
            task_phase != TaskPhase::Idle;

        /*
         * 新任务只增加一次 generation。
         *
         * 旧后台线程：
         * task_id != active_task
         * 就会自动失效。
         */
        task_id =
            active_task.fetch_add(1) + 1;

        task_phase = TaskPhase::Capturing;

        capturing_audio = true;
        uart_audio_start = start;
    }
    
    /*
     * 新任务开始时建立新的 UART / TTS generation。
     *
     * 如果确实正在打断旧任务，则这里也会立即停止
     * 旧 UART 和旧 TTS。
     */
    uart_sender_cancel_current();

    if (!test_opus_only) {
        tts_cancel_current();
    }

    opus_acc.reset();
    text_acc.reset();

    if (!test_opus_only) {
        uart_sender_set_input_start(start);
        tts_set_start_time(start);
    }

    if (interrupt_old && had_old_task) {
        std::cerr
            << "[中断] 取消任务 "
            << old_task
            << "，启动任务 "
            << task_id
            << std::endl;
    } else {
        std::cerr
            << "[任务] 启动任务 "
            << task_id
            << std::endl;
    }

    return task_id;
    };

auto run_llm_reply =
    [&](const std::string& text,
        uint64_t task_id,
        uint64_t tts_generation,
        uint64_t uart_generation) {

    auto check_cancelled = [&]() {
        if (task_id != active_task.load()) {
            throw TaskCancelled();
        }
    };


    // ------------------------------------------------------------
    // 提问文本
    // ------------------------------------------------------------

    check_cancelled();

    if (!uart_send_question_text(text)) {
        throw std::runtime_error(
            "提问文本 0x13 下发失败");
    }

    check_cancelled();


    // ------------------------------------------------------------
    // LLM
    // ------------------------------------------------------------

    std::cerr
        << "  [AI] 处理中..."
        << std::endl;

    const std::string reply =
        stream_llm_to_tts(
            {{"user", text}},

            [](const std::string& delta) {
                std::cerr
                    << delta
                    << std::flush;
            },

            [&, task_id] {
                return
                    task_id != active_task.load();
            },

            tts_generation,
            uart_generation);


    check_cancelled();


    if (reply.empty()) {
        throw std::runtime_error(
            "LLM 返回空回复");
    }

    std::cerr << std::endl;


    // ------------------------------------------------------------
    // TTS
    // ------------------------------------------------------------

    // if (!tts_wait_current(tts_generation)) {

    //     check_cancelled();

    //     throw std::runtime_error(
    //         "TTS 分段生成失败");
    // }
    std::cerr
    << "[TTS DEBUG] 准备等待 generation="
    << tts_generation
    << std::endl;

    bool tts_ok =
        tts_wait_current(tts_generation);

    std::cerr
        << "[TTS DEBUG] tts_wait_current 返回="
        << tts_ok
        << " generation="
        << tts_generation
        << std::endl;

    if (!tts_ok) {

        check_cancelled();

        throw std::runtime_error(
            "TTS 分段生成失败");
    }


    check_cancelled();


    // ------------------------------------------------------------
    // UART 回答结束
    // ------------------------------------------------------------

    if (!uart_finish_response()) {

        check_cancelled();

        throw std::runtime_error(
            "TTS 未生成可下发的 0x11 音频");
    }


    check_cancelled();


    if (!uart_send_control_if_present(text)) {

        check_cancelled();

        throw std::runtime_error(
            "控制指令 0x12 下发失败");
    }


    // ------------------------------------------------------------
    // 整轮真正结束
    // ------------------------------------------------------------

    {
        std::lock_guard<std::mutex> lock(task_state_mutex);

        if (task_id != active_task.load()) {
            throw TaskCancelled();
        }

        task_phase =
            TaskPhase::Idle;

        capturing_audio = false;
        capturing_task = 0;
    }

    std::cerr
        << "[任务] 任务 "
        << task_id
        << " 已完成，进入 Idle"
        << std::endl;
};

    // auto run_llm_reply = [&](const std::string& text,
    //                          uint64_t task_id,
    //                          uint64_t tts_generation,
    //                          uint64_t uart_generation) {
    //     if (task_id != active_task.load()) throw TaskCancelled();
    //     {
    //         std::lock_guard<std::mutex> task_lock(task_state_mutex);
    //         if (task_id != active_task.load()) throw TaskCancelled();
    //         if (!uart_send_question_text(text))  // 0x13/STATE=0
    //             throw std::runtime_error("提问文本 0x13 下发失败");
    //     }
    //     std::cerr << "  [AI] 处理中..." << std::endl;
    //     const std::string reply = stream_llm_to_tts(
    //         {{"user", text}},
    //         [](const std::string& delta) {
    //             std::cerr << delta << std::flush;
    //         },
    //         [&, task_id] { return task_id != active_task.load(); },
    //         tts_generation,
    //         uart_generation);
    //     if (reply.empty()) throw std::runtime_error("LLM 返回空回复");
    //     std::cerr << std::endl;
    //     if (!tts_wait_current(tts_generation))
    //         throw std::runtime_error("TTS 分段生成失败或已取消");
    //     {
    //         std::lock_guard<std::mutex> task_lock(task_state_mutex);
    //         if (task_id != active_task.load()) throw TaskCancelled();
    //         if (!uart_finish_response())
    //             throw std::runtime_error("TTS 未生成可下发的 0x11 音频");
    //         // 新协议 6.5 仍规定播报结束后发送 0x12；协议未定义控制 ACK。
    //         if (!uart_send_control_if_present(text))
    //             throw std::runtime_error("控制指令 0x12 下发失败");
    //     }
    // };

    // auto handle_task_exception = [&](uint64_t task_id,
    //                                  const std::exception& error,
    //                                  const char* label) {
    //     std::lock_guard<std::mutex> task_lock(task_state_mutex);
    //     if (task_id != active_task.load()) {
    //         std::cerr << "\n[中断] 旧" << label << "任务 " << task_id
    //                   << " 已取消" << std::endl;
    //         return;
    //     }
    //     // 使失败任务立即失效，拒绝同一任务迟到的回调。
    //     active_task.fetch_add(1);
    //     uart_sender_cancel_current();
    //     if (!test_opus_only) tts_cancel_current();
    //     report_ai_error(error.what());
    // };

auto handle_task_exception =
    [&](uint64_t task_id,
        const std::exception& error,
        const char* label) {

    uint64_t current_task = 0;
    bool is_current_task = false;

    {
        std::lock_guard<std::mutex>
            lock(task_state_mutex);

        current_task =
            active_task.load();

        // 这是已经被 STATE=1 淘汰的旧任务
        if (task_id != current_task) {

            std::cerr
                << "[中断] 旧"
                << label
                << "任务 "
                << task_id
                << " 已停止"
                << std::endl;

            return;
        }

        is_current_task = true;

        // 当前任务异常，立即使它失效
        active_task.fetch_add(1);

        task_phase =
            TaskPhase::Idle;

        capturing_audio = false;
        capturing_task = 0;
        uart_audio_start.reset();
    }


    if (!is_current_task)
        return;


    // 不持 task_state_mutex 执行这些耗时操作

    uart_sender_cancel_current();

    if (!test_opus_only) {
        tts_cancel_current();
    }

    report_ai_error(error.what());
};

    opus_acc.set_decode_callback([&](const std::string& wav_path,
                                      const std::string& info) {
        const uint64_t task_id = capturing_task;
        const uint64_t tts_generation = capturing_tts_generation;
        const uint64_t uart_generation = capturing_uart_generation;
        const auto request_start =
            uart_audio_start.value_or(std::chrono::steady_clock::now());
        uart_audio_start.reset();

        std::cerr << "\n[OPUS→WAV] " << info << std::endl;
        std::cerr << "  WAV: " << wav_path << std::endl;
        if (test_opus_only) {
            std::cerr << "  [测试模式] 播放: aplay -D " << AUDIO_DEVICE
                      << " " << wav_path << std::endl;
            return;
        }

        // ASR → 0x13提问文本 → LLM → TTS 放入后台。
        std::thread([&, task_id, tts_generation, uart_generation,
                     request_start, wav_path]() {
            TemporaryAudioFile wav_cleanup(wav_path);
            try {
                std::cerr
                << "[ASR] 任务 "
                << task_id
                << " 开始识别"
                << std::endl;
                /*
                * ASR 返回以后第一件事情就是检查任务是否已经被打断。
                *
                * 如果 STATE=1 在 ASR 过程中到来：
                *
                * active_task 已经变成新任务 ID，
                * 所以旧 ASR 结果直接丢弃。
                */
                std::string text = asr_transcribe(wav_path);
                if (task_id != active_task.load()) throw TaskCancelled();


                if (text.empty()) throw std::runtime_error("ASR 识别结果为空");
                std::cerr << "  ASR: " << text << std::endl;
                const auto latency_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - request_start)
                        .count();
                std::cerr << "[延迟] UART音频首帧→ASR完成: "
                          << latency_ms << " ms" << std::endl;
                run_llm_reply(
                    text, task_id, tts_generation, uart_generation);
            } catch (const TaskCancelled& e) {
                handle_task_exception(task_id, e, "语音");
            } catch (const std::exception& e) {
                handle_task_exception(task_id, e, "语音");
            }
        }).detach();
    });

    parser.set_frame_callback([&](const DataFrame& frame) {
        if (frame.type == uart_protocol::TYPE_HANDSHAKE) {
            // 任意 0xAA 都是会话边界。即使 CHECK/密钥错误，
            // 也必须先撤销旧会话，不得继续沿用旧授权。
            handshake_ok = false;
            missing_handshake_reported = false;
            cancel_all_tasks("收到新握手");
            if (!frame.checksum_ok) {
                report_ai_error("握手 UART CHECK 校验失败");
                return;
            }


            
            // if (!frame.dsid_ok || frame.slice != 1) {
            //     report_ai_error("握手 DSID 必须为 1");
            //     return;
            // }

            //只有0x01和0x02的DSID不为0，其余命令都不用判断DSID
            const bool need_dsid_check =
                frame.type == 0x01 ||
                frame.type == 0x02;

            if (need_dsid_check) {
                if (!frame.dsid_ok || frame.slice == 0) {
                    report_ai_error("TYPE=0x01/0x02 的 DSID 不能为 0");
                    return;
                }
            }


            const auto result =
                uart_protocol::parse_handshake(frame.data, frame.status);
            if (!result.ok) {
                report_ai_error(result.error);
                return;
            }
            handshake_info = result.info;
            std::cerr << "[握手] 通过，IMEI=" << handshake_info.imei
                      << "，经度=" << handshake_info.longitude
                      << "，纬度=" << handshake_info.latitude << std::endl;
            if (!uart_send_ai_info(make_ai_info_json(
                    ai_runtime_state_text(ai_state.load())))) {
                std::cerr << "[握手] 0x14 回复下发失败" << std::endl;
                handshake_info = {};
                return;
            }
            // 只有 MCU 可以收到成功回复后，才对业务帧开放新会话。
            parser.accept_sequence_baseline(frame.seq);
            handshake_ok = true;
            missing_handshake_reported = false;
            return;
        }

        if (!frame.checksum_ok) {
            report_ai_error("UART CHECK 校验失败");
            return;
        }
        if (!uart_protocol::is_uplink_type(frame.type)) {
            report_ai_error("收到方向错误的下行 TYPE");
            return;
        }

        if (!frame.dsid_ok) {
            report_ai_error("新协议禁止 DSID=0");
            return;
        }


        // const bool is_fresh_question =
        // (frame.type == uart_protocol::TYPE_AUDIO_QUESTION ||
        // frame.type == uart_protocol::TYPE_TEXT_QUESTION) &&
        // frame.status == uart_protocol::QUESTION_STATE_WAKE;

        // // STATE=WAKE 仍然负责新问题打断旧任务。
        // // 此逻辑不再依赖 seq 是否连续。
        // if (is_fresh_question) {
        //     cancel_all_tasks();
        // }
        // if (!frame.sequence_ok) {
        //     const bool is_fresh_question =
        //         (frame.type == uart_protocol::TYPE_AUDIO_QUESTION ||
        //          frame.type == uart_protocol::TYPE_TEXT_QUESTION) &&
        //         frame.status == uart_protocol::QUESTION_STATE_WAKE;
        //     // STATE=1 的“立即打断”优先于慢速的 0x14 错误回报。
        //     if (is_fresh_question) cancel_all_tasks();
        //     const std::string error =
        //         "UART SEQ 不连续，期望 " +
        //         std::to_string(frame.expected_seq) + "，实际 " +
        //         std::to_string(frame.seq);
        //     report_ai_error(error);
        //     if (!is_fresh_question) {
        //         if (frame.type == uart_protocol::TYPE_AUDIO_QUESTION) {
        //             opus_acc.reset();
        //             capturing_audio = false;
        //         }
        //         if (frame.type == uart_protocol::TYPE_TEXT_QUESTION) {
        //             text_acc.reset();
        //         }
        //         return;
        //     }
        // }

        if (!handshake_ok) {
            if (!missing_handshake_reported) {
                report_ai_error("业务帧被拒绝：尚未完成 0xAA 握手");
                missing_handshake_reported = true;
            }
            return;
        }

        switch (frame.type) {

            case uart_protocol::TYPE_AUDIO_QUESTION: {

                if (ai_state.load() != AiRuntimeState::Running) {
                    report_ai_error("AI 当前不是启动状态，忽略音频提问");
                    return;
                }

                std::cerr
                    << "[AUDIO]"
                    << " STATE=" << static_cast<int>(frame.status)
                    << " DSID=" << frame.slice
                    << " SEQ=" << static_cast<int>(frame.seq)
                    << " LEN=" << frame.data.size()
                    << " active_task=" << active_task.load()
                    << std::endl;

                // 空 NULL 帧直接忽略
                if (frame.status == uart_protocol::QUESTION_STATE_NULL &&
                    frame.data.empty()) {

                    std::cerr
                        << "[UART↑] 忽略空音频 NULL 帧"
                        << std::endl;

                    return;
                }

                // 读取当前任务状态
                TaskPhase phase;

                {
                    std::lock_guard<std::mutex> lock(task_state_mutex);
                    phase = task_phase;
                }


                // ============================================================
                // STATE = 1
                //
                // Idle:
                //     直接启动新任务
                //
                // Capturing / Processing:
                //     立即打断旧任务，并启动新任务
                // ============================================================

                if (frame.status ==
                    uart_protocol::QUESTION_STATE_WAKE) {

                    const bool interrupt_old =
                        phase != TaskPhase::Idle;

                    const auto start =
                        std::chrono::steady_clock::now();

                    capturing_task =
                        start_new_task(
                            start,
                            interrupt_old);

                    capturing_tts_generation =
                        test_opus_only
                            ? 0
                            : tts_current_generation();

                    capturing_uart_generation =
                        uart_sender_current_generation();

                    std::cerr
                        << "[语音] STATE=1，开始接收任务 "
                        << capturing_task
                        << std::endl;
                }


                // ============================================================
                // STATE = 2
                //
                // Idle:
                //     自动创建任务
                //
                // Capturing:
                //     当前任务继续收音频
                //
                // Processing:
                //     不允许 STATE=2 打断旧任务
                // ============================================================

                else if (frame.status == 2) {

                    if (phase == TaskPhase::Idle) {

                        const auto start =
                            std::chrono::steady_clock::now();

                        capturing_task =
                            start_new_task(
                                start,
                                false);

                        capturing_tts_generation =
                            test_opus_only
                                ? 0
                                : tts_current_generation();

                        capturing_uart_generation =
                            uart_sender_current_generation();

                        std::cerr
                            << "[语音] STATE=2 自动启动任务 "
                            << capturing_task
                            << std::endl;
                    }

                    else if (phase == TaskPhase::Processing) {

                        std::cerr
                            << "[语音] 当前任务 "
                            << active_task.load()
                            << " 正在处理，忽略 STATE=2"
                            << std::endl;

                        return;
                    }

                    // Capturing：
                    // 什么都不做，继续往下面 opus_acc.feed()
                }


                // ============================================================
                // STATE = 3
                //
                // 音频输入结束，进入 Processing。
                // Processing 包含：
                //
                // ASR -> LLM -> TTS -> UART
                // ============================================================

                else if (frame.status ==
                        uart_protocol::QUESTION_STATE_END) {

                    if (phase == TaskPhase::Idle) {

                        report_ai_error(
                            "收到 STATE=3，但当前没有活动语音任务");

                        return;
                    }

                    if (phase == TaskPhase::Processing) {

                        std::cerr
                            << "[语音] 当前任务已经进入处理阶段，"
                            "忽略重复 STATE=3"
                            << std::endl;

                        return;
                    }

                    /*
                    * 必须在 feed(STATE=3) 之前进入 Processing。
                    *
                    * 因为 opus_acc.feed() 可能同步触发
                    * decode callback。
                    */
                    {
                        std::lock_guard<std::mutex> lock(task_state_mutex);

                        task_phase =
                            TaskPhase::Processing;

                        capturing_audio = false;
                    }

                    std::cerr
                        << "[语音] 任务 "
                        << capturing_task
                        << " 音频接收完成，进入 Processing"
                        << std::endl;
                }


                // ============================================================
                // 其它非法 STATE
                // ============================================================

                else {

                    report_ai_error(
                        "未知音频 STATE=" +
                        std::to_string(frame.status));

                    return;
                }


                // ============================================================
                // STATE=1 / 2 / 3 的数据最终都送入 OPUS
                // ============================================================

                opus_acc.feed(
                    frame.data,
                    frame.status,
                    frame.slice,
                    frame.seq);


                if (const std::string error =
                        opus_acc.take_error();
                    !error.empty()) {

                    report_ai_error(error);

                    // OPUS 已经出错，本轮任务不能继续悬挂
                    cancel_all_tasks("OPUS数据异常");

                    return;
                }

                break;
            }

        case uart_protocol::TYPE_TEXT_QUESTION: {
            if (ai_state.load() != AiRuntimeState::Running) {
                report_ai_error(
                    "AI 当前不是启动状态，忽略文本提问");
                return;
            }

            std::cerr
                << "[TEXT]"
                << " STATE=" << static_cast<int>(frame.status)
                << " DSID=" << frame.slice
                << " SEQ=" << static_cast<int>(frame.seq)
                << " LEN=" << frame.data.size()
                << " active_task=" << active_task.load()
                << std::endl;


            // ============================================================
            // STATE = 1
            //
            // 当前无任务：
            //     正常启动文本任务
            //
            // 当前有任务：
            //     打断旧任务，包括：
            //     - UART 下发
            //     - TTS
            //     - 旧 LLM 结果
            //     - 旧 ASR 结果
            // ============================================================

            if (frame.status ==
                uart_protocol::QUESTION_STATE_WAKE) {

                TaskPhase phase;

                {
                    std::lock_guard<std::mutex> lock(task_state_mutex);
                    phase = task_phase;
                }

                const bool interrupt_old =
                    phase != TaskPhase::Idle;

                const auto start =
                    std::chrono::steady_clock::now();

                text_task =
                    start_new_task(
                        start,
                        interrupt_old);

                /*
                * start_new_task() 当前里面会把
                * capturing_audio=true，
                * 但这里是文本任务，所以必须重新改回来。
                */
                {
                    std::lock_guard<std::mutex> lock(task_state_mutex);

                    capturing_audio = false;
                    uart_audio_start.reset();
                }

                text_tts_generation =
                    test_opus_only
                        ? 0
                        : tts_current_generation();

                text_uart_generation =
                    uart_sender_current_generation();


                    //调试打印
                    // std::cerr
                    // << "[GEN DEBUG]"
                    // << " task=" << capturing_task
                    // << " tts_gen=" << capturing_tts_generation
                    // << " uart_gen=" << capturing_uart_generation
                    // << std::endl;

                    std::cerr
                        << "[文本] 开始接收任务 "
                        << text_task
                        << std::endl;
            }


            // ============================================================
            // 如果没有文本任务，却直接收到后续数据
            // ============================================================

            else {

                TaskPhase phase;

                {
                    std::lock_guard<std::mutex> lock(task_state_mutex);
                    phase = task_phase;
                }

                /*
                * 如果这里你协议规定：
                * 文本必须由 STATE=1 开始，
                * 那么 Idle 状态收到后续帧直接拒绝。
                */
                if (phase == TaskPhase::Idle ||
                    text_task == 0) {

                    report_ai_error(
                        "文本提问未由 STATE=1 开始");

                    return;
                }

                /*
                * 如果当前已经 Processing，
                * 后续 STATE=2/3 不属于新的合法文本输入。
                */
                if (phase == TaskPhase::Processing) {

                    std::cerr
                        << "[文本] 当前任务正在处理，"
                        "忽略后续文本帧"
                        << std::endl;

                    return;
                }
            }


            // ============================================================
            // 文本分片累计
            // ============================================================

            const auto result =
                text_acc.feed(
                    frame.data,
                    frame.status,
                    frame.slice);


            // ============================================================
            // 文本组包异常
            // ============================================================

            if (result.status ==
                uart_protocol::TextQuestionAccumulator::Status::Error) {

                report_ai_error(result.error);

                /*
                * 当前任务已经无法继续，
                * 必须把状态恢复到 Idle。
                */
                cancel_all_tasks("文本提问组包失败");

                return;
            }


            // ============================================================
            // 文本接收完整
            // ============================================================

            if (result.status ==
                uart_protocol::TextQuestionAccumulator::Status::Complete) {

                if (test_opus_only) {

                    std::cerr
                        << "[文本提问] "
                        << result.text
                        << std::endl;

                    /*
                    * 测试模式也要恢复 Idle，
                    * 否则下一轮会一直被认为有活动任务。
                    */
                    {
                        std::lock_guard<std::mutex> lock(task_state_mutex);

                        if (text_task == active_task.load()) {
                            task_phase = TaskPhase::Idle;
                            text_task = 0;
                        }
                    }

                    return;
                }


                // --------------------------------------------------------
                // Capturing -> Processing
                // --------------------------------------------------------

                {
                    std::lock_guard<std::mutex> lock(task_state_mutex);

                    /*
                    * 在准备进入后台线程之前再检查一次。
                    *
                    * 如果刚好被新的 STATE=1 打断，
                    * 当前文本结果就直接丢弃。
                    */
                    if (text_task != active_task.load()) {

                        std::cerr
                            << "[中断] 文本任务 "
                            << text_task
                            << " 已失效"
                            << std::endl;

                        return;
                    }

                    task_phase =
                        TaskPhase::Processing;
                }


                const uint64_t task_id =
                    text_task;

                const uint64_t generation =
                    text_tts_generation;

                const uint64_t uart_generation =
                    text_uart_generation;

                const std::string text =
                    result.text;


                std::cerr
                    << "[文本] 任务 "
                    << task_id
                    << " 接收完成，进入 Processing"
                    << std::endl;


                // --------------------------------------------------------
                // LLM -> TTS -> UART 放到后台
                // --------------------------------------------------------

                std::thread(
                    [&, task_id,
                        generation,
                        uart_generation,
                        text]() {

                    try {

                        /*
                        * 后台线程刚启动再检查一次。
                        *
                        * 防止线程创建到真正执行之间，
                        * 新 STATE=1 已经把它取消。
                        */
                        if (task_id != active_task.load()) {
                            throw TaskCancelled();
                        }

                        run_llm_reply(
                            text,
                            task_id,
                            generation,
                            uart_generation);

                    } catch (const TaskCancelled& e) {

                        handle_task_exception(
                            task_id,
                            e,
                            "文本");

                    } catch (const std::exception& e) {

                        handle_task_exception(
                            task_id,
                            e,
                            "文本");
                    }

                }).detach();
            }

            break;
        }

        case uart_protocol::TYPE_DEVICE_INFO:
        case uart_protocol::TYPE_SLEEP_DATA: {
            if (frame.slice != 1) {
                report_ai_error("设备/睡眠 JSON 必须在单帧 DSID=1 内发送");
                return;
            }
            std::string json;
            std::string error;
            if (!validate_json_object(frame.data, json, error)) {
                report_ai_error(std::string(uart_protocol::type_name(frame.type)) +
                                ": " + error);
                return;
            }
            if (frame.type == uart_protocol::TYPE_DEVICE_INFO) {
                latest_device_info = std::move(json);
                std::cerr << "[UART↑] 已更新设备信息，"
                          << latest_device_info.size() << " 字节" << std::endl;
            } else {
                latest_sleep_data = std::move(json);
                std::cerr << "[UART↑] 已更新睡眠数据，"
                          << latest_sleep_data.size() << " 字节" << std::endl;
            }
            break;
        }

        case uart_protocol::TYPE_AI_STATE:
        case uart_protocol::TYPE_AI_STATE_LEGACY: {
            if (frame.type == uart_protocol::TYPE_AI_STATE_LEGACY) {
                std::cerr << "[兼容] 收到协议正文误写的 TYPE=0x06；"
                             "正式值应为 0x05" << std::endl;
            }
            if (frame.slice != 1 || frame.data.size() != 1) {
                report_ai_error("设置 AI 状态要求 DSID=1、CODE=1字节");
                return;
            }
            switch (frame.data[0]) {
            case 0x00:
                break;
            case 0x01:
                ai_state.store(AiRuntimeState::Sleeping);
                cancel_all_tasks("AI进入休眠");
                break;
            case 0x02:
                ai_state.store(AiRuntimeState::Running);
                break;
            case 0x03:
                ai_state.store(AiRuntimeState::EnergySaving);
                cancel_all_tasks("AI进入节能状态");
                break;
            default:
                report_ai_error("未知 AI 状态值 " +
                                std::to_string(frame.data[0]));
                return;
            }
            if (!uart_send_ai_info(make_ai_info_json(
                    ai_runtime_state_text(ai_state.load())))) {
                std::cerr << "[AI状态] 0x14 回复下发失败"
                          << std::endl;
            }
            break;
        }

        default:
            report_ai_error("未处理的上行 TYPE");
            break;
        }
    });

    // 主循环：30ms 轮询
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        parser.process();
    }

    return EXIT_SUCCESS;
}

// ============================================================================
// 入口
// ============================================================================

int main(int argc, char* argv[]) {
    {
        std::error_code ec;
        const std::filesystem::path temp_dir{
            std::string(PIPELINE_TEMP_DIR)};
        std::filesystem::create_directories(temp_dir, ec);
        if (!ec) {
            std::filesystem::permissions(
                temp_dir, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace, ec);
        }
        if (ec)
            std::cerr << "无法创建临时目录: " << ec.message() << std::endl;
    }

    // TTS 测试: ./audio_pipeline --test-tts [text]
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--test-tts") == 0) {
            std::string text = (i + 1 < argc) ? argv[i + 1] : "你好，TTS测试。";
            std::string out = (i + 2 < argc)
                ? argv[i + 2] : "/tmp/audio-pipeline/tts_test.mp3";
            std::cerr << "TTS 测试: " << text << " → " << out << std::endl;
            tts_to_file(strip_emoji(strip_markdown(text)), out);
            std::cerr << "完成: " << out << std::endl;
            return 0;
        }
    }

    // UART 模式: ./audio_pipeline -u [--test-opus] [--test-downlink text.txt mp3.mp3]
    bool uart_mode = false, test_opus = false;
    std::string downlink_text, downlink_mp3;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--uart") == 0)
            uart_mode = true;
        if (strcmp(argv[i], "--test-opus") == 0)
            test_opus = true;
        if (strcmp(argv[i], "--test-downlink") == 0 && i + 2 < argc) {
            downlink_text = argv[i + 1];
            downlink_mp3  = argv[i + 2];
            i += 2;
        }
    }
    if (uart_mode) return run_uart_mode(test_opus, downlink_text, downlink_mp3);

    // 文件模式: ./audio_pipeline -f input.opus -o output.mp3
    std::string file_input, file_output;
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) && i + 1 < argc)
            file_input = argv[++i];
        else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc)
            file_output = argv[++i];
    }
    if (!file_input.empty())
        return run_file_mode(file_input, file_output.empty() ? "output.mp3" : file_output);

    if (argc >= 2) {
        std::string input;
        for (int i = 1; i < argc; ++i) {
            if (i > 1) input += " ";
            input += argv[i];
        }
        return run_once(input);
    }
    return run_interactive();
}
