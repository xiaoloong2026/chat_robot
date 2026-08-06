#pragma once

#include <string>

/** 调用 ASR 服务，返回识别文本 */
std::string asr_transcribe(const std::string& wav_path);
