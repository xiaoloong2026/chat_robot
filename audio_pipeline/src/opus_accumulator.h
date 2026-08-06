#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
// OPUS 音频帧累积器
//
// TYPE 0x01 帧：OPUS 16Kbps mono，按 DSID 切片
// STATE=1 唤醒生效, STATE=2 问话中, STATE=3 问话结束
// DSID=0 表示切片结束
// ============================================================================

class OpusAccumulator {
public:
    /// 每累积完一段音频（STATE=3 或 DSID=0），解码后回调 WAV 文件路径
    using DecodeCallback = std::function<void(const std::string& wav_path,
                                               const std::string& log_info)>;

    OpusAccumulator();

    /// 设置解码完成回调
    void set_decode_callback(DecodeCallback cb) { on_decode_ = std::move(cb); }

    /// 喂入一帧 TYPE 0x01 的数据
    /// @return 是否触发了累积完成和解码
    bool feed(const std::vector<uint8_t>& data,
              uint8_t  state,
              uint16_t dsid,
              uint8_t  seq);

    /// 手动刷新（强制解码当前累积数据）
    std::string flush();

    /// 丢弃上一轮尚未结束的 OPUS 数据。
    void reset();

private:
    std::string decode_opus(const std::vector<uint8_t>& opus_data);

    std::vector<uint8_t> opus_buf_;   // OPUS 数据累积
    uint16_t    last_dsid_ = 0xFFFF;
    uint8_t     last_seq_  = 0xFF;
    int         segment_count_ = 0;

    DecodeCallback on_decode_;
};
