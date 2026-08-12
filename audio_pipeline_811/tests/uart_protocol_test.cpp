#include "parser.hpp"
#include "uart.hpp"
#include "uart_protocol.hpp"
#include "uart_sender.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

template <typename Exception, typename Function>
void expect_throw(Function&& function, const std::string& message) {
    try {
        function();
        expect(false, message);
    } catch (const Exception&) {
    } catch (...) {
        expect(false, message + "（异常类型错误）");
    }
}

void write_fixed_text(std::vector<uint8_t>& code,
                      size_t offset,
                      size_t width,
                      const std::string& text) {
    expect(text.size() < width, "定长文本必须留出 NUL");
    std::copy(text.begin(), text.end(), code.begin() + offset);
}

std::vector<uint8_t> make_handshake(uint8_t key_index) {
    std::vector<uint8_t> code(48, 0);
    const uint8_t key = key_index == 0 ? 199 : 218;
    const uint8_t plain[] = {0x00, 0x01, 0xFE, 0xFF};
    for (size_t i = 0; i < 4; ++i) {
        code[i] = plain[i];
        code[4 + i] = uart_protocol::encrypt_handshake_byte(plain[i], key);
    }
    write_fixed_text(code, 8, 12, "113.12345");
    write_fixed_text(code, 20, 12, "23.12345");
    write_fixed_text(code, 32, 16, "123456789012345");
    return code;
}

void test_frame_codec() {
    const std::vector<uint8_t> payload{0x10, 0x20, 0x30};
    const auto frame = uart_protocol::encode_frame(
        uart_protocol::TYPE_TEXT, 0xFE, 0x1234,
        uart_protocol::TEXT_STATE_ANSWER,
        payload.data(), payload.size());

    expect(frame.size() == payload.size() + uart_protocol::FRAME_OVERHEAD,
           "帧总长应为 CODE+10");
    expect(frame[0] == 0xAA && frame[1] == 0x55, "同步头");
    expect(frame[2] == uart_protocol::TYPE_TEXT, "TYPE");
    expect(frame[3] == 0xFE, "SEQ");
    expect(frame[4] == 0x34 && frame[5] == 0x12, "DSID 小端");
    expect(frame[6] == uart_protocol::TEXT_STATE_ANSWER, "STATE");
    expect(frame[7] == 3 && frame[8] == 0, "LEN 小端");
    expect(frame.back() == uart_protocol::checksum(frame.data() + 2,
                                                    frame.size() - 3),
           "CHECK 从 TYPE 累加到 CODE");

    std::vector<uint8_t> max_code(uart_protocol::MAX_CODE_SIZE, 0x5A);
    const auto max_frame = uart_protocol::encode_frame(
        uart_protocol::TYPE_TTS_AUDIO, 0, 1, 0,
        max_code.data(), max_code.size());
    expect(max_frame.size() == uart_protocol::MAX_FRAME_SIZE,
           "最大帧必须正好 1024 字节");

    expect_throw<std::invalid_argument>([&] {
        uart_protocol::encode_frame(uart_protocol::TYPE_TEXT, 0, 0, 0,
                                    payload.data(), payload.size());
    }, "拒绝 DSID=0");
    expect_throw<std::length_error>([&] {
        std::vector<uint8_t> too_large(uart_protocol::MAX_CODE_SIZE + 1);
        uart_protocol::encode_frame(uart_protocol::TYPE_TEXT, 0, 1, 0,
                                    too_large.data(), too_large.size());
    }, "拒绝超过 1014 字节的 CODE");

    const auto empty_frame = uart_protocol::encode_frame(
        uart_protocol::TYPE_AI_INFO, 0, 1, 0, nullptr, 0);
    expect(empty_frame.size() == uart_protocol::FRAME_OVERHEAD,
           "允许 LEN=0 的 10 字节帧");
}

void test_handshake() {
    for (uint8_t key_index : {uint8_t{0}, uint8_t{1}}) {
        auto code = make_handshake(key_index);
        const auto result = uart_protocol::parse_handshake(code, key_index);
        expect(result.ok, "两把协议密钥均应通过握手");
        expect(result.info.imei == "123456789012345", "解析 IMEI");
        expect(result.info.longitude == "113.12345", "解析经度");
        expect(result.info.latitude == "23.12345", "解析纬度");
    }

    auto bad = make_handshake(0);
    bad[4] ^= 0x01;
    expect(!uart_protocol::parse_handshake(bad, 0).ok,
           "拒绝不匹配的握手密文");
    expect(!uart_protocol::parse_handshake(std::vector<uint8_t>(47), 0).ok,
           "拒绝非 48 字节握手");
    expect(!uart_protocol::parse_handshake(make_handshake(0), 2).ok,
           "拒绝文档附录 mod3 产生的非法 STATE=2");
}

void test_text_accumulator() {
    uart_protocol::TextQuestionAccumulator accumulator;
    const std::vector<uint8_t> a{'A'};
    const std::vector<uint8_t> b{'B'};
    const std::vector<uint8_t> c{'C'};

    expect(accumulator.feed(a, uart_protocol::QUESTION_STATE_WAKE, 1).status ==
               uart_protocol::TextQuestionAccumulator::Status::Accepted,
           "STATE=1 开始文本提问");
    expect(accumulator.feed(b, uart_protocol::QUESTION_STATE_STREAMING, 2).status ==
               uart_protocol::TextQuestionAccumulator::Status::Accepted,
           "STATE=2 追加文本提问");
    const auto complete =
        accumulator.feed(c, uart_protocol::QUESTION_STATE_END, 3);
    expect(complete.status ==
               uart_protocol::TextQuestionAccumulator::Status::Complete &&
               complete.text == "ABC",
           "STATE=3 只提交一次完整文本");

    accumulator.feed(a, uart_protocol::QUESTION_STATE_WAKE, 1);
    expect(accumulator.feed(c, uart_protocol::QUESTION_STATE_END, 3).status ==
               uart_protocol::TextQuestionAccumulator::Status::Error,
           "拒绝跳号 DSID");
    expect(accumulator.feed(a, uart_protocol::QUESTION_STATE_WAKE, 0).status ==
               uart_protocol::TextQuestionAccumulator::Status::Error,
           "文本状态机拒绝 DSID=0");

    expect(accumulator.feed({}, uart_protocol::QUESTION_STATE_NULL, 1).status ==
               uart_protocol::TextQuestionAccumulator::Status::Accepted,
           "安全忽略 STATE=0 空 NULL 帧");
    expect(accumulator.feed(a, uart_protocol::QUESTION_STATE_NULL, 1).status ==
               uart_protocol::TextQuestionAccumulator::Status::Error,
           "STATE=0 NULL 帧不得承载 CODE");

    const std::vector<uint8_t> utf8_first{0xE4};
    const std::vector<uint8_t> utf8_rest{0xBD, 0xA0};  // “你”跨帧
    accumulator.feed(utf8_first, uart_protocol::QUESTION_STATE_WAKE, 1);
    const auto utf8_complete = accumulator.feed(
        utf8_rest, uart_protocol::QUESTION_STATE_END, 2);
    expect(utf8_complete.status ==
               uart_protocol::TextQuestionAccumulator::Status::Complete,
           "UTF-8 码点可以跨分片，在整句完成时验证");

    const std::vector<uint8_t> invalid_utf8{0xED, 0xA0, 0x80};
    accumulator.feed(invalid_utf8,
                     uart_protocol::QUESTION_STATE_WAKE, 1);
    expect(accumulator.feed({}, uart_protocol::QUESTION_STATE_END, 2).status ==
               uart_protocol::TextQuestionAccumulator::Status::Error,
           "拒绝 UTF-8 代理项编码");
}

std::vector<DataFrame> parse_bytes(const std::vector<uint8_t>& bytes,
                                   bool byte_by_byte) {
    UART uart("/dev/null", B230400);
    RingBuffer ring;
    Parser parser(ring, uart);
    std::vector<DataFrame> frames;
    parser.set_frame_callback(
        [&](const DataFrame& frame) { frames.push_back(frame); });
    if (byte_by_byte) {
        for (uint8_t byte : bytes) {
            ring.write(&byte, 1);
            parser.process();
        }
    } else {
        ring.write(bytes.data(), bytes.size());
        parser.process();
    }
    return frames;
}

void test_parser_streaming_and_sequence() {
    const uint8_t one = 1;
    auto first = uart_protocol::encode_frame(
        uart_protocol::TYPE_AI_STATE, 254, 1, 0, &one, 1);
    auto second = uart_protocol::encode_frame(
        uart_protocol::TYPE_AI_STATE, 255, 1, 0, &one, 1);
    auto third = uart_protocol::encode_frame(
        uart_protocol::TYPE_AI_STATE, 0, 1, 0, &one, 1);

    std::vector<uint8_t> stream{0x00, 0xAA, 0x00, 0x42};
    stream.insert(stream.end(), first.begin(), first.end());
    stream.insert(stream.end(), second.begin(), second.end());
    stream.insert(stream.end(), third.begin(), third.end());
    const auto frames = parse_bytes(stream, true);
    expect(frames.size() == 3, "噪声和逐字节输入后仍解析三帧");
    expect(frames.size() >= 3 && frames[0].sequence_ok &&
               frames[1].sequence_ok && frames[2].sequence_ok,
           "SEQ 254→255→0 连续");

    auto gap = uart_protocol::encode_frame(
        uart_protocol::TYPE_AI_STATE, 2, 1, 0, &one, 1);
    std::vector<uint8_t> gap_stream = first;
    gap_stream.insert(gap_stream.end(), gap.begin(), gap.end());
    const auto gap_frames = parse_bytes(gap_stream, false);
    expect(gap_frames.size() == 2 && !gap_frames[1].sequence_ok &&
               gap_frames[1].expected_seq == 255,
           "检测 SEQ 跳号");

    first.back() ^= 0x01;
    const auto bad_checksum = parse_bytes(first, false);
    expect(bad_checksum.size() == 1 && !bad_checksum[0].checksum_ok,
           "报告 CHECK 错误帧");

    // 超长 LEN 的伪帧之后仍应重新同步到下一合法帧。
    std::vector<uint8_t> oversized{
        0xAA, 0x55, uart_protocol::TYPE_TEXT_QUESTION, 1,
        1, 0, 1,
        static_cast<uint8_t>((uart_protocol::MAX_CODE_SIZE + 1) & 0xFF),
        static_cast<uint8_t>((uart_protocol::MAX_CODE_SIZE + 1) >> 8)};
    oversized.insert(oversized.end(), second.begin(), second.end());
    const auto recovered = parse_bytes(oversized, false);
    expect(recovered.size() == 1 && recovered[0].seq == 255,
           "非法 LEN 后恢复同步");

    // CHECK 正确但密钥未经上层验证的握手，不得重置 SEQ。
    auto seq10 = uart_protocol::encode_frame(
        uart_protocol::TYPE_AI_STATE, 10, 1, 0, &one, 1);
    const auto handshake_code = make_handshake(0);
    auto unchecked_handshake = uart_protocol::encode_frame(
        uart_protocol::TYPE_HANDSHAKE, 200, 1, 0,
        handshake_code.data(), handshake_code.size());
    auto seq11 = uart_protocol::encode_frame(
        uart_protocol::TYPE_AI_STATE, 11, 1, 0, &one, 1);
    std::vector<uint8_t> handshake_stream = seq10;
    handshake_stream.insert(handshake_stream.end(),
                            unchecked_handshake.begin(),
                            unchecked_handshake.end());
    handshake_stream.insert(
        handshake_stream.end(), seq11.begin(), seq11.end());
    const auto handshake_frames = parse_bytes(handshake_stream, false);
    expect(handshake_frames.size() == 3 &&
               handshake_frames[2].sequence_ok,
           "未被上层接受的握手不改写 SEQ 基线");
}

std::vector<uint8_t> read_pipe_bytes(int fd) {
    const int old_flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, old_flags | O_NONBLOCK);
    std::vector<uint8_t> bytes;
    uint8_t buffer[4096];
    while (true) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            bytes.insert(bytes.end(), buffer, buffer + count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        break;
    }
    return bytes;
}

void test_sender_states_and_slicing() {
    int descriptors[2];
    expect(pipe(descriptors) == 0, "创建发送测试管道");
    uart_sender_set_fd(descriptors[1]);

    expect(uart_send_question_text("question"),
           "0x13 提问文本写入成功");

    char template_path[] = "/tmp/uart_protocol_mp3_XXXXXX";
    const int file = mkstemp(template_path);
    expect(file >= 0, "创建临时 MP3");
    std::vector<uint8_t> mp3(1500, 0x55);
    if (file >= 0) {
        const ssize_t written = write(file, mp3.data(), mp3.size());
        expect(written == static_cast<ssize_t>(mp3.size()), "写入临时 MP3");
        close(file);
    }

    uart_sender_set_input_start(std::chrono::steady_clock::now());
    const uint64_t response_generation =
        uart_sender_current_generation();
    expect(uart_send_segment(
               "answer", template_path, response_generation),
           "当前代次的 TTS 分段发送成功");
    expect(uart_finish_response(), "有音频时成功发送整轮末片");
    expect(!uart_finish_response(), "没有待发送音频时拒绝伪造结束帧");

    const auto bytes = read_pipe_bytes(descriptors[0]);

    uart_sender_cancel_current();
    uart_sender_set_input_start(std::chrono::steady_clock::now());
    expect(!uart_send_segment(
               "stale", template_path, response_generation),
           "取消后拒绝迟到的旧 TTS 代次");
    expect(read_pipe_bytes(descriptors[0]).empty(),
           "迟到的旧 TTS 不得输出任何 UART 帧");

    expect(!uart_send_segment(
               "missing", "/tmp/uart_protocol_missing_file.mp3",
               uart_sender_current_generation()),
           "MP3 文件失败向上传播");
    expect(!uart_send_segment(
               "must-not-leak", template_path,
               uart_sender_current_generation()),
           "一个分段失败后拒绝同轮后续分段");
    expect(read_pipe_bytes(descriptors[0]).empty(),
           "失败轮次的后续分段不得泄漏到 UART");
    expect(!uart_finish_response(),
           "任一 TTS 分段失败时不得伪造成功末片");

    uart_sender_set_fd(-1);
    close(descriptors[0]);
    close(descriptors[1]);
    unlink(template_path);

    const auto frames = parse_bytes(bytes, false);
    expect(frames.size() == 4, "问题文本、回答文本和两片 MP3 共四帧");
    if (frames.size() == 4) {
        expect(frames[0].type == uart_protocol::TYPE_TEXT &&
                   frames[0].status == uart_protocol::TEXT_STATE_QUESTION,
               "0x13 提问文本 STATE=0");
        expect(frames[1].type == uart_protocol::TYPE_TEXT &&
                   frames[1].status == uart_protocol::TEXT_STATE_ANSWER,
               "0x13 回答文本 STATE=1");
        expect(frames[2].type == uart_protocol::TYPE_TTS_AUDIO &&
                   frames[2].status == uart_protocol::AUDIO_STATE_NORMAL &&
                   frames[2].slice == 1 &&
                   frames[2].data.size() == uart_protocol::MAX_CODE_SIZE,
               "0x11 普通分片 STATE=0、DSID=1、CODE=1014");
        expect(frames[3].type == uart_protocol::TYPE_TTS_AUDIO &&
                   frames[3].status == uart_protocol::AUDIO_STATE_END &&
                   frames[3].slice == 2 && frames[3].data.size() == 486,
               "0x11 最后带数据分片 STATE=1、DSID连续");
        expect(std::all_of(frames.begin(), frames.end(),
                           [](const DataFrame& frame) {
                               return frame.total_len <=
                                      uart_protocol::MAX_FRAME_SIZE;
                           }),
               "所有下行帧不超过 1024 字节");
    }
}

}  // namespace

int main() {
    test_frame_codec();
    test_handshake();
    test_text_accumulator();
    test_parser_streaming_and_sequence();
    test_sender_states_and_slicing();
    if (failures != 0) {
        std::cerr << failures << " protocol test(s) failed" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "all UART protocol tests passed" << std::endl;
    return EXIT_SUCCESS;
}
