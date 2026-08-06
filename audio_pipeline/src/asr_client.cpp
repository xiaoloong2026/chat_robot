#include "asr_client.h"

#include "config.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

std::string asr_transcribe(const std::string& wav_path) {
    // curl POST 音频文件到 ASR 服务
    std::ostringstream cmd;
    cmd << "curl --noproxy '*' -sS --connect-timeout 3 --max-time 30 "
        << "--data-binary @\"" << wav_path << "\" "
        << "\"" << ASR_URL << "\" ";

    std::cerr << "[ASR] 发送音频..." << std::flush;

    std::string response;
    std::array<char, 4096> buf{};
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe)
        throw std::runtime_error("无法启动 ASR curl");

    while (fgets(buf.data(), buf.size(), pipe))
        response += buf.data();
    const int curl_status = pclose(pipe);

    if (response.empty())
        throw std::runtime_error(
            "ASR 服务无响应，curl status=" + std::to_string(curl_status));

    // 解析 JSON: {"text": "..."}
    std::istringstream iss(response);
    boost::property_tree::ptree root;
    boost::property_tree::read_json(iss, root);
    if (auto error = root.get_optional<std::string>("error"))
        throw std::runtime_error("ASR 服务错误: " + *error);
    std::string text = root.get<std::string>("text", "");

    if (text.empty())
        std::cerr << " [识别结果为空]" << std::endl;
    else
        std::cerr << " " << text << std::endl;
    return text;
}
