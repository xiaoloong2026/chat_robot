#!/usr/bin/env python3
"""和 test_app_onnx_server.py 完全一致的 TTS 调用方式"""
import sys, requests, shutil, os

TTS    = "http://127.0.0.1:18083"
PROMPT = "/home/yu/workspace/prompt_zh.wav"
TEXT   = sys.argv[1] if len(sys.argv) > 1 else "你好，测试。"
OUT    = sys.argv[2] if len(sys.argv) > 2 else "/tmp/tts_py.wav"

# ====== 和测试脚本一模一样的请求 ======
with open(PROMPT, "rb") as pf:
    r = requests.post(f"{TTS}/api/generate-stream/start",
        data={
            "mode": "voice_clone",
            "language": "",
            "text": TEXT,
            "prompt_text": "",
            "max_new_tokens": "750",
            "codec_chunk_frames": "8",
            "seed": "1234",
            "attn_implementation": "fixed",
            "demo_id": "",
        },
        files={"prompt_audio": (os.path.basename(PROMPT), pf, "audio/wav")},
        timeout=(30, 1800))
r.raise_for_status()
sid = r.json()["stream_id"]

# ====== 下载 PCM ======
tmp = OUT + ".raw"
with requests.get(f"{TTS}/api/generate-stream/{sid}/audio",
                  stream=True, timeout=(30, 1800)) as ar:
    ar.raise_for_status()
    with open(tmp, "wb") as f:
        shutil.copyfileobj(ar.raw, f)

# ====== ffmpeg 转 WAV ======
os.system(f"ffmpeg -y -f f32le -ar 48000 -ac 2 -i {tmp}"
          f" -ar 24000 -ac 1 {OUT} 2>/dev/null")
os.remove(tmp)
print(f"OK: {OUT}")
