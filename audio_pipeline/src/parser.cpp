#include "parser.hpp"
#include "uart.hpp"

#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdio>

// ============================================================
//  构造
// ============================================================

Parser::Parser(RingBuffer& rb, UART& uart)
    : rb_(rb), uart_(uart)
{
    rx_buf_.reserve(RingBuffer::SIZE);
    frame_buf_.reserve(RingBuffer::SIZE * 2);
}

// ============================================================
//  主入口：从环形缓冲区读取 → 同步 → 组帧 → 解析
// ============================================================

void Parser::process()
{
    // 1. 读走环形缓冲区中全部数据
    size_t n = rb_.read_all(rx_buf_);
    if (n == 0) return;

    // 2. 追加到帧组装缓冲区
    frame_buf_.insert(frame_buf_.end(), rx_buf_.data(), rx_buf_.data() + n);

    // 3. 循环提取完整帧
    size_t consumed_total = 0;
    while (consumed_total < frame_buf_.size())
    {
        size_t consumed = 0;
        if (try_extract_frame(frame_buf_.data() + consumed_total,
                              frame_buf_.size() - consumed_total,
                              consumed))
        {
            consumed_total += consumed;
        }
        else
        {
            break;  // 数据不足，等待下次
        }
    }

    // 4. 移除已消费的数据
    if (consumed_total > 0)
    {
        frame_buf_.erase(frame_buf_.begin(),
                         frame_buf_.begin() + consumed_total);
    }

    // 5. 防止无限增长（无同步字时丢弃旧数据）
    if (frame_buf_.size() > RingBuffer::SIZE * 2)
    {
        // 打印前 20 字节，排查收到的到底是什么
        std::cerr << "[WARN] frame_buf overflow " << frame_buf_.size() << "B, first bytes: ";
        for (size_t i = 0; i < 20 && i < frame_buf_.size(); ++i)
            fprintf(stderr, "%02X ", frame_buf_[i]);
        std::cerr << std::endl;
        frame_buf_.clear();
    }
}

// ============================================================
//  尝试提取一帧
//  协议格式：
//    [0-1]  AA 55          同步字
//    [2]    type           帧类型
//    [3]    seq            帧序号
//    [4-5]  slice          数据切片序号 (uint16 LE)
//    [6]    status         状态字
//    [7-8]  data_len       数据字节长度 (uint16 LE)
//    [9..]  data           数据载荷 (data_len 字节)
//    [last] checksum       校验位
//  帧总长 = 10 + data_len
// ============================================================

bool Parser::try_extract_frame(const uint8_t* data, size_t len,
                                size_t& consumed)
{
    consumed = 0;

    // ---- 搜索 AA 55 同步字 ----
    const uint8_t* p = data;
    const uint8_t* end = data + len;

    while (p < end && *p != SYNC_BYTE0) { p++; }

    if (p >= end)
    {
        consumed = len;  // 无同步字，全部丢弃
        return false;
    }

    // 同步字前的杂散数据
    size_t skip = p - data;
    if (skip > 0)
    {
        std::cout << "[SYNC] dropped " << skip
                  << " stray bytes before AA 55" << std::endl;
    }

    // ---- 至少需要 AA 55 + 7字节头 = 9 字节才够读 data_len ----
    size_t remain = len - skip;
    if (remain < FRAME_MIN_SIZE)
    {
        consumed = skip;  // 保留从 AA 开始的数据
        return false;
    }

    // 检查第二个同步字节
    if (p[1] != SYNC_BYTE1)
    {
        consumed = skip + 1;  // 假同步，跳过这个 AA
        return false;
    }

    // ---- 解析帧头，获取 data_len ----
    DataFrame frame;
    bool header_ok = parse_header(p + 2, frame);  // p+2 = AA 55 之后
    if (!header_ok)
    {
        consumed = skip + 2;  // 头解析异常，跳过
        return false;
    }

    // 假同步过滤：TYPE 必须是协议已知值
    // (0x01-0x04 上行, 0x11-0x14 下行)
    if (frame.type < 0x01 || (frame.type > 0x04 && frame.type < 0x11) || frame.type > 0x14)
    {
        std::cerr << "[DBG] false AA55, TYPE=0x" << std::hex << (int)frame.type << std::dec
                  << " at offset ~" << (data - (const uint8_t*)nullptr) << std::endl;
        consumed = skip + 1;  // 假 AA 55，跳过第一个字节继续搜
        return false;
    }

    // 帧总长 = 2(AA55) + 8(头) + data_len + 1(校验) = 10 + data_len
    size_t frame_total = FRAME_MIN_SIZE + frame.data_len;

    // 检查数据是否够
    if (remain < frame_total)
    {
        consumed = skip;  // 不够一帧，等下次
        return false;
    }

    // ---- 提取数据载荷 ----
    const uint8_t* data_start = p + 2 + FRAME_HEAD_SIZE;  // p[10]
    frame.data.assign(data_start, data_start + frame.data_len);

    // ---- 读取接收到的校验字节 ----
    frame.checksum_rx = data_start[frame.data_len];  // 数据末尾后一字节

    // ---- 本地计算校验 ----
    frame.checksum_calc = calc_checksum(frame);
    frame.checksum_ok    = (frame.checksum_calc == frame.checksum_rx);

    // ---- 保存头部原始字节 ----
    frame.total_len = frame_total;
    frame.raw_sync[0] = SYNC_BYTE0;
    frame.raw_sync[1] = SYNC_BYTE1;

    // ---- 消费整帧 ----
    consumed = skip + frame_total;
    frame_count_++;

    if (!frame.checksum_ok)
        checksum_errors_++;

    // ---- 回调完整帧数据 ----
    if (on_frame_) on_frame_(frame);

    return true;
}

// ============================================================
//  解析 8 字节帧头（AA 55 之后）
//    [0] type     [1] seq     [2-3] slice LE
//    [4] status   [5-6] data_len LE
// ============================================================

bool Parser::parse_header(const uint8_t* hdr, DataFrame& frame)
{
    frame.type     = hdr[0];
    frame.seq      = hdr[1];
    frame.slice    = hdr[2] | (hdr[3] << 8);
    frame.status   = hdr[4];
    frame.data_len = hdr[5] | (hdr[6] << 8);

    // 合理性检查：data_len 不应过大
    if (frame.data_len > 2048)
    {
        std::cerr << "[ERR] data_len too large: " << frame.data_len
                  << ", header may be corrupted" << std::endl;
        return false;
    }

    return true;
}

// ============================================================
//  计算校验和：从 type 位（第3字节）到 data 末尾逐字节相加，取低 8 位
//  即：type + seq + slice_lo + slice_hi + status + len_lo + len_hi
//      + data[0] + data[1] + ... + data[data_len-1]
//  结果 & 0xFF
// ============================================================

uint8_t Parser::calc_checksum(const DataFrame& frame) const
{
    uint32_t sum = 0;

    // 帧头（type 到 len_hi，共 7 字节）
    sum += frame.type;
    sum += frame.seq;
    sum += (frame.slice & 0xFF);         // slice 低字节
    sum += ((frame.slice >> 8) & 0xFF);  // slice 高字节
    sum += frame.status;
    sum += (frame.data_len & 0xFF);      // len 低字节
    sum += ((frame.data_len >> 8) & 0xFF); // len 高字节

    // 数据字节
    for (auto b : frame.data)
        sum += b;

    return static_cast<uint8_t>(sum & 0xFF);
}

// ============================================================
//  打印解析结果
// ============================================================

void Parser::print_frame(const DataFrame& frame)
{
    const char* ck_mark = frame.checksum_ok ? "OK" : "FAIL";

    std::cout << "\n========================================" << std::endl;
    std::cout << "Frame #" << frame_count_
              << "  total=" << frame.total_len << "B"
              << "  data=" << frame.data_len << "B"
              << "  checksum=" << ck_mark << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    // 帧头
    std::cout << "  Type     : 0x" << std::hex << std::setfill('0')
              << std::setw(2) << (int)frame.type << std::dec << std::endl;
    std::cout << "  Seq      : " << (int)frame.seq << std::endl;
    std::cout << "  Slice    : " << frame.slice
              << " (0x" << std::hex << std::setw(4) << frame.slice
              << ")" << std::dec << std::endl;
    std::cout << "  Status   : 0x" << std::hex << std::setfill('0')
              << std::setw(2) << (int)frame.status << std::dec << std::endl;
    std::cout << "  Data Len : " << frame.data_len
              << " (0x" << std::hex << std::setw(4) << frame.data_len
              << ")" << std::dec << std::endl;

    // 校验
    std::cout << "  Checksum : rx=0x" << std::hex << std::setfill('0')
              << std::setw(2) << (int)frame.checksum_rx
              << " calc=0x" << std::setw(2) << (int)frame.checksum_calc
              << std::dec;
    if (!frame.checksum_ok)
        std::cout << " *** MISMATCH ***";
    std::cout << std::endl;

    // 数据 hex 全部打印
    std::cout << "  Data [" << frame.data_len << "B]:" << std::endl;
    for (size_t i = 0; i < frame.data_len; ++i)
    {
        if ((i % 32) == 0)
            std::cout << "    ";
        printf("%02X ", frame.data[i]);
        if ((i + 1) % 32 == 0)
            std::cout << std::endl;
    }
    if (frame.data_len % 32 != 0)
        std::cout << std::endl;

    std::cout << "========================================" << std::endl;
}
