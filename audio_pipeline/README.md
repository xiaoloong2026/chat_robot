# Audio Pipeline — 语音对话调度引擎

## 概述

audio_pipeline 是一个 C++ 实现的语音对话调度引擎，串联 ASR（语音识别）、LLM（大语言模型）、TTS（语音合成）三个 AI 服务，实现端到端的语音交互。

**运行平台**：NVIDIA Jetson AGX Orin (ARM aarch64, Ubuntu 22.04)

---

## 架构

```
┌─────────────────────────────────────────────────────┐
│                  audio_pipeline                      │
│                    (C++ 调度层)                       │
│                                                      │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐       │
│  │   ASR    │ →  │   LLM    │ →  │   TTS    │       │
│  │ :56999   │    │ :8000    │    │ :56789   │       │
│  └──────────┘    └──────────┘    └──────────┘       │
│       ↑                               │              │
│   麦克风/文件                      扬声器/MP3         │
└─────────────────────────────────────────────────────┘
```

### 三个服务

| 服务 | 端口 | 引擎 | 模型 | API |
|---|---|---|---|---|
| ASR | 56999 | SenseVoice.cpp + Python HTTP 包装 | sense-voice-small-q4_k | `POST /v1/audio/transcriptions` |
| LLM | 8000 | llama.cpp | Qwen3-4B-Q4_0 (4B参数, Q4量化) | `POST /v1/chat/completions` (OpenAI兼容) |
| TTS | 56789 | qwentts.cpp | Qwen-Talker-0.6B-customvoice-Q4_K_M | `POST /v1/audio/speech` (OpenAI兼容) |

---

## 运行模式

### 交互式对话
```bash
./audio_pipeline                # 文字输入
# 或输入 :v 触发语音输入
```

### 文件处理（音频 → ASR → LLM → TTS → 逐句 MP3）
```bash
./audio_pipeline -f input.opus -o result
# 输出: result_1.mp3, result_2.mp3, ...
```

### 单次文字
```bash
./audio_pipeline "你好，介绍一下你自己"
```

---

## 项目结构

```
audio_pipeline/
├── CMakeLists.txt              # CMake 构建 (C++17, Boost)
├── README.md
└── src/
    ├── config.h                # 全局配置常量
    ├── json_helper.h/cpp       # JSON 转义 + emoji/markdown 清洗
    ├── asr_client.h/cpp        # ASR 客户端 (curl → ASR API)
    ├── llm_client.h/cpp        # LLM 客户端 (SSE 流式 + 非流式)
    ├── tts_client.h/cpp        # TTS 客户端 (并发下载 + 串行播放)
    └── main.cpp                # 主入口：交互模式 / 文件模式 / 单发模式
```

---

## 数据流详解

### 交互模式流程

```
用户回车
  │
  ├─ 文字输入: 直接发送
  └─ :v 语音: arecord 录制 5s → OPUS → ASR API → 文本
  │
  ▼
┌──────────────────────────────────────────────┐
│              LLM 流式生成 (SSE)               │
│                                              │
│  curl -N POST :8000                          │
│  → setvbuf(_IONBF) 无缓冲                    │
│  → fgets 逐行读 SSE                          │
│  → 解析 data: {"delta":{"content":"..."}}   │
│  → on_delta 回调每个 token                   │
└──────────────────┬───────────────────────────┘
                   │ token 流
                   ▼
┌──────────────────────────────────────────────┐
│             SentenceSplitter                  │
│                                              │
│  前3句: 按 ， 。！？ \n\n 切分                │
│  第4句起: 按 。！？ \n\n 切分                 │
│  → 逐句入队 SentenceQueue                    │
└──────────────────┬───────────────────────────┘
                   │ 句子队列 (互斥锁+条件变量)
                   ▼
┌──────────────────────────────────────────────┐
│              TTS 消费线程                     │
│                                              │
│  while queue.pop():                          │
│    strip_markdown() → strip_emoji()          │
│    → tts_speak_async() 启动下载线程           │
│                                              │
│  并发控制: semaphore 最多 3 个同时下载        │
└──────────────────┬───────────────────────────┘
                   │ 下载线程 × N
                   ▼
┌──────────────────────────────────────────────┐
│         下载 → 播放队列 → 播放线程             │
│                                              │
│  curl PCM → 加 WAV 头 → enqueue              │
│  play_loop: fork() + aplay -D plughw:2,0    │
│  串行播放: waitpid 等上一个播完再播下一个      │
└──────────────────────────────────────────────┘
```

### 文件模式流程

```
音频文件 (opus/wav/...)
  │
  ├─ ffmpeg → 16000Hz mono WAV
  ▼
ASR API → 文本
  │
  ▼
LLM 流式 (SSE) → SentenceSplitter 切句
  │
  ├─ 句1 → strip → curl TTS → PCM → ffmpeg → result_1.mp3 → 后台播放
  ├─ 句2 → 等句1播完 → curl TTS → PCM → ffmpeg → result_2.mp3 → 后台播放
  └─ ...
  │
  ▼
统计输出: 首音延迟、每段耗时、总文件数
```

---

## 关键设计

### 句子切分策略

| 条件 | 分隔符 | 用途 |
|---|---|---|
| 前 3 句 | `，` `。` `！` `？` `\n\n` | 细粒度切分，快速出首音 |
| 第 4 句起 | `。` `！` `？` `\n\n` | 正常句子粒度 |

### 并发模型

```
LLM 主线程          TTS 消费线程         下载线程池 (MAX 3)
    │                    │                    │
    │  push sentence ──→ │ pop sentence        │
    │                    │ tts_speak_async ──→ │ dl_acquire_slot()
    │                    │                    │ curl download
    │  push sentence ──→ │ pop sentence        │ dl_release_slot()
    │                    │ tts_speak_async ──→ │ enqueue_play()
    │                    │                    │
    │ (LLM 结束)         │ (queue.close)       │
    │                    │                    ▼
    │                    │              播放线程 (串行)
    │                    │              aplay -D plughw:2,0
```

### Markdown/Emoji 清洗

LLM 输出常含 markdown 格式，直接送 TTS 会产生怪声：

| 输入 | 清洗后 |
|---|---|
| `**粗体文本**` | `粗体文本` |
| `### 标题` | `标题` |
| `1. 列表项` | `列表项` |
| `你好😊世界` | `你好世界` |

---

## 延迟分析

### 首音延迟定义

**LLM+TTS 首音延迟** = 用户输入（或音频识别完成）→ 第一个 MP3 生成完成 / 第一段音频开始播放

### 各环节耗时

| 环节 | 典型耗时 | 说明 |
|---|---|---|
| 音频转换 (opus→wav) | ~0.1s | ffmpeg 本地转码 |
| ASR 识别 | ~1-3s | SenseVoice small 模型，RTF 0.1-0.2 |
| LLM 首 token | ~0.1-0.3s | SSE 流式输出，延迟低 |
| 首句形成 | ~0.1-0.5s | 遇首个 `，` 或 `。` 即切句 |
| TTS 下载合成 | **~5-15s** | **最大瓶颈** |
| 播放启动 | <0.1s | fork + aplay |

### 延迟根因

**TTS 服务端是主要瓶颈**，两个原因叠加：

#### 1. 模型推理慢
qwen-talker-0.6B 在 Jetson AGX Orin 上实时倍速约 0.05x-0.1x。
生成 1 秒音频需要 10-20 秒计算。

#### 2. Chunk 缓冲策略（关键）
`qwentts.cpp` 源码 `pipeline-tts.cpp:541`：
```cpp
const float chunk_sec = params->codec_chunk_sec > 0.0f ? params->codec_chunk_sec : 24.0f;
//                                                                               ^^^^
//                                                                    默认 24 秒积攒阈值
```

服务端的 codec 流式解码器内置 24 秒音频积攒阈值。生成的 PCM 帧先填内部缓冲区，
攒满 24 秒才通过 `on_chunk` 回调输出一次 HTTP chunk。短文本（< 24s 音频）永远
无法触发，表现为全部生成完后一次性发回。

**影响**：
- 短对话（"你好" ~0.5s）：全生成完才收到 → TTFB = 全部推理时间
- 长文本（> 24s）：每生成 24s 吐一块，但因 Jetson 推理太慢，实际走不到这分支

**修复**：`tts-server.cpp` 中设置：
```cpp
p.codec_chunk_sec = 2.0f;  // 替代默认 24s，每 2s 吐一块
```

---

## 性能数据 (Jetson AGX Orin)

| 指标 | 数值 |
|---|---|
| LLM 生成速度 | ~30 tokens/s |
| TTS 生成倍速 | 0.05x-0.1x 实时 |
| ASR RTF | 0.1-0.2 |
| 首音延迟 (短句) | ~10-15s |
| 首音延迟 (长句) | ~15-25s |

---

## 构建与部署

### 依赖

- C++17 (g++ 11+)
- CMake 3.16+
- Boost (头文件，property_tree)
- libasound2-dev
- curl, ffmpeg (运行时)

### 编译

```bash
cd audio_pipeline
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 部署服务

三个服务需要在开发板上分别启动：

```bash
# 1. LLM (zx 用户)
cd /home/zx/workspace/llama_cpp/llama.cpp/build/bin
./llama-server --model /home/zx/workspace/model/Qwen3-4B-Q4_0.gguf \
  --n-gpu-layers 999 --ctx-size 4096 --batch-size 512 \
  --host 0.0.0.0 --port 8000 &

# 2. TTS (xhw 用户)
cd /home/xhw/build
./tts-server \
  --model /home/xhw/models/qwen-talker-0.6b-customvoice-Q4_K_M.gguf \
  --codec /home/xhw/models/qwen-tokenizer-12hz-Q4_K_M.gguf \
  --host 0.0.0.0 --port 56789 &

# 3. ASR (yu 用户)
python3 /home/yu/workspace/asr_server.py &
```

### 测试密钥

| 服务 | 健康检查 |
|---|---|
| ASR | `curl http://127.0.0.1:56999/health` |
| LLM | `curl http://127.0.0.1:8000/health` |
| TTS | `curl http://127.0.0.1:56789/health` |

---

## 配置参数

```cpp
// config.h
LLM_URL           = "http://127.0.0.1:8000/v1/chat/completions"
LLM_MAX_TOKENS    = 256
LLM_TEMPERATURE   = 0.7

TTS_URL           = "http://127.0.0.1:56789/v1/audio/speech"
TTS_VOICE         = "vivian"       // 可选: eric, dylan, uncle_fu, serena...
TTS_SEED          = 42             // 服务端不支持，预留

ASR_URL           = "http://127.0.0.1:56999/v1/audio/transcriptions"

AUDIO_DEVICE      = "plughw:2,0"   // ReSpeaker 4 Mic Array (USB)
MIC_SAMPLE_RATE   = 16000          // SenseVoice 要求 16kHz
SAMPLE_RATE       = 24000          // TTS 输出 24kHz
```

---

## 已知问题与改进方向

| 问题 | 原因 | 改进方向 |
|---|---|---|
| TTS 非流式 | codec_chunk_sec 默认 24s | 修改 qwentts.cpp 源码，设置 2s chunk |
| TTS seed 无效 | tts-server.cpp 未解析 seed 字段 | 修改 tts-server.cpp 解析 seed |
| TTS 音色不一致 | 同上，seed 无效导致每次随机 | 同上 |
| 首音延迟大 | TTS 推理慢 + 非流式 | 服务端改 chunk 大小 + 客户端真流式接收 |
| ASR 不支持流式 | 需录完整句再识别 | 接入流式 ASR (SenseVoice streaming mode) |
| NVIDIA GPU 日志 | Jetson 板载日志，无害 | 无法彻底消掉 |
