# Audio Pipeline

`audio_pipeline` 是运行在 NVIDIA Jetson AGX Orin 上的 C++17 调度程序，将
ASR、LLM、TTS 与呼吸机 UART 协议串联起来。

UART 已按 2026-08-07 版协议实现为 230400/8N1，并包含握手、TYPE 路由、
CHECK/SEQ/DSID 完整性检查、问话分片和打断代次。精确实施口径与待确认项见
[PROTOCOL_20260807.md](PROTOCOL_20260807.md)。

## 当前服务契约

| 服务 | 地址 | 当前引擎 | 调用方式 |
|---|---|---|---|
| ASR | `127.0.0.1:56999` | SenseVoice.cpp + Python HTTP 包装 | `POST /v1/audio/transcriptions`，请求体为原始音频文件 |
| LLM | `127.0.0.1:8080` | llama.cpp / Qwen3.5-4B | `POST /v1/chat/completions`，SSE 流式返回 |
| TTS | `127.0.0.1:18083` | MOSS-TTS-Nano ONNX | `/api/generate-stream/start` 启动，再从 `/audio` 读 PCM |

TTS 客户端默认使用 `demo-1` 内置音色，输出转换为 16 kbps、单声道 MP3
后按 `0x11` 下发。若将 `TTS_VOICE` 设为空，才会使用
`TTS_PROMPT_AUDIO` 进行参考音频克隆。

## UART 数据流

```text
MCU 0xAA 握手
  ↓ 验证通过
MCU 0x01 音频提问 或 0x02 文本提问
  ↓ STATE=1/2/3 累积，仅 STATE=3 提交
ASR（仅音频） → AI 0x13/STATE=0 回传问题
  ↓
LLM SSE → 按句切分 → TTS
  ↓
每段：AI 0x13/STATE=1 文本 → AI 0x11 MP3
  ↓
整轮最后一个 0x11 带数据分片使用 STATE=1
  ↓
若识别为设备控制，再发 AI 0x12 JSON
```

`0x03` 设备信息和 `0x04` 睡眠数据只校验/保存，不会被当成 LLM 指令。
`0x05` 用于休眠、启动或节能状态。新的 `STATE=1` 会作废旧代结果，
迟到的 TTS 文件和回调不得混入新会话。

## 构建与测试

```bash
cd /home/duan/move_project
cmake -S audio_pipeline -B audio_pipeline/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build audio_pipeline/build --parallel
ctest --test-dir audio_pipeline/build --output-on-failure
```

主程序：

```bash
# UART 模式
./audio_pipeline/build/audio_pipeline -u

# 只测 UART 上行 Opus 解码
./audio_pipeline/build/audio_pipeline -u --test-opus

# 交互式文本模式
./audio_pipeline/build/audio_pipeline

# 单次文本
./audio_pipeline/build/audio_pipeline "你好"
```

UART 模式需要访问 `/dev/ttyTHS1`。建议通过用户组/udev 规则授权，不要将整个
调度程序长期以 root 运行。

## 服务启动与检查

根目录 [readme.md](../readme.md) 保留了本机 LLM/TTS 完整启动命令。ASR HTTP
包装当前位于 `/home/duan/workspace/asr_server.py`；顶层 README 中单次运行
`sense-voice-main` 的命令不会启动 56999 端口。

启动后检查：

```bash
sudo ss -ltnp | grep -E ':(56999|8080|18083) '

curl -q --noproxy '*' -sS http://127.0.0.1:56999/health
curl -q --noproxy '*' -sS http://127.0.0.1:8080/health
curl -q --noproxy '*' -sS http://127.0.0.1:18083/health
```

本地回环请求都显式使用 `--noproxy '*'`，避免环境代理导致“服务在监听但
curl 无输出”等难以复现的现象。

## 配置

主要常量在 [src/config.h](src/config.h)：

- 服务 URL 与超时；
- UART 设备和 230400 波特率；
- TTS 内置音色/参考音频；
- 音频设备；
- `DEBUG_SAVE_AUDIO`。

`DEBUG_SAVE_AUDIO` 产品默认为 `false`，用户问话和生成音频只存放在
`/tmp/audio-pipeline` 并按流程清理。排障时才应开启保留，并配置访问权限与
保留周期。

注意：CMake 只编译 `audio_pipeline/src/` 下的实现。目录根部的同名旧
`opus_accumulator.*` 和 `*.bak_bargein` 仅为历史文件，不参与当前构建，
协议修改不应再写入这些文件。

## 当前限制

- 协议未定义 `0x12` 控制执行 ACK，所以只能在播报后下发，不能证明 MCU
  已执行成功。
- `0x03/0x04` 没有多帧结束语义，目前要求单帧且只保存内存中的最新值。
- 上行音频的 codec/采样率/包边界在 DOCX 中未写全；当前延用 Opus 16 kHz、
  80 字节包，需 MCU 固件确认。
- 打断能保证旧结果不下发，但 ASR/LLM/TTS 服务端未实现可抢占取消，
  已经开始的旧推理可能仍会占用资源。已进入 TTS `/audio` 的旧流最长可等待
  `TTS_TIMEOUT_SEC=300` 秒；其结果会被丢弃，但期间可能拖慢新问题。
