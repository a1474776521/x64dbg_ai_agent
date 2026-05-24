// tools/probe_reasoning.cpp
//
// M-1 复核：DeepSeek reasoning_content 回传方向。
//
// 项目当前实现（src/ai/deepseek_chat_client.cpp:163-166）：上一轮 assistant 的
// reasoning_content 原样回传给服务端。注释声称"不回传会 HTTP 400"。
// 本工具独立验证该假设，分两组实测对比：
//   组 A: 回传 reasoning_content
//   组 B: 剥离 reasoning_content（仅保留 content）
// 模型固定 deepseek-reasoner，stream=false 便于解析。
//
// 依赖：cpr + nlohmann::json + Crypt32（DPAPI）。
// 不依赖项目 src/ 任何模块，独立可编译。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Shell32.lib")

namespace fs = std::filesystem;
using nlohmann::json;

// ---- DPAPI 解密 deepseek_api_key.bin ----
static std::optional<std::string> loadApiKeyDpapi() {
    PWSTR appdataW = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdataW))) {
        std::fprintf(stderr, "SHGetKnownFolderPath failed\n");
        return std::nullopt;
    }
    fs::path p = fs::path(appdataW) / L"x64dbg-ai-plugin" / L"secrets" / L"deepseek_api_key.bin";
    CoTaskMemFree(appdataW);

    if (!fs::exists(p)) {
        std::fprintf(stderr, "secret file not found: %ls\n", p.c_str());
        return std::nullopt;
    }
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    auto sz = ifs.tellg();
    if (sz <= 0) return std::nullopt;
    std::vector<BYTE> enc(static_cast<size_t>(sz));
    ifs.seekg(0);
    ifs.read(reinterpret_cast<char*>(enc.data()), sz);

    DATA_BLOB in{}, out{};
    in.cbData = static_cast<DWORD>(enc.size());
    in.pbData = enc.data();
    LPWSTR descr = nullptr;
    if (!CryptUnprotectData(&in, &descr, nullptr, nullptr, nullptr, 0, &out)) {
        std::fprintf(stderr, "CryptUnprotectData failed: 0x%lx\n", GetLastError());
        return std::nullopt;
    }
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    if (descr) LocalFree(descr);
    LocalFree(out.pbData);
    return plain;
}

// ---- 一轮真实请求 ----
struct RoundResult {
    long   status   = 0;
    bool   ok       = false;
    std::string errMsg;     // HTTP 错误时的服务端报文（截断）
    std::string content;    // 200 时模型答复 content
    std::string reasoning;  // 200 时 reasoning_content
};

static RoundResult callDeepseek(const std::string& apiKey,
                                const json& messages,
                                const std::string& tag) {
    json body = {
        {"model",       "deepseek-reasoner"},
        {"messages",    messages},
        {"stream",      false},
        {"temperature", 0.2},
        {"max_tokens",  256},
    };

    cpr::Response r = cpr::Post(
        cpr::Url{"https://api.deepseek.com/chat/completions"},
        cpr::Header{
            {"Authorization", "Bearer " + apiKey},
            {"Content-Type",  "application/json"},
            {"Accept",        "application/json"},
            {"User-Agent",    "x64dbg-ai-plugin-probe/1.0"},
        },
        cpr::Body{body.dump()},
        cpr::Timeout{120000});

    RoundResult res;
    res.status = r.status_code;
    std::printf("[%s] HTTP %ld (cpr_error=%s)\n", tag.c_str(),
                r.status_code, r.error ? r.error.message.c_str() : "(none)");

    if (r.status_code != 200) {
        res.errMsg = r.text.substr(0, 800);
        std::printf("[%s] body(<=800): %s\n", tag.c_str(), res.errMsg.c_str());
        return res;
    }
    try {
        auto j = json::parse(r.text);
        auto& m = j["choices"][0]["message"];
        if (m.contains("content") && m["content"].is_string())
            res.content = m["content"].get<std::string>();
        if (m.contains("reasoning_content") && m["reasoning_content"].is_string())
            res.reasoning = m["reasoning_content"].get<std::string>();
        res.ok = true;
        std::printf("[%s] ok content(%zu) reasoning(%zu)\n",
                    tag.c_str(), res.content.size(), res.reasoning.size());
    } catch (const std::exception& e) {
        std::printf("[%s] parse error: %s\n", tag.c_str(), e.what());
        res.errMsg = r.text.substr(0, 800);
    }
    return res;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    auto key = loadApiKeyDpapi();
    if (!key || key->empty()) {
        std::fprintf(stderr, "ERROR: DeepSeek API Key 未配置或 DPAPI 解密失败。\n"
                             "请先在 x64dbg 插件 UI 里登录一次。\n");
        return 1;
    }
    std::printf("API key loaded (%zu chars, sk-***%s)\n\n",
                key->size(),
                key->size() > 4 ? key->substr(key->size() - 4).c_str() : "");

    // ---- 第一轮：让模型产生 reasoning_content ----
    json firstMessages = json::array({
        {{"role", "system"}, {"content",
            "You are a math assistant. Answer briefly."}},
        {{"role", "user"}, {"content",
            "What is 17 * 23? Reply with only the number."}},
    });
    auto round1 = callDeepseek(*key, firstMessages, "ROUND1");
    if (!round1.ok) {
        std::fprintf(stderr, "ROUND1 失败，无法进入对比测试。\n");
        return 2;
    }
    if (round1.reasoning.empty()) {
        std::fprintf(stderr, "WARN: ROUND1 没有 reasoning_content。\n"
                             "可能模型不是 reasoner，或服务端不再返回该字段。\n");
        std::fprintf(stderr, "结论：当前回传策略可能已经无意义。\n");
        // 仍继续做组 B 测试，看是否 200。
    }

    std::printf("\n---- ROUND1 assistant content (preview) ----\n%s\n",
                round1.content.substr(0, 300).c_str());
    if (!round1.reasoning.empty()) {
        std::printf("---- ROUND1 reasoning (preview, <=300) ----\n%s\n\n",
                    round1.reasoning.substr(0, 300).c_str());
    }

    // ---- 组 A：回传 reasoning_content（项目当前实现）----
    json msgsA = firstMessages;
    {
        json assistant = {
            {"role",    "assistant"},
            {"content", round1.content},
        };
        if (!round1.reasoning.empty())
            assistant["reasoning_content"] = round1.reasoning;
        msgsA.push_back(assistant);
        msgsA.push_back({{"role", "user"},
                         {"content", "Now double that result. Reply with only the number."}});
    }
    auto roundA = callDeepseek(*key, msgsA, "GROUP_A_keep");

    // ---- 组 B：剥离 reasoning_content ----
    json msgsB = firstMessages;
    {
        json assistant = {
            {"role",    "assistant"},
            {"content", round1.content},
        };
        msgsB.push_back(assistant);
        msgsB.push_back({{"role", "user"},
                         {"content", "Now double that result. Reply with only the number."}});
    }
    auto roundB = callDeepseek(*key, msgsB, "GROUP_B_strip");

    // ---- 结论 ----
    std::printf("\n================ VERDICT ================\n");
    std::printf("GROUP_A (回传 reasoning_content): %s (HTTP %ld)\n",
                roundA.ok ? "OK" : "FAILED", roundA.status);
    std::printf("GROUP_B (剥离 reasoning_content): %s (HTTP %ld)\n",
                roundB.ok ? "OK" : "FAILED", roundB.status);

    if (roundA.ok && roundB.ok) {
        std::printf("\n结论：两种方式都被接受，回传与否对 deepseek-reasoner 无影响。\n"
                    "建议：deepseek_chat_client.cpp:163-166 的回传逻辑可保留，但注释\n"
                    "      '必须回传否则 400' 应修正为 '回传可选'。\n");
        return 0;
    }
    if (roundA.ok && !roundB.ok) {
        std::printf("\n结论：必须回传 reasoning_content，否则 HTTP %ld。\n"
                    "建议：维持现状，注释正确。仅放宽 K-12 哨兵为日志告警。\n",
                    roundB.status);
        return 0;
    }
    if (!roundA.ok && roundB.ok) {
        std::printf("\n结论：必须剥离 reasoning_content，否则 HTTP %ld。\n"
                    "建议：删除 deepseek_chat_client.cpp:163-166 的回传逻辑，\n"
                    "      修正 chat_provider.h:51-53 注释。\n",
                    roundA.status);
        return 3;
    }
    std::printf("\n两组均失败，疑似 Key/配额/网络问题。请人工检查上面 HTTP body。\n");
    return 4;
}
