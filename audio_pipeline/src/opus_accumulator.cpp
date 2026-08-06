#include "opus_accumulator.h"
#include "config.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

// ============================================================================
// libopus 动态加载（免编译时依赖 libopus-dev）
// ============================================================================

namespace {

class OpusDecoderDyn {
public:
    OpusDecoderDyn(int sampleRate, int channels)
        : sampleRate_(sampleRate), channels_(channels) {
        library_ = dlopen("libopus.so.0", RTLD_NOW);
        if (!library_) library_ = dlopen("libopus.so", RTLD_NOW);
        if (!library_) throw std::runtime_error("找不到 libopus");

        create_   = load<CreateFn>("opus_decoder_create");
        decode_   = load<DecodeFn>("opus_decode");
        destroy_  = load<DestroyFn>("opus_decoder_destroy");

        int error = 0;
        decoder_ = create_(sampleRate_, channels_, &error);
        if (!decoder_ || error != 0)
            throw std::runtime_error("opus_decoder_create 失败");
    }

    ~OpusDecoderDyn() {
        if (decoder_ && destroy_) destroy_(decoder_);
        if (library_) dlclose(library_);
    }

    std::vector<int16_t> decodePacket(const uint8_t* data, int size) {
        const int maxSamples = sampleRate_ * 120 / 1000;  // max 120ms
        std::vector<int16_t> pcm(maxSamples * channels_);
        int samples = decode_(decoder_, data, size, pcm.data(), maxSamples, 0);
        if (samples < 0) throw std::runtime_error("opus_decode 失败");
        pcm.resize(samples * channels_);
        return pcm;
    }

private:
    using CreateFn  = void* (*)(int, int, int*);
    using DecodeFn  = int (*)(void*, const uint8_t*, int32_t, int16_t*, int, int);
    using DestroyFn = void (*)(void*);

    template <typename T>
    T load(const char* name) {
        dlerror();
        void* sym = dlsym(library_, name);
        const char* err = dlerror();
        if (err) throw std::runtime_error(std::string("dlsym: ") + err);
        T fn = nullptr;
        std::memcpy(&fn, &sym, sizeof(fn));
        return fn;
    }

    void* library_ = nullptr;
    void* decoder_ = nullptr;
    CreateFn  create_  = nullptr;
    DecodeFn  decode_  = nullptr;
    DestroyFn destroy_ = nullptr;
    int sampleRate_;
    int channels_;
};

// WAV 写入
void writeLe16(std::ostream& out, uint16_t v) {
    char b[] = {char(v & 0xff), char((v >> 8) & 0xff)};
    out.write(b, 2);
}
void writeLe32(std::ostream& out, uint32_t v) {
    char b[] = {char(v & 0xff), char((v >> 8) & 0xff),
                char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
    out.write(b, 4);
}

void writeWav(const std::string& path, const std::vector<int16_t>& pcm,
              uint32_t sampleRate, uint16_t channels) {
    uint32_t dataSize = pcm.size() * sizeof(int16_t);
    std::ofstream out(path, std::ios::binary);
    if (!out) return;

    out.write("RIFF", 4);
    writeLe32(out, 36 + dataSize);
    out.write("WAVEfmt ", 8);
    writeLe32(out, 16);
    writeLe16(out, 1);  // PCM
    writeLe16(out, channels);
    writeLe32(out, sampleRate);
    writeLe32(out, sampleRate * channels * 2);
    writeLe16(out, channels * 2);
    writeLe16(out, 16);
    out.write("data", 4);
    writeLe32(out, dataSize);
    for (auto s : pcm) writeLe16(out, uint16_t(s));
}

}  // namespace

// ============================================================================
// OpusAccumulator
// ============================================================================

OpusAccumulator::OpusAccumulator() {
    opus_buf_.reserve(256 * 1024);
}

bool OpusAccumulator::feed(const std::vector<uint8_t>& data,
                            uint8_t  state,
                            uint16_t dsid,
                            uint8_t  seq) {
    if (state == 1) reset();
    opus_buf_.insert(opus_buf_.end(), data.begin(), data.end());
    segment_count_++;
    last_dsid_ = dsid;
    last_seq_  = seq;

    // STATE=3 (问话结束) 或 DSID=0 → 解码
    if (state == 3 || dsid == 0) {
        if (!opus_buf_.empty()) {
            const auto completed_segments = segment_count_;
            const auto completed_bytes = opus_buf_.size();
            std::string wav = decode_opus(opus_buf_);
            opus_buf_.clear();
            segment_count_ = 0;
            last_dsid_ = 0xFFFF;

            if (!wav.empty() && on_decode_) {
                std::ostringstream info;
                info << "segments=" << completed_segments
                     << " bytes=" << completed_bytes
                     << " state=" << (int)state;
                on_decode_(wav, info.str());
            }
            return true;
        }
    }

    return false;
}

void OpusAccumulator::reset() {
    opus_buf_.clear();
    segment_count_ = 0;
    last_dsid_ = 0xFFFF;
    last_seq_ = 0xFF;
}

std::string OpusAccumulator::flush() {
    if (opus_buf_.empty()) return "";
    std::string wav = decode_opus(opus_buf_);
    opus_buf_.clear();
    segment_count_ = 0;
    return wav;
}

std::string OpusAccumulator::decode_opus(const std::vector<uint8_t>& opus_data) {
    static int file_seq = 0;
    ++file_seq;

    const std::string base_dir = DEBUG_SAVE_AUDIO
        ? std::string(DEBUG_AUDIO_DIR) : std::string(PIPELINE_TEMP_DIR);
    std::error_code directory_error;
    std::filesystem::create_directories(base_dir, directory_error);
    const std::string unique = std::to_string(getpid()) + "_" +
                               std::to_string(file_seq);
    const std::string opus_file = base_dir + "/uart_opus_" + unique + ".opus";
    const std::string wav_file  = base_dir + "/uart_wav_" + unique + ".wav";

    // 产品模式不落盘保存 OPUS，调试模式才保留。
    if (DEBUG_SAVE_AUDIO) {
        FILE* f = fopen(opus_file.c_str(), "wb");
        if (f) { fwrite(opus_data.data(), 1, opus_data.size(), f); fclose(f); }
    }

    const int OPUS_PACKET_SIZE = 80;   // 和 parse_ventilator_audio.cpp 一致
    const int SAMPLE_RATE      = 16000;

    try {
        OpusDecoderDyn decoder(SAMPLE_RATE, 1);
        std::vector<int16_t> allPcm;

        if (opus_data.size() % OPUS_PACKET_SIZE != 0) {
            std::cerr << "[OPUS] 数据 " << opus_data.size()
                      << " 不能被 " << OPUS_PACKET_SIZE << " 整除" << std::endl;
            return "";
        }

        for (size_t pos = 0; pos < opus_data.size(); pos += OPUS_PACKET_SIZE) {
            auto pcm = decoder.decodePacket(opus_data.data() + pos,
                                            OPUS_PACKET_SIZE);
            allPcm.insert(allPcm.end(), pcm.begin(), pcm.end());
        }

        writeWav(wav_file, allPcm, SAMPLE_RATE, 1);

        double duration = (double)allPcm.size() / SAMPLE_RATE;
        std::cerr << "[OPUS] " << opus_data.size() / OPUS_PACKET_SIZE
                  << " 包, " << duration << "s → " << wav_file << std::endl;
        return wav_file;

    } catch (const std::exception& e) {
        std::cerr << "[OPUS] 解码失败: " << e.what()
                  << (DEBUG_SAVE_AUDIO ? ", file=" + opus_file : "")
                  << std::endl;
        if (!DEBUG_SAVE_AUDIO) std::remove(wav_file.c_str());
        return "";
    }
}
