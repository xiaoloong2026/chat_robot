#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

// ============================================================================
// TTS 客户端 — 并发下载 + 串行播放
// ============================================================================

void tts_init();

/// 设置对话开始时间，用于计算首音延迟
void tts_set_start_time(std::chrono::steady_clock::time_point t);

/// 每段 MP3 生成完成后的回调；按原始句子顺序触发。
/// delivery_generation 由调用方传入，用于拒绝取消后迟到的 UART 音频。
using TtsSegmentReadyCallback = std::function<void(
    const std::string& text,
    const std::string& mp3_path,
    uint64_t delivery_generation)>;
void tts_set_segment_ready_callback(TtsSegmentReadyCallback callback);

/// 取消当前轮次：作废未完成结果和回调。
/// 旧的服务端流可能仍在后台排空，但不会再交付给 UART。
void tts_cancel_current();

void tts_cleanup();
void tts_speak_async(const std::string& text,
                     uint64_t expected_generation = UINT64_MAX,
                     uint64_t delivery_generation = UINT64_MAX);
uint64_t tts_current_generation();
void tts_wait_all();
/// 等待指定轮次；被取消或任一分段失败时返回 false。
bool tts_wait_current(uint64_t expected_generation = UINT64_MAX);

/// 合成语音保存为 MP3 文件（不走扬声器）
void tts_to_file(const std::string& text, const std::string& output_path);
