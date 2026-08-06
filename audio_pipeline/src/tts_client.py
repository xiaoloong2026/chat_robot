#!/usr/bin/env python3
"""TTS 客户端：调用 MOSS-TTS-Nano 服务，下载 PCM 并转 WAV/MP3"""
import sys, os, requests, shutil

TTS_URL = "http://127.0.0.1:18083"
PROMPT  = "/home/yu/workspace/prompt_zh.wav"

def tts_synthesize(text, output_path, prompt_audio=PROMPT):
    """text → TTS → output WAV"""
    # 1. 启动任务
    with open(prompt_audio, "rb") as f:
        resp = requests.post(
            f"{TTS_URL}/api/generate-stream/start",
            data={
                "mode": "voice_clone",
                "text": text,
                "max_new_tokens": "750",
                "seed": "1234",
                "codec_chunk_frames": "8",
            },
            files={"prompt_audio": (os.path.basename(prompt_audio), f, "audio/wav")},
            timeout=(30, 1800),
        )
    resp.raise_for_status()
    stream_id = resp.json()["stream_id"]

    # 2. 下载 PCM 流
    tmp_raw = output_path + ".raw"
    with requests.get(
        f"{TTS_URL}/api/generate-stream/{stream_id}/audio",
        stream=True, timeout=(30, 1800),
    ) as r:
        r.raise_for_status()
        with open(tmp_raw, "wb") as f:
            shutil.copyfileobj(r.raw, f)

    # 3. ffmpeg 转 WAV: 48kHz stereo f32 → 24kHz mono s16
    os.system(
        f"ffmpeg -y -f f32le -ar 48000 -ac 2 -i {tmp_raw}"
        f" -ar 24000 -ac 1 {output_path} 2>/dev/null"
    )
    os.remove(tmp_raw)
    return output_path

if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--file":
        # 文件模式: python3 tts_client.py --file output.wav < text.txt
        text = sys.stdin.read()
        tts_synthesize(text, sys.argv[2])
        print(f"OK:{sys.argv[2]}")
    elif len(sys.argv) >= 2:
        # 命令行模式: python3 tts_client.py "text" [output.wav]
        out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/tts_out.wav"
        tts_synthesize(sys.argv[1], out)
        print(f"OK:{out}")
    else:
        print("用法: python3 tts_client.py <text> [output.wav]", file=sys.stderr)
        sys.exit(1)
