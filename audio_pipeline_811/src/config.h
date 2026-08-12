#pragma once

#include <string_view>
#include <termios.h>

// ============================================================================
// 产品/调试模式
// ============================================================================
// 产品默认不保留用户音频；排障时改为 true 后重新编译。
inline constexpr bool DEBUG_SAVE_AUDIO = false;
inline constexpr std::string_view PIPELINE_TEMP_DIR{"/tmp/audio-pipeline"};
inline constexpr std::string_view DEBUG_AUDIO_DIR{
    "/home/dev/project/audio_debug"};

// ============================================================================
// LLM 服务配置
// ============================================================================
inline constexpr std::string_view LLM_URL{"http://127.0.0.1:8080/v1/chat/completions"};
inline constexpr std::string_view LLM_MODEL{"Qwen3.5-4B"};
inline constexpr int    LLM_MAX_TOKENS   = 256;
inline constexpr double LLM_TEMPERATURE  = 0.7;
inline constexpr double LLM_REPEAT_PENALTY = 1.1;
inline constexpr std::string_view LLM_SYSTEM_PROMPT{
    "你是呼吸机设备的语音助手。"
    "当用户要求打开、关闭、启动、停止、调节或设置设备功能时，"
    "只输出一句简短的中文完成确认，不解释、不扩展、不自我介绍，"
    "不使用Markdown，不添加表情。"
    "确认句采用“好的，已为您……”格式，并准确复述动作和对象。"
    "例如：用户说“打开呼吸机”，你必须只回答“好的，已为您打开呼吸机。”；"
    "用户说“关闭呼吸机”，你必须只回答“好的，已为您关闭呼吸机。”；"
    "用户说“把管路温度设为24度”，你必须只回答"
    "“好的，已为您将管路温度设置为24度。”。"
    "对于非设备控制问题，直接、简洁地正常回答。"
};

// ============================================================================
// TTS 服务配置
// ============================================================================
inline constexpr std::string_view TTS_URL{"http://127.0.0.1:18083"};        // MOSS-TTS-Nano
inline constexpr std::string_view TTS_VOICE{""};                       // 内置音色
// TTS_VOICE 为空时才使用参考音频进行音色克隆。
inline constexpr std::string_view TTS_PROMPT_AUDIO{
    "/home/dev/project/TTS/workspace/MOSS-TTS-Nano/assets/audio/zh_1.wav"};
inline constexpr int TTS_SEED = 1234;

// ============================================================================
// 音频播放配置
// ============================================================================
// 只进行 TTS 合成，不通过开发板本机声卡播放。
inline constexpr bool TTS_PLAY_AUDIO = false;
inline constexpr std::string_view TTS_OUTPUT_DIR{
    DEBUG_SAVE_AUDIO ? "/home/dev/project/tts_output"
                     : "/tmp/audio-pipeline/tts_output"};

// ALSA 设备名，plughw 支持自动重采样
inline constexpr std::string_view AUDIO_DEVICE{"plughw:2,0"};

// PCM 格式参数（qwentts.cpp 输出: S16LE, 24kHz, mono）
inline constexpr unsigned int SAMPLE_RATE  = 24000;
inline constexpr int          CHANNELS     = 1;
inline constexpr int          SAMPLE_WIDTH = 2;  // 16-bit

// ============================================================================
// ASR 服务配置
// ============================================================================
inline constexpr std::string_view ASR_URL{"http://127.0.0.1:56999/v1/audio/transcriptions"};
inline constexpr std::string_view MIC_DEVICE{"plughw:2,0"};      // ReSpeaker 麦克风
inline constexpr int MIC_SAMPLE_RATE = 16000;                     // SenseVoice 要求 16kHz

// ============================================================================
// UART 串口配置
// ============================================================================
inline constexpr std::string_view UART_DEVICE{"/dev/ttyTHS1"};
inline constexpr speed_t        UART_BAUD{B230400};
inline constexpr int            UART_BAUD_RATE = 230400;
inline constexpr std::string_view AI_VERSION{"V1.0"};

// ============================================================================
// 超时配置
// ============================================================================
inline constexpr int CURL_TIMEOUT_SEC = 120;   // HTTP 超时
inline constexpr int TTS_TIMEOUT_SEC  = 300;   // TTS 合成超时（长文本）
