#ifndef PARSER_HPP
#define PARSER_HPP

#include "ring_buffer.hpp"
#include "uart_protocol.hpp"

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

class UART;  // 前向声明

// ============================================================
//  帧头结构（AA 55 之后紧接 7 字节头）
//  AA 55 | type(1B) | seq(1B) | slice(2B LE) | status(1B) | data_len(2B LE)
//  之后 data_len 字节数据 + 最后 1 字节校验
//  帧总长 = 2(同步) + 1+1+2+1+2(头) + data_len + 1(校验) = 10 + data_len
// ============================================================
static constexpr size_t FRAME_MIN_SIZE = uart_protocol::FRAME_OVERHEAD;
static constexpr size_t FRAME_HEAD_SIZE = 7;
static constexpr uint8_t SYNC_BYTE0 = uart_protocol::SYNC0;
static constexpr uint8_t SYNC_BYTE1 = uart_protocol::SYNC1;

struct DataFrame
{
    // ---- 帧头字段 ----
    uint8_t  type{};         // 帧类型
    uint8_t  seq{};          // 帧序号
    uint16_t slice{};        // 数据切片序号 (LE)
    uint8_t  status{};       // 状态字
    uint16_t data_len{};     // 数据字节长度 (LE)

    // ---- 数据 ----
    std::vector<uint8_t> data;   // 数据载荷 (data_len 字节)

    // ---- 校验 ----
    uint8_t  checksum_rx{};        // 接收到的校验字节
    uint8_t  checksum_calc{};      // 本地计算的校验值
    bool     checksum_ok{};        // 校验是否通过

    // ---- 完整性 ----
    bool     dsid_ok{};            // 新协议 DSID 必须为 1..65535
    bool     sequence_ok{true};     // 与上一有效上行帧的 SEQ 是否连续
    uint8_t  expected_seq{};       // sequence_ok=false 时的期望值

    // ---- 元信息 ----
    size_t   total_len{};          // 帧总长度
    uint8_t  raw_sync[2]{};        // AA 55
};

// ============================================================
//  协议解析器
// ============================================================
class Parser
{
public:
    /// 每解析出一个完整帧，调此回调传入完整 DataFrame
    using FrameCallback = std::function<void(const DataFrame& frame)>;

    Parser(RingBuffer& rb, UART& uart);

    void set_frame_callback(FrameCallback cb) { on_frame_ = std::move(cb); }

    /// 只有上层完整验证通过 0xAA 握手后才重建 SEQ 基线。
    void accept_sequence_baseline(uint8_t seq) {
        last_seq_ = seq;
        has_last_seq_ = true;
    }

    // 主入口：main 循环每轮调用一次
    void process();

private:
    // 尝试从字节流提取一帧，返回 true 表示成功解析
    bool try_extract_frame(const uint8_t* data, size_t len,
                           size_t& consumed);

    // 解析帧头（7 字节 → 填入 DataFrame 字段）
    bool parse_header(const uint8_t* header, DataFrame& frame);

    // 计算校验和：从 type 位到 data 末尾累加，取低 8 位
    uint8_t calc_checksum(const DataFrame& frame) const;

    // 打印解析结果
    void print_frame(const DataFrame& frame);

    // ---- 回调 ----
    FrameCallback on_frame_;

    // ---- 成员 ----
    RingBuffer& rb_;
    UART&       uart_;

    std::vector<uint8_t> rx_buf_;     // 环形缓冲区读出暂存
    std::vector<uint8_t> frame_buf_;  // 帧组装缓冲区

    // 统计
    uint32_t    frame_count_{0};
    uint32_t    checksum_errors_{0};
    uint32_t    sequence_errors_{0};
    bool        has_last_seq_{false};
    uint8_t     last_seq_{0};
};

#endif
