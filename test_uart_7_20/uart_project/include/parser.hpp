#ifndef PARSER_HPP
#define PARSER_HPP

#include "ring_buffer.hpp"

#include <vector>
#include <cstdint>

class UART;  // 前向声明

// ============================================================
//  帧头结构（AA 55 之后紧接 7 字节头）
//  AA 55 | type(1B) | seq(1B) | slice(2B LE) | status(1B) | data_len(2B LE)
//  之后 data_len 字节数据 + 最后 1 字节校验
//  帧总长 = 2(同步) + 1+1+2+1+2(头) + data_len + 1(校验) = 10 + data_len
// ============================================================
static constexpr size_t FRAME_MIN_SIZE  = 10;   // 同步字 + 头 + 校验（data_len=0）
static constexpr size_t FRAME_HEAD_SIZE = 7;    // AA 55 之后的 7 字节: type+seq+slice+status+len
static constexpr uint8_t SYNC_BYTE0 = 0xAA;
static constexpr uint8_t SYNC_BYTE1 = 0x55;

struct DataFrame
{
    // ---- 帧头字段 ----
    uint8_t  type;         // 帧类型
    uint8_t  seq;          // 帧序号
    uint16_t slice;        // 数据切片序号 (LE)
    uint8_t  status;       // 状态字
    uint16_t data_len;     // 数据字节长度 (LE)

    // ---- 数据 ----
    std::vector<uint8_t> data;   // 数据载荷 (data_len 字节)

    // ---- 校验 ----
    uint8_t  checksum_rx;        // 接收到的校验字节
    uint8_t  checksum_calc;      // 本地计算的校验值
    bool     checksum_ok;        // 校验是否通过

    // ---- 元信息 ----
    size_t   total_len;          // 帧总长度
    uint8_t  raw_sync[2];       // AA 55
};

// ============================================================
//  协议解析器
// ============================================================
class Parser
{
public:
    Parser(RingBuffer& rb, UART& uart);

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

    // ---- 成员 ----
    RingBuffer& rb_;
    UART&       uart_;

    std::vector<uint8_t> rx_buf_;     // 环形缓冲区读出暂存
    std::vector<uint8_t> frame_buf_;  // 帧组装缓冲区

    // 统计
    uint32_t    frame_count_{0};
    uint32_t    checksum_errors_{0};
};

#endif
