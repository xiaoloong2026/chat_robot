#include "tts_client.h"

#include "config.h"
#include "json_helper.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

// ============================================================================
// MOSS-TTS-Nano API:
//   1. POST /api/generate-stream/start (multipart form) → JSON {stream_id}
//   2. GET  /api/generate-stream/{stream_id}/audio → PCM stream
//   输出 pcm_s16le / 48kHz / stereo → 转 24kHz mono WAV
// ============================================================================

static constexpr int TTS_SRC_RATE    = 48000;
static constexpr int TTS_SRC_CH      = 2;

// ============================================================================
// 播放队列 (复用原有的 aplay 串行播放逻辑)
// ============================================================================

namespace {

struct PlayItem { std::string wav_file; };
struct SegmentResult {
    uint64_t generation{};
    uint64_t delivery_generation{UINT64_MAX};
    std::string text;
    std::string temporary_mp3_file;
    std::string final_mp3_file;
    std::string wav_file;
};

std::mutex              g_play_mutex;
std::condition_variable g_play_cv;
std::queue<PlayItem>    g_play_queue;
bool                    g_play_done = false;
std::thread             g_play_thread;
std::atomic<uint64_t>    g_next_request_id{0};
std::atomic<uint64_t>    g_generation{0};

std::mutex                        g_result_mutex;
std::mutex                        g_delivery_mutex;
std::map<uint64_t, SegmentResult> g_result_pending;
std::set<uint64_t>                g_failed_generations;
uint64_t                          g_next_submit_sequence = 0;
uint64_t                          g_next_result_sequence = 0;
TtsSegmentReadyCallback           g_segment_ready_callback;

std::mutex                              g_stream_mutex;
std::map<uint64_t, std::set<std::string>> g_active_streams;

std::mutex                            g_start_mutex;
std::chrono::steady_clock::time_point g_request_start;
bool                                  g_first_audio = true;
bool                                  g_first_mp3_ready = true;
uint64_t                              g_output_segment = 0;

void play_loop() {
    pid_t prev_pid = -1;
    std::string prev_file;
    int seg = 0;

    while (true) {
        PlayItem item;
        {
            std::unique_lock<std::mutex> lk(g_play_mutex);
            g_play_cv.wait(lk, []{ return !g_play_queue.empty() || g_play_done; });
            if (g_play_queue.empty()) break;
            item = std::move(g_play_queue.front());
            g_play_queue.pop();
        }

        if (prev_pid > 0) {
            int status = 0;
            waitpid(prev_pid, &status, 0);
            if (!prev_file.empty()) std::remove(prev_file.c_str());
        }

        ++seg;
        bool is_first_audio = false;
        std::chrono::steady_clock::time_point request_start;
        {
            std::lock_guard<std::mutex> lk(g_start_mutex);
            if (g_first_audio) {
                g_first_audio = false;
                is_first_audio = true;
                request_start = g_request_start;
            }
        }
        if (is_first_audio) {
            auto now = std::chrono::steady_clock::now();
            auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - request_start).count();
            std::cerr << "[首音延迟] " << ms << " ms, 段 " << seg << "/" << std::flush;
        }

        pid_t child = fork();
        if (child == 0) {
            execlp("aplay", "aplay", "-q", "-D", AUDIO_DEVICE.data(),
                   item.wav_file.c_str(), nullptr);
            _exit(1);
        }
        prev_pid  = child;
        prev_file = std::move(item.wav_file);
    }

    if (prev_pid > 0) {
        int status = 0;
        waitpid(prev_pid, &status, 0);
    }
    if (!prev_file.empty()) std::remove(prev_file.c_str());
    if (seg > 0) std::cerr << seg << std::endl;
}

void remove_segment_files(const SegmentResult& result) {
    if (!result.temporary_mp3_file.empty())
        std::remove(result.temporary_mp3_file.c_str());
    if (!result.wav_file.empty())
        std::remove(result.wav_file.c_str());
}

void mark_generation_failed(uint64_t generation) {
    if (generation != g_generation.load()) return;
    std::lock_guard<std::mutex> result_lk(g_result_mutex);
    if (generation == g_generation.load())
        g_failed_generations.insert(generation);
}

void deliver_segment_result(SegmentResult ready,
                            const TtsSegmentReadyCallback& callback) {
    if (ready.generation != g_generation.load()) {
        remove_segment_files(ready);
        return;
    }

    std::string completed_mp3;
    if (!ready.temporary_mp3_file.empty()) {
        std::error_code rename_error;
        std::filesystem::rename(
            ready.temporary_mp3_file,
            ready.final_mp3_file,
            rename_error);
        if (rename_error) {
            std::cerr << "[TTS] MP3 完成文件重命名失败: "
                      << rename_error.message() << std::endl;
            std::remove(ready.temporary_mp3_file.c_str());
            mark_generation_failed(ready.generation);
        } else {
            completed_mp3 = ready.final_mp3_file;
            std::cerr << "[TTS] 保存: " << completed_mp3 << std::endl;
        }
    }

    // 重命名与回调之间也可能收到新的 STATE=1。
    if (ready.generation != g_generation.load()) {
        if (!completed_mp3.empty()) std::remove(completed_mp3.c_str());
        if (!ready.wav_file.empty()) std::remove(ready.wav_file.c_str());
        return;
    }

    if (!completed_mp3.empty()) {
        bool report_first_mp3 = false;
        std::chrono::steady_clock::time_point request_start;
        {
            std::lock_guard<std::mutex> start_lk(g_start_mutex);
            if (g_first_mp3_ready) {
                g_first_mp3_ready = false;
                report_first_mp3 = true;
                request_start = g_request_start;
            }
        }
        if (report_first_mp3) {
            const auto latency_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - request_start)
                    .count();
            std::cerr << "[首音延迟] 输入首帧→首段MP3完成: "
                      << latency_ms << " ms" << std::endl;
        }
    }

    if (callback && !completed_mp3.empty() &&
        ready.generation == g_generation.load()) {
        callback(ready.text, completed_mp3, ready.delivery_generation);
    }

    // UART 回调已经将完整 MP3 读入发送缓存；产品模式不长期保存。
    if (!DEBUG_SAVE_AUDIO && !completed_mp3.empty())
        std::remove(completed_mp3.c_str());

    if (TTS_PLAY_AUDIO && !ready.wav_file.empty() &&
        ready.generation == g_generation.load()) {
        {
            std::lock_guard<std::mutex> play_lk(g_play_mutex);
            g_play_queue.push({std::move(ready.wav_file)});
        }
        g_play_cv.notify_one();
    } else if (!ready.wav_file.empty()) {
        std::remove(ready.wav_file.c_str());
    }
}

// 并发合成可以乱序完成；回调和播放必须严格按 LLM 句子提交顺序触发。
// 失败结果也会占位，避免阻塞后续句子。慢速 UART 回调不持有结果锁。
void dispatch_segment_result(uint64_t sequence, SegmentResult result) {
    // 慢回调使用独立锁保持交付顺序；取消不需要这把锁。
    std::lock_guard<std::mutex> delivery_lk(g_delivery_mutex);
    std::vector<SegmentResult> ready_results;
    TtsSegmentReadyCallback callback;
    {
        std::lock_guard<std::mutex> result_lk(g_result_mutex);
        if (result.generation != g_generation.load() ||
            sequence < g_next_result_sequence) {
            remove_segment_files(result);
            return;
        }
        g_result_pending.emplace(sequence, std::move(result));
        const auto inserted = g_result_pending.find(sequence);
        if (inserted != g_result_pending.end() &&
            inserted->second.temporary_mp3_file.empty()) {
            g_failed_generations.insert(inserted->second.generation);
        }

        while (true) {
            auto it = g_result_pending.find(g_next_result_sequence);
            if (it == g_result_pending.end()) break;
            ready_results.push_back(std::move(it->second));
            g_result_pending.erase(it);
            ++g_next_result_sequence;
        }
        callback = g_segment_ready_callback;
    }

    for (auto& ready : ready_results)
        deliver_segment_result(std::move(ready), callback);
}

void finish_play() {
    { std::lock_guard<std::mutex> lk(g_play_mutex); g_play_done = true; }
    g_play_cv.notify_all();
}

// ============================================================================
// 并发下载控制
// ============================================================================

constexpr int MAX_CONCURRENT = 3;
std::mutex              g_dl_mutex;
std::condition_variable g_dl_cv;
int                     g_dl_active = 0;
int                     g_dl_pending = 0;
std::map<uint64_t, int> g_dl_pending_by_generation;

bool dl_acquire_slot(uint64_t generation) {
    std::unique_lock<std::mutex> lk(g_dl_mutex);
    g_dl_cv.wait(lk, [generation] {
        return generation != g_generation.load() ||
               g_dl_active < MAX_CONCURRENT;
    });
    if (generation != g_generation.load()) return false;
    ++g_dl_active;
    return true;
}

void dl_release_slot() {
    { std::lock_guard<std::mutex> lk(g_dl_mutex); --g_dl_active; }
    g_dl_cv.notify_one();
}

std::string shell_quote(const std::string& value) {
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') result += "'\\''";
        else result += c;
    }
    result += "'";
    return result;
}

bool close_tts_stream(const std::string& stream_id, bool log_failure) {
    std::ostringstream command;
    command << "curl -q --noproxy '*' "
            << "--fail --silent --show-error --max-time 5 -X POST "
            << shell_quote(std::string(TTS_URL) +
                           "/api/generate-stream/" + stream_id + "/close")
            << " -o /dev/null";
    const int status = system(command.str().c_str());
    if (status != 0 && log_failure) {
        std::cerr << "[TTS] 服务端流资源清理失败, ret="
                  << status << std::endl;
    }
    return status == 0;
}

bool convert_wav_to_mp3(const std::string& wav_path,
                        const std::string& output_path) {
    std::ostringstream cmd;
    cmd << "ffmpeg -y -loglevel error -i " << shell_quote(wav_path)
        << " -c:a libmp3lame -b:a 16k -ac 1 " << shell_quote(output_path)
        << " >/dev/null 2>&1";
    int ret = system(cmd.str().c_str());
    if (ret != 0) {
        std::cerr << "[TTS] MP3 转换失败, ret=" << ret << std::endl;
        return false;
    }
    return true;
}

/// 下载 PCM + 转 WAV
bool register_tts_stream(uint64_t generation,
                         const std::string& stream_id) {
    std::lock_guard<std::mutex> lk(g_stream_mutex);
    g_active_streams[generation].insert(stream_id);
    return true;
}

void unregister_tts_stream(uint64_t generation,
                           const std::string& stream_id) {
    std::lock_guard<std::mutex> lk(g_stream_mutex);
    auto it = g_active_streams.find(generation);
    if (it == g_active_streams.end()) return;
    it->second.erase(stream_id);
    if (it->second.empty()) g_active_streams.erase(it);
}

std::string moss_tts_download(const std::string& text,
                              uint64_t generation) {
    const auto request_id = g_next_request_id.fetch_add(1);
    std::error_code directory_error;
    std::filesystem::create_directories(
        std::string(PIPELINE_TEMP_DIR), directory_error);
    const std::string base = std::string(PIPELINE_TEMP_DIR) +
                           "/tts_moss_" + std::to_string(getpid()) + "_"
                           + std::to_string(request_id);
    const std::string text_path = base + ".txt";
    const std::string json_path = base + ".json";
    const std::string status_path = base + ".status.json";
    const std::string raw_path  = base + ".raw";
    const std::string wav_path  = base + ".wav";

    {
        std::ofstream text_file(text_path, std::ios::binary);
        if (!text_file) {
            std::cerr << "[TTS] 无法创建文本临时文件" << std::endl;
            return "";
        }
        text_file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    auto cleanup_temp = [&]() {
        std::remove(text_path.c_str());
        std::remove(json_path.c_str());
        std::remove(status_path.c_str());
        std::remove(raw_path.c_str());
    };

    // 字段与 test_app_onnx_server.py 保持一致。文本通过文件提交，
    // 避免引号、换行等内容被 shell 解释。
    std::ostringstream start_cmd;
    start_cmd << "curl -q --noproxy '*' "
              << "--fail-with-body --silent --show-error --max-time "
              << TTS_TIMEOUT_SEC << " "
              << "-F 'mode=voice_clone' "
              << "-F " << shell_quote("text=<" + text_path) << " "
              << "-F 'max_new_frames=375' "
              << "-F 'voice_clone_max_text_tokens=75' "
              << "-F " << shell_quote("seed=" + std::to_string(TTS_SEED)) << " "
              << "-F 'attn_implementation=fixed' ";
    if (!TTS_VOICE.empty()) {
        start_cmd << "-F "
                  << shell_quote("demo_id=" + std::string(TTS_VOICE)) << " ";
    } else {
        std::error_code prompt_error;
        if (!std::filesystem::is_regular_file(
                std::string(TTS_PROMPT_AUDIO), prompt_error)) {
            std::cerr << "[TTS] 参考音频不存在: "
                      << TTS_PROMPT_AUDIO << std::endl;
            cleanup_temp();
            return "";
        }
        start_cmd << "-F 'demo_id=' "
                  << "-F " << shell_quote("prompt_audio=@" +
                                          std::string(TTS_PROMPT_AUDIO) +
                                          ";type=audio/wav") << " ";
    }
    start_cmd << shell_quote(std::string(TTS_URL) +
                             "/api/generate-stream/start")
              << " -o " << shell_quote(json_path);

    int ret = system(start_cmd.str().c_str());
    std::remove(text_path.c_str());
    if (ret != 0) {
        std::cerr << "[TTS] 启动合成任务失败, ret=" << ret << std::endl;
        cleanup_temp();
        return "";
    }

    std::string stream_id;
    try {
        boost::property_tree::ptree root;
        boost::property_tree::read_json(json_path, root);
        stream_id = root.get<std::string>(
            "stream_id",
            root.get<std::string>("job_id", root.get<std::string>("id", "")));
    } catch (const std::exception& e) {
        std::cerr << "[TTS] 任务响应解析失败: " << e.what() << std::endl;
    }
    std::remove(json_path.c_str());
    if (stream_id.empty()) {
        std::cerr << "[TTS] 启动接口未返回任务 ID" << std::endl;
        cleanup_temp();
        return "";
    }
    if (generation != g_generation.load()) {
        // /start 已成功但还没有发起 /audio，此时可安全关闭作业。
        close_tts_stream(stream_id, false);
        cleanup_temp();
        return "";
    }
    register_tts_stream(generation, stream_id);

    std::ostringstream download_cmd;
    download_cmd << "curl -q --noproxy '*' "
                 << "--fail --silent --show-error -N --max-time "
                 << TTS_TIMEOUT_SEC << " "
                 << shell_quote(std::string(TTS_URL) +
                                "/api/generate-stream/" + stream_id + "/audio")
                 << " -o " << shell_quote(raw_path);
    ret = system(download_cmd.str().c_str());
    unregister_tts_stream(generation, stream_id);
    if (ret != 0) {
        if (generation == g_generation.load())
            std::cerr << "[TTS] PCM 音频流下载失败, ret=" << ret << std::endl;
        close_tts_stream(stream_id, generation == g_generation.load());
        cleanup_temp();
        return "";
    }
    if (generation != g_generation.load()) {
        close_tts_stream(stream_id, false);
        cleanup_temp();
        return "";
    }

    // StreamingResponse 在中途失败时仍可能以 HTTP 200 结束；
    // 必须另外检查作业状态，避免把部分 PCM 当成完整回答下发。
    std::ostringstream status_cmd;
    status_cmd << "curl -q --noproxy '*' "
               << "--fail --silent --show-error --max-time 10 "
               << shell_quote(std::string(TTS_URL) +
                              "/api/generate-stream/" + stream_id +
                              "/status")
               << " -o " << shell_quote(status_path);
    ret = system(status_cmd.str().c_str());
    bool stream_complete = false;
    if (ret == 0) {
        try {
            boost::property_tree::ptree status;
            boost::property_tree::read_json(status_path, status);
            stream_complete = status.get<std::string>("state", "") == "done" &&
                              !status.get<bool>("failed", false);
            if (!stream_complete) {
                std::cerr << "[TTS] 音频流未成功完成: "
                          << status.get<std::string>("error", "unknown")
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[TTS] 状态响应解析失败: " << e.what()
                      << std::endl;
        }
    }

    // 流已经结束后 /close 不再与 StreamingResponse 竞争，可安全清理
    // 服务端 job 元数据和最终 WAV，避免长期运行持续占盘。
    close_tts_stream(stream_id, true);

    if (generation != g_generation.load()) {
        cleanup_temp();
        return "";
    }
    if (!stream_complete) {
        cleanup_temp();
        return "";
    }

    std::error_code raw_error;
    const auto raw_size = std::filesystem::file_size(raw_path, raw_error);
    if (raw_error || raw_size == 0 || raw_size % sizeof(int16_t) != 0) {
        std::cerr << "[TTS] PCM 数据为空或长度非法" << std::endl;
        cleanup_temp();
        return "";
    }

    // 官方客户端声明输出为 pcm_s16le，而不是 f32le。
    std::ostringstream convert_cmd;
    convert_cmd << "ffmpeg -y -loglevel error -f s16le -ar " << TTS_SRC_RATE
                << " -ac " << TTS_SRC_CH
                << " -i " << shell_quote(raw_path)
                << " -ar " << SAMPLE_RATE << " -ac " << CHANNELS << " "
                << shell_quote(wav_path) << " >/dev/null 2>&1";
    ret = system(convert_cmd.str().c_str());
    cleanup_temp();
    if (ret != 0) {
        std::cerr << "[TTS] PCM 转 WAV 失败, ret=" << ret << std::endl;
        std::remove(wav_path.c_str());
        return "";
    }
    return wav_path;
}

}  // namespace

// ============================================================================
// 公开接口
// ============================================================================

void tts_init() {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(std::string(PIPELINE_TEMP_DIR)), ec);
    std::filesystem::create_directories(
        std::filesystem::path(std::string(TTS_OUTPUT_DIR)), ec);
    if (ec)
        std::cerr << "[TTS] 无法创建输出目录: " << ec.message() << std::endl;

    if (TTS_PLAY_AUDIO)
        g_play_thread = std::thread(play_loop);
}

void tts_set_start_time(std::chrono::steady_clock::time_point t) {
    tts_cancel_current();
    std::lock_guard<std::mutex> lk(g_start_mutex);
    g_request_start = t;
    g_first_audio   = true;
    g_first_mp3_ready = true;
    g_output_segment = 0;

    // 只清除当前进程上一轮的结果，不影响并行运行的其他实例。
    std::error_code ec;
    const std::filesystem::path output_dir{std::string(TTS_OUTPUT_DIR)};
    const std::string own_prefix =
        "result_p" + std::to_string(getpid()) + "_";
    for (std::filesystem::directory_iterator it(output_dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string name = it->path().filename().string();
        if (name.size() > own_prefix.size() + 4 &&
            name.rfind(own_prefix, 0) == 0 &&
            name.substr(name.size() - 4) == ".mp3")
            std::filesystem::remove(it->path(), ec);
    }
}

void tts_cancel_current() {
    const uint64_t old_generation = g_generation.fetch_add(1);
    size_t discarded_streams = 0;
    {
        std::lock_guard<std::mutex> lk(g_stream_mutex);
        auto it = g_active_streams.find(old_generation);
        if (it != g_active_streams.end()) {
            discarded_streams = it->second.size();
            g_active_streams.erase(it);
        }
    }
    // MOSS-TTS 的 /close 在音频 StreamingResponse 正在输出时可能阻塞
    // uvicorn 事件循环，继而让后续 /start 和 /health 全部卡死。
    // 因此这里只作废客户端结果，旧下载继续静默排空；绝不再调用 /close。

    {
        std::lock_guard<std::mutex> lk(g_result_mutex);
        for (auto& [sequence, result] : g_result_pending) {
            (void)sequence;
            remove_segment_files(result);
        }
        g_result_pending.clear();
        g_failed_generations.erase(old_generation);
        // 序号分配与此基线更新共用同一把锁，避免取消时
        // 旧代任务“偷走”新代的首个序号并让结果队列永久缺号。
        g_next_result_sequence = g_next_submit_sequence;
    }
    if (discarded_streams > 0)
        std::cerr << "[中断] 已作废 " << discarded_streams
                  << " 个旧 TTS 流（后台静默排空）" << std::endl;
    g_dl_cv.notify_all();
}

void tts_set_segment_ready_callback(TtsSegmentReadyCallback callback) {
    std::lock_guard<std::mutex> lk(g_result_mutex);
    g_segment_ready_callback = std::move(callback);
}

void tts_cleanup() {
    tts_wait_all();
    if (TTS_PLAY_AUDIO) {
        finish_play();
        if (g_play_thread.joinable()) g_play_thread.join();
    }
}

void tts_wait_all() {
    std::unique_lock<std::mutex> lk(g_dl_mutex);
    g_dl_cv.wait(lk, []{ return g_dl_pending == 0; });
}

bool tts_wait_current(uint64_t expected_generation) {
    const uint64_t generation = expected_generation == UINT64_MAX
        ? g_generation.load() : expected_generation;
    std::unique_lock<std::mutex> lk(g_dl_mutex);
    g_dl_cv.wait(lk, [generation] {
        if (generation != g_generation.load()) return true;
        auto it = g_dl_pending_by_generation.find(generation);
        return it == g_dl_pending_by_generation.end() || it->second == 0;
    });
    if (generation != g_generation.load()) return false;
    lk.unlock();
    std::lock_guard<std::mutex> result_lk(g_result_mutex);
    return g_failed_generations.find(generation) ==
           g_failed_generations.end();
}

uint64_t tts_current_generation() {
    return g_generation.load();
}

void tts_speak_async(const std::string& text,
                     uint64_t expected_generation,
                     uint64_t delivery_generation) {
    uint64_t generation = 0;
    uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> result_lk(g_result_mutex);
        generation = g_generation.load();
        if (expected_generation != UINT64_MAX &&
            expected_generation != generation)
            return;
        sequence = g_next_submit_sequence++;
    }
    uint64_t output_segment = 0;
    {
        std::lock_guard<std::mutex> lk(g_start_mutex);
        output_segment = ++g_output_segment;
    }
    // 每个代次使用独立文件名，旧代取消/删除不会覆盖新代文件。
    const std::string final_path =
        std::string(TTS_OUTPUT_DIR) + "/result_p" +
        std::to_string(getpid()) + "_g" + std::to_string(generation) + "_" +
        std::to_string(output_segment) + ".mp3";
    const std::string temporary_path =
        std::string(PIPELINE_TEMP_DIR) + "/tts_result_" +
        std::to_string(getpid()) + "_" +
        std::to_string(generation) + "_" + std::to_string(sequence) + ".mp3";
    {
        std::lock_guard<std::mutex> lk(g_dl_mutex);
        ++g_dl_pending;
        ++g_dl_pending_by_generation[generation];
    }
    std::thread([text, generation, delivery_generation, sequence,
                 final_path, temporary_path]() {
        // 已排队但尚未发起 HTTP 的旧代任务在取消时直接唤醒退出。
        if (!dl_acquire_slot(generation)) {
            dispatch_segment_result(sequence,
                {generation, delivery_generation, text, {}, final_path, {}});
            {
                std::lock_guard<std::mutex> lk(g_dl_mutex);
                --g_dl_pending;
                auto it = g_dl_pending_by_generation.find(generation);
                if (it != g_dl_pending_by_generation.end() &&
                    --it->second == 0)
                    g_dl_pending_by_generation.erase(it);
            }
            g_dl_cv.notify_all();
            return;
        }
        std::string wav = moss_tts_download(text, generation);
        std::string mp3;
        if (!wav.empty() && generation == g_generation.load()) {
            if (convert_wav_to_mp3(wav, temporary_path))
                mp3 = temporary_path;
            else
                std::remove(temporary_path.c_str());
        }
        dl_release_slot();
        dispatch_segment_result(sequence,
            {generation, delivery_generation, text, std::move(mp3),
             final_path, std::move(wav)});
        {
            std::lock_guard<std::mutex> lk(g_dl_mutex);
            --g_dl_pending;
            auto it = g_dl_pending_by_generation.find(generation);
            if (it != g_dl_pending_by_generation.end() && --it->second == 0)
                g_dl_pending_by_generation.erase(it);
        }
        g_dl_cv.notify_all();
    }).detach();
}

void tts_to_file(const std::string& text, const std::string& output_path) {
    const uint64_t generation = g_generation.load();
    std::string wav = moss_tts_download(text, generation);
    if (!wav.empty()) {
        convert_wav_to_mp3(wav, output_path);
        std::remove(wav.c_str());
    }
}
