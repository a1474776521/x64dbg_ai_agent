// util/utf8_safe.h
//
// K-38 抽出（K-39 共用）：UTF-8 安全工具。
//
// 提供两个 inline 函数：
//   - safeUtf8Truncate(s, maxBytes)：按 UTF-8 字符边界截断，避免切断多字节序列
//   - sanitizeUtf8(s)：把字符串中的非法 UTF-8 字节替换为 '?'
//
// 这些函数原本住在 src/ai/agent_loop.cpp 的匿名 namespace（K-38）。
// K-39 引入 system_tools（fs_read_file / shell_cmd 等）也要给 LLM 回 UTF-8
// 文本，需要复用同样的兜底逻辑，因此抽到这个 header。
//
// 用法：所有从外部（文件磁盘、shell stdout/stderr、PE 字符串等）读到的字节
// 流，在塞进 ToolResult.data 之前，**都应**走一遍 sanitizeUtf8。
// nlohmann::json::dump() 对非法 UTF-8 会抛 type_error.316，会把整个
// agent run 炸掉（K-38 的真实事故）。
#pragma once

#include <cstddef>
#include <string>

namespace x64ai::util {

// 按 UTF-8 字符边界截断。
//
// 算法：
//   ASCII 字节  (0xxxxxxx)        独立成字符
//   续接字节    (10xxxxxx)        必须紧跟前导字节
//   2-byte 头   (110xxxxx)        +1 续接
//   3-byte 头   (1110xxxx)        +2 续接
//   4-byte 头   (11110xxx)        +3 续接
// 回退最多 3 字节（UTF-8 最长 4 字节）。
//
// 返回截断后的字符串；输入 size <= maxBytes 时原样返回。
inline std::string safeUtf8Truncate(const std::string& s, std::size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    std::size_t cut = maxBytes;
    // 若 s[cut] 是续接字节，往前回退到首字节，再砍掉首字节（不完整 → 整个丢）
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    // 现在 s[cut] 是首字节或 ASCII。检查它的预期续接字节数是否齐了。
    // 若不齐（被 maxBytes 切断），把这个首字节也砍掉。
    if (cut < s.size()) {
        unsigned char b = static_cast<unsigned char>(s[cut]);
        std::size_t need = 0;
        if ((b & 0x80) == 0)         need = 0;          // ASCII，本身完整
        else if ((b & 0xE0) == 0xC0) need = 1;
        else if ((b & 0xF0) == 0xE0) need = 2;
        else if ((b & 0xF8) == 0xF0) need = 3;
        else                          need = 0;          // 非法字节，原地切
        // 若 cut+1+need 超出 maxBytes（即续接字节不齐），整字符砍掉
        if (need > 0 && (cut + 1 + need) > maxBytes) {
            // do nothing; cut 保持指向首字节即可（substr(0, cut) 把这个字符整个丢掉）
        } else {
            cut = cut + 1 + need;  // 这个字符完整保留
        }
    }
    return s.substr(0, cut);
}

// 把字符串中的非法 UTF-8 字节替换为 '?'。合法 UTF-8 序列保持原样。
//
// 规则：
//   - 0xxxxxxx：ASCII，原样保留
//   - 110xxxxx + 10xxxxxx：2-byte，校验 overlong（首字节 < 0xC2 视为非法）
//   - 1110xxxx + 10xxxxxx*2：3-byte
//   - 11110xxx + 10xxxxxx*3：4-byte，校验首字节 <= 0xF4（U+10FFFF 上限）
//   - 续接字节孤立、0xF5-0xFF、末尾被截断 → 单字节替换 '?'
inline std::string sanitizeUtf8(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        std::size_t need = 0;
        if ((b & 0x80) == 0) {                 // 0xxxxxxx ASCII
            out.push_back(s[i++]);
            continue;
        } else if ((b & 0xE0) == 0xC0) {
            need = 1;
            if (b < 0xC2) { out.push_back('?'); ++i; continue; }  // overlong
        } else if ((b & 0xF0) == 0xE0) {
            need = 2;
        } else if ((b & 0xF8) == 0xF0) {
            need = 3;
            if (b > 0xF4) { out.push_back('?'); ++i; continue; }  // > U+10FFFF
        } else {
            // 续接字节（0x80-0xBF）孤立出现，或 0xF5-0xFF 非法首字节
            out.push_back('?');
            ++i;
            continue;
        }
        // 检查 need 个续接字节
        if (i + need >= n) {
            // 末尾被截断
            out.push_back('?');
            ++i;
            continue;
        }
        bool ok = true;
        for (std::size_t k = 1; k <= need; ++k) {
            unsigned char cb = static_cast<unsigned char>(s[i + k]);
            if ((cb & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) {
            out.push_back('?');
            ++i;
            continue;
        }
        // 合法 N 字节字符，整段拷过去
        out.append(s, i, need + 1);
        i += need + 1;
    }
    return out;
}

}  // namespace x64ai::util
