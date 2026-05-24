// ai/tools/tool_args_util.h
//
// Tool 参数解析辅助：所有 ITool 共享的小工具函数（header-only inline）。
//
// 设计目的（S0-H1 / S2-E）：
//   LLM 频繁把 number 序列化为 string（"0x100" / "32" / "0x20"）。
//   原本各工具用 nlohmann::json::is_number_integer() 严格校验会反复拒绝，
//   触发 LLM 重试浪费 agent 迭代。统一用本文件的宽松解析器。
#pragma once

#include <cstdio>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace x64ai {

// 宽松整数解析：接受
//   - JSON number（123、123.0 截断、unsigned）
//   - 十进制字符串 "123"
//   - 十六进制字符串 "0x100" / "0X100"
// 范围 clamp 到 [lo, hi]；越界视为错误（不是默默 clamp，避免悄悄忽略 LLM 的意图）。
// 返回 false 时 outErr 填友好原因（含 lo/hi）。
inline bool parseInt32Lenient(const nlohmann::json& v, int lo, int hi,
                              int& out, std::string& outErr)
{
    long long n = 0;
    bool ok = false;
    if (v.is_number_integer()) {
        n  = v.get<long long>();
        ok = true;
    } else if (v.is_number_float()) {
        n  = static_cast<long long>(v.get<double>());
        ok = true;
    } else if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (!s.empty()) {
            try {
                std::size_t pos = 0;
                if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                    n = std::stoll(s.substr(2), &pos, 16);
                    ok = (pos + 2 == s.size());
                } else {
                    n = std::stoll(s, &pos, 0);
                    ok = (pos == s.size());
                }
            } catch (...) {
                ok = false;
            }
        }
    }
    if (!ok) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "expected integer or decimal/hex string in [%d, %d]", lo, hi);
        outErr = buf;
        return false;
    }
    if (n < lo || n > hi) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "value out of range [%d, %d]", lo, hi);
        outErr = buf;
        return false;
    }
    out = static_cast<int>(n);
    return true;
}

// 可选 hint 整数：args.contains(key) 且为可识别格式时返回 true 并填 out；
// 不存在/为 null 时返回 false（保留 out 不变；调用方用默认值）。
// 若存在但非法（解析失败 / 越界），返回 false 且 outErr 非空，
// 调用方应回 ToolResult 报错而不是吞掉。
inline bool tryGetInt32Hint(const nlohmann::json& args, const char* key,
                            int lo, int hi, int& out, std::string& outErr)
{
    outErr.clear();
    if (!args.contains(key)) return false;
    const auto& v = args[key];
    if (v.is_null()) return false;
    return parseInt32Lenient(v, lo, hi, out, outErr);
}

}  // namespace x64ai
