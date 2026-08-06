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
#include "uart_sender.h"

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
    system(cmd.str().c_str());

    std::cerr << "识别中..." << std::flush;
    //调用ASR服务
    std::string text = asr_transcribe(tmp_wav);
    //识别完成后删除临时文件
    std::remove(tmp_wav.c_str());
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
    uint64_t tts_generation = UINT64_MAX) {
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
        // 处理每一个完整的句子
        for (auto& sentence : sentences) {

            if (is_cancelled && is_cancelled()) throw TaskCancelled();
            // 清洗TTS文本
            std::string clean = strip_emoji(strip_markdown(sentence));
            // 提交给TTS
            if (!clean.empty()) tts_speak_async(clean, tts_generation);
        }
    });

    // LLM 结束时，末尾没有句号的内容也必须送入 TTS。
    std::string remaining = splitter.flush();
    if (is_cancelled && is_cancelled()) throw TaskCancelled();
    std::string clean = strip_emoji(strip_markdown(remaining));
    if (!clean.empty()) tts_speak_async(clean, tts_generation);
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
static void uart_send_control_if_present(const std::string& user_text) {
    auto control = build_control_json(user_text);
    //没有控制指令直接退出
    if (!control) return;
    std::cerr << "[UART↓] 控制JSON: " << *control << std::endl;
    uart_send_control(*control);
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
    long first_tts_ms = 0;

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
    std::cerr << "UART 模式: " << UART_DEVICE << " " << UART_BAUD << " baud"
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
            [](const std::string& text, const std::string& mp3_path) {
                // 协议 6.4/6.5：每段先发对应文本 0x13，再发 MP3 0x11。
                uart_send_segment(text, mp3_path);
            });
    }

    Parser parser(uart.rx_buffer(), uart);
    OpusAccumulator opus_acc;
    std::optional<std::chrono::steady_clock::time_point> uart_audio_start;
    std::atomic<uint64_t> active_task{0};
    uint64_t capturing_task = 0;
    bool capturing_audio = false;

    opus_acc.set_decode_callback([&](const std::string& wav_path,
                                      const std::string& info) {
        const uint64_t task_id = capturing_task;
        const uint64_t tts_generation = tts_current_generation();
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

        // ASR → LLM → TTS 放入后台，UART 解析线程继续接收下一条语音。
        std::thread([&, task_id, tts_generation, request_start, wav_path]() {
          TemporaryAudioFile wav_cleanup(wav_path);
          try {
            std::string text = asr_transcribe(wav_path);
            if (task_id != active_task.load()) throw TaskCancelled();
            if (text.empty()) {
                std::cerr << "[ASR] 识别结果为空，本轮不进入 LLM/TTS；"
                          << "调试 WAV 已保留: " << wav_path << std::endl;
                return;
            }
            std::cerr << "  ASR: " << text << std::endl;
            const auto asr_latency_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - request_start)
                    .count();
            std::cerr << "[延迟] UART音频首帧→ASR完成: "
                      << asr_latency_ms << " ms" << std::endl;

            if (task_id != active_task.load()) throw TaskCancelled();
            std::string reply = stream_llm_to_tts(
                {{"user", text}},
                [](const std::string& delta) {
                    std::cerr << delta << std::flush;
                },
                [&, task_id] { return task_id != active_task.load(); },
                tts_generation);
            if (reply.empty()) return;
            std::cerr << std::endl;
            tts_wait_current();
            if (task_id != active_task.load()) throw TaskCancelled();
            uart_finish_response();
            if (task_id != active_task.load()) throw TaskCancelled();
            uart_send_control_if_present(text);
          } catch (const TaskCancelled&) {
            std::cerr << "\n[中断] 旧指令任务 " << task_id
                      << " 已取消" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "  [错误] " << e.what() << std::endl;
        }
        }).detach();
    });

    parser.set_frame_callback([&](const DataFrame& frame) {
        if (!frame.checksum_ok) return;

        if (frame.type == 0x01) {
            // 音频帧 → OPUS 累积
            // STATE=1 表示新唤醒；否则本轮第一个 0x01 帧作为计时起点。
            if (frame.status == 1 || !uart_audio_start.has_value()) {
                capturing_task = active_task.fetch_add(1) + 1;
                capturing_audio = true;
                uart_sender_cancel_current();
                uart_audio_start = std::chrono::steady_clock::now();
                if (!test_opus_only) {
                    uart_sender_set_input_start(*uart_audio_start);
                    tts_set_start_time(*uart_audio_start);
                }
                std::cerr << "\n[中断] 收到新语音，停止上一轮并启动任务 "
                          << capturing_task << std::endl;
                std::cerr << "[延迟] 收到本轮 UART 音频首帧"
                          << " (STATE=" << static_cast<int>(frame.status)
                          << ")" << std::endl;
            }
            if (opus_acc.feed(frame.data, frame.status, frame.slice, frame.seq))
                capturing_audio = false;
        } else if (frame.type >= 0x02 && frame.type <= 0x04) {
            // 文本帧 (JSON/UTF-8) → 直接送 LLM
            std::string text(frame.data.begin(), frame.data.end());
            std::cerr << "\n[UART TYPE=0x" << std::hex << (int)frame.type << std::dec
                      << "] " << text << std::endl;

            if (!test_opus_only) {
                const uint64_t task_id = active_task.fetch_add(1) + 1;
                uart_sender_cancel_current();
                const auto text_start = std::chrono::steady_clock::now();
                uart_sender_set_input_start(text_start);
                tts_set_start_time(text_start);
                const uint64_t tts_generation = tts_current_generation();
                std::thread([&, task_id, tts_generation, text]() {
                  try {
                    std::cerr << "  [AI] 处理中..." << std::endl;
                    std::string reply = stream_llm_to_tts(
                        {{"user", text}},
                        [](const std::string& delta) {
                            std::cerr << delta << std::flush;
                        },
                        [&, task_id] {
                            return task_id != active_task.load();
                        },
                        tts_generation);
                    std::cerr << std::endl;
                    tts_wait_current();
                    if (task_id != active_task.load()) throw TaskCancelled();
                    uart_finish_response();
                    if (task_id != active_task.load()) throw TaskCancelled();
                    uart_send_control_if_present(text);
                  } catch (const TaskCancelled&) {
                    std::cerr << "\n[中断] 旧文本任务 " << task_id
                              << " 已取消" << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "  [错误] " << e.what() << std::endl;
                }
                }).detach();
            }
        } else {
            // 下行帧或其他
            std::string text(frame.data.begin(), frame.data.end());
            std::cerr << "[UART TYPE=0x" << std::hex << (int)frame.type << std::dec
                      << "] " << text << std::endl;
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
        if (!DEBUG_SAVE_AUDIO && !ec) {
            for (std::filesystem::directory_iterator it(temp_dir, ec), end;
                 !ec && it != end; it.increment(ec))
                std::filesystem::remove_all(it->path(), ec);
        }
    }

    // TTS 测试: ./audio_pipeline --test-tts [text]
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--test-tts") == 0) {
            std::string text = (i + 1 < argc) ? argv[i + 1] : "你好，TTS测试。";
            std::string out  = (i + 2 < argc) ? argv[i + 2] : "/home/yu/workspace/tts_test.mp3";
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
