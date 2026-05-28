// ai/tools/data_write_tools.cpp
//
// S4：数据写工具三件套。
//
//   patch_memory   把一段 hex 字节写入指定 VA（DbgMemWrite）
//   set_register   修改通用寄存器 / 段相关 / EFLAGS（Script::Register::Set）
//   write_string   按编码（utf8 / utf16le / ascii）把字符串写入 VA，自动 \0 终止
//
// 共性：
//   - category() = Write
//   - requiresUserConfirmation() = true（5s 倒计时确认 + write_audit.log）
//   - 必须在 debugger active；进程是否 paused 不做硬限制（patch 运行中也允许，
//     但 dispatch 层已通过 confirm 阻止误操作）。
//   - 越界检查：DbgMemIsValidReadPtr + 末字节也检测，防止跨页 partial write。
//   - 寄存器白名单：name 必须命中 kRegisterMap；CR/DR/FPU 等不支持。
//   - write_string utf16le 自动按 wchar_t 对齐，自动追加 2 字节 0；
//     ascii 路径会拒绝 >0x7F 字节，避免静默 mojibake。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <Windows.h>
#include "bridgemain.h"
#include "_dbgfunctions.h"
#include "_scriptapi_register.h"

#include "util/logging.h"

namespace x64ai {

namespace {

std::string formatHexU64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

// 与 debug_write_tools 的 parseVa 同款（独立 TU，重复定义无冲突）。
bool parseVa(const nlohmann::json& args, const char* key, std::uint64_t& out, std::string& err)
{
    if (!args.contains(key)) {
        err = std::string("'") + key + "' is required";
        return false;
    }
    const auto& v = args[key];
    if (v.is_number_unsigned()) { out = v.get<std::uint64_t>(); return true; }
    if (v.is_number_integer())  { out = static_cast<std::uint64_t>(v.get<std::int64_t>()); return true; }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        try {
            std::size_t pos = 0;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                out = std::stoull(s.substr(2), &pos, 16);
                if (pos + 2 == s.size()) return true;
            } else {
                out = std::stoull(s, &pos, 0);
                if (pos == s.size()) return true;
            }
        } catch (...) {}
    }
    err = std::string("invalid '") + key + "': expected number or hex string";
    return false;
}

// 把 "DE AD BE EF" / "deadbeef" / "DE,AD,BE,EF" 解析成字节数组。
// - 允许空白 / 逗号 / 冒号 / 短横作分隔
// - 不允许通配符（patch_memory 是精确写入）
bool parseHexBytes(const std::string& s, std::vector<std::uint8_t>& out, std::string& err)
{
    out.clear();
    int nibble = -1;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == ',' || c == ':' || c == '-' || c == '_') {
            if (nibble != -1) {
                err = "odd nibble before delimiter";
                return false;
            }
            continue;
        }
        int v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else {
            err = std::string("invalid hex char: '") + c + "'";
            return false;
        }
        if (nibble == -1) {
            nibble = v;
        } else {
            out.push_back(static_cast<std::uint8_t>((nibble << 4) | v));
            nibble = -1;
        }
    }
    if (nibble != -1) {
        err = "odd number of hex nibbles";
        return false;
    }
    if (out.empty()) {
        err = "no hex bytes parsed";
        return false;
    }
    return true;
}

// 简单将字节数组重新格式化为 "DE AD BE EF" 便于 audit 日志可读
std::string formatHexBytes(const std::vector<std::uint8_t>& v, std::size_t maxBytes = 64)
{
    std::string out;
    const std::size_t n = std::min(v.size(), maxBytes);
    out.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", v[i]);
        if (i) out.push_back(' ');
        out.append(buf);
    }
    if (v.size() > maxBytes) {
        char tail[32];
        std::snprintf(tail, sizeof(tail), " ...(+%zu)", v.size() - maxBytes);
        out.append(tail);
    }
    return out;
}

// 寄存器名 → RegisterEnum 映射；name 在比较前会转大写。
// 仅支持通用 GPR + DR + CFLAGS / CIP / CSP / Cxx。
// XMM/YMM/MXCSR/FPU 不支持（Script API 没暴露 Set 入口）。
struct RegEntry {
    const char* name;
    Script::Register::RegisterEnum id;
    int byteWidth;   // 1/2/4/8（写入时校验 value 范围）
};

const std::vector<RegEntry>& regTable()
{
    using R = Script::Register::RegisterEnum;
    static const std::vector<RegEntry> tbl = {
        // DR
        {"DR0", R::DR0, sizeof(void*)}, {"DR1", R::DR1, sizeof(void*)},
        {"DR2", R::DR2, sizeof(void*)}, {"DR3", R::DR3, sizeof(void*)},
        {"DR6", R::DR6, sizeof(void*)}, {"DR7", R::DR7, sizeof(void*)},
        // x86 GPR + 32/16/8 子寄存器
        {"EAX", R::EAX, 4}, {"AX", R::AX, 2}, {"AH", R::AH, 1}, {"AL", R::AL, 1},
        {"EBX", R::EBX, 4}, {"BX", R::BX, 2}, {"BH", R::BH, 1}, {"BL", R::BL, 1},
        {"ECX", R::ECX, 4}, {"CX", R::CX, 2}, {"CH", R::CH, 1}, {"CL", R::CL, 1},
        {"EDX", R::EDX, 4}, {"DX", R::DX, 2}, {"DH", R::DH, 1}, {"DL", R::DL, 1},
        {"EDI", R::EDI, 4}, {"DI", R::DI, 2},
        {"ESI", R::ESI, 4}, {"SI", R::SI, 2},
        {"EBP", R::EBP, 4}, {"BP", R::BP, 2},
        {"ESP", R::ESP, 4}, {"SP", R::SP, 2},
        {"EIP", R::EIP, 4},
#ifdef _WIN64
        {"RAX", R::RAX, 8}, {"RBX", R::RBX, 8}, {"RCX", R::RCX, 8}, {"RDX", R::RDX, 8},
        {"RSI", R::RSI, 8}, {"SIL", R::SIL, 1},
        {"RDI", R::RDI, 8}, {"DIL", R::DIL, 1},
        {"RBP", R::RBP, 8}, {"BPL", R::BPL, 1},
        {"RSP", R::RSP, 8}, {"SPL", R::SPL, 1},
        {"RIP", R::RIP, 8},
        {"R8",  R::R8,  8}, {"R8D",  R::R8D,  4}, {"R8W",  R::R8W,  2}, {"R8B",  R::R8B,  1},
        {"R9",  R::R9,  8}, {"R9D",  R::R9D,  4}, {"R9W",  R::R9W,  2}, {"R9B",  R::R9B,  1},
        {"R10", R::R10, 8}, {"R10D", R::R10D, 4}, {"R10W", R::R10W, 2}, {"R10B", R::R10B, 1},
        {"R11", R::R11, 8}, {"R11D", R::R11D, 4}, {"R11W", R::R11W, 2}, {"R11B", R::R11B, 1},
        {"R12", R::R12, 8}, {"R12D", R::R12D, 4}, {"R12W", R::R12W, 2}, {"R12B", R::R12B, 1},
        {"R13", R::R13, 8}, {"R13D", R::R13D, 4}, {"R13W", R::R13W, 2}, {"R13B", R::R13B, 1},
        {"R14", R::R14, 8}, {"R14D", R::R14D, 4}, {"R14W", R::R14W, 2}, {"R14B", R::R14B, 1},
        {"R15", R::R15, 8}, {"R15D", R::R15D, 4}, {"R15W", R::R15W, 2}, {"R15B", R::R15B, 1},
#endif
        // 架构无关别名（duint 宽度，32 或 64）
        {"CIP", R::CIP, sizeof(void*)}, {"CSP", R::CSP, sizeof(void*)},
        {"CAX", R::CAX, sizeof(void*)}, {"CBX", R::CBX, sizeof(void*)},
        {"CCX", R::CCX, sizeof(void*)}, {"CDX", R::CDX, sizeof(void*)},
        {"CDI", R::CDI, sizeof(void*)}, {"CSI", R::CSI, sizeof(void*)},
        {"CBP", R::CBP, sizeof(void*)},
        {"CFLAGS", R::CFLAGS, sizeof(void*)},
    };
    return tbl;
}

const RegEntry* lookupRegister(const std::string& nameUpper)
{
    for (const auto& e : regTable()) {
        if (nameUpper == e.name) return &e;
    }
    return nullptr;
}

std::string toUpperCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    return s;
}

// 把 args["value"] 解析为无符号整数：支持 number / "123" / "0x1F"。
bool parseUInt64(const nlohmann::json& args, const char* key, std::uint64_t& out, std::string& err)
{
    if (!args.contains(key)) { err = std::string("'") + key + "' required"; return false; }
    const auto& v = args[key];
    if (v.is_number_unsigned()) { out = v.get<std::uint64_t>(); return true; }
    if (v.is_number_integer())  { out = static_cast<std::uint64_t>(v.get<std::int64_t>()); return true; }
    if (v.is_string()) {
        try {
            const std::string s = v.get<std::string>();
            std::size_t pos = 0;
            if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
                out = std::stoull(s.substr(2), &pos, 16);
                if (pos + 2 == s.size()) return true;
            } else {
                out = std::stoull(s, &pos, 0);
                if (pos == s.size()) return true;
            }
        } catch (...) {}
    }
    err = std::string("invalid '") + key + "': expected uint or hex string";
    return false;
}

// 校验内存区段全部可读（写之前用 IsValidReadPtr 是最简单的"已映射"判定）。
// DbgMemIsValidReadPtr 只校验单字节，这里采每 4 KB 边界 + 末字节都查一次。
bool checkMemRangeWritable(std::uint64_t va, std::size_t size, std::string& err)
{
    if (size == 0) { err = "size==0"; return false; }
    if (va == 0)   { err = "addr is null"; return false; }
    // 4 KB 步进 + 末字节
    for (std::uint64_t p = va; p < va + size; p += 0x1000) {
        if (!DbgMemIsValidReadPtr(static_cast<duint>(p))) {
            err = "address not mapped: " + formatHexU64(p);
            return false;
        }
    }
    const std::uint64_t last = va + size - 1;
    if (!DbgMemIsValidReadPtr(static_cast<duint>(last))) {
        err = "tail not mapped: " + formatHexU64(last);
        return false;
    }
    return true;
}

}  // namespace

// ============= T-07 patch_memory =============
class PatchMemoryTool : public ITool {
public:
    std::string name() const override { return "patch_memory"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Write a sequence of raw bytes to memory at VA and register the change "
               "as a tracked patch (visible to list_patches, undoable by restore_patch, "
               "exportable by patch_file). "
               "bytes_hex accepts \"DE AD BE EF\" / \"deadbeef\" / \"DE,AD,BE,EF\" "
               "(spaces/commas/colons/dashes as delimiters; no wildcards). "
               "Caller MUST verify the target is writable code/data; on protected pages "
               "this will fail and report the address.";
    }
    std::string descriptionZh() const override
    {
        return "向指定 VA 写入一段原始字节，并登记为追踪补丁"
               "（可被 list_patches 看到、restore_patch 撤销、patch_file 导出）。"
               "bytes_hex 接受 \"DE AD BE EF\" / \"deadbeef\" / \"DE,AD,BE,EF\" "
               "（空格 / 逗号 / 冒号 / 短横线作分隔符；不支持通配符）。"
               "调用方需自行确认目标可写；若是受保护页面将失败并返回出错地址。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"addr",      {{"type", "string"}, {"description", "VA, e.g. \"0x401000\""}}},
                {"bytes_hex", {{"type", "string"}, {"description", "Hex byte string, e.g. \"90 90 90\" (NOPs)"}}},
            }},
            {"required", nlohmann::json::array({"addr", "bytes_hex"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string err;
        if (!parseVa(args, "addr", va, err)) { r.ok = false; r.error = err; return r; }
        if (!args.contains("bytes_hex") || !args["bytes_hex"].is_string()) {
            r.ok = false; r.error = "'bytes_hex' required (string)"; return r;
        }
        std::vector<std::uint8_t> bytes;
        if (!parseHexBytes(args["bytes_hex"].get<std::string>(), bytes, err)) {
            r.ok = false; r.error = err; return r;
        }
        // 上限 4 KB，防 prompt 注入或误用
        if (bytes.size() > 4096) {
            r.ok = false;
            r.error = "bytes_hex too long: " + std::to_string(bytes.size()) + " > 4096";
            return r;
        }
        if (!checkMemRangeWritable(va, bytes.size(), err)) {
            r.ok = false; r.error = err; return r;
        }
        // 走 DBGFUNCTIONS->MemPatch 而非裸 DbgMemWrite，后者不会登记到 Patches 表，
        // 导致 list_patches 看不到、restore_patch 撤不回、patch_file 也导不出。
        // 见 docs/known-issues.md K-28。
        const auto* fns = DbgFunctions();
        if (!fns || !fns->MemPatch) {
            r.ok = false; r.error = "DbgFunctions->MemPatch is null"; return r;
        }
        if (!fns->MemPatch(static_cast<duint>(va), bytes.data(),
                           static_cast<duint>(bytes.size()))) {
            r.ok = false;
            r.error = "MemPatch failed at " + formatHexU64(va) +
                      " size=" + std::to_string(bytes.size());
            return r;
        }
        XAI_LOG_INFO("patch_memory: wrote {} bytes (tracked) at {}",
                     bytes.size(), formatHexU64(va).c_str());
        r.ok = true;
        r.data = {
            {"addr",         formatHexU64(va)},
            {"size",         bytes.size()},
            {"bytes_pretty", formatHexBytes(bytes)},
        };
        return r;
    }
};

// ============= T-08 set_register =============
class SetRegisterTool : public ITool {
public:
    std::string name() const override { return "set_register"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Set the value of a CPU register. "
               "name is case-insensitive; supported: GPRs (RAX/EAX/AX/AH/AL etc.), "
               "R8-R15 + D/W/B variants (x64 only), RIP/EIP/CIP, RSP/ESP/CSP, "
               "RBP/EBP/CBP, RSI/RDI + sub-regs, DR0-DR3/DR6/DR7, CFLAGS, "
               "and arch-neutral aliases Cxx. "
               "XMM/YMM/MXCSR/FPU are NOT supported.";
    }
    std::string descriptionZh() const override
    {
        return "设置 CPU 寄存器的值。name 大小写不敏感；支持：通用寄存器（RAX/EAX/AX/AH/AL 等）、"
               "R8-R15 及其 D/W/B 变体（仅 x64）、RIP/EIP/CIP、RSP/ESP/CSP、RBP/EBP/CBP、"
               "RSI/RDI 及子寄存器、DR0-DR3/DR6/DR7、CFLAGS，以及架构无关别名 Cxx。"
               "不支持 XMM / YMM / MXCSR / FPU。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"name",  {{"type", "string"},
                           {"description", "Register name (case-insensitive), e.g. \"rax\", \"eflags\""}}},
                {"value", {{"type", "string"},
                           {"description", "New value as number or hex string, e.g. \"0x401000\""}}},
            }},
            {"required", nlohmann::json::array({"name", "value"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        if (!args.contains("name") || !args["name"].is_string()) {
            r.ok = false; r.error = "'name' required (string)"; return r;
        }
        const std::string nameUpper = toUpperCopy(args["name"].get<std::string>());
        const RegEntry* e = lookupRegister(nameUpper);
        if (!e) {
            r.ok = false; r.error = "unsupported register: " + nameUpper;
            return r;
        }
        std::uint64_t value = 0; std::string err;
        if (!parseUInt64(args, "value", value, err)) { r.ok = false; r.error = err; return r; }

        // 范围校验：value 不能超出寄存器宽度
        if (e->byteWidth < 8) {
            const std::uint64_t maxV = (e->byteWidth == 1) ? 0xFFull
                                     : (e->byteWidth == 2) ? 0xFFFFull
                                     : (e->byteWidth == 4) ? 0xFFFFFFFFull
                                     : 0;  // unreachable
            if (value > maxV) {
                char buf[80];
                std::snprintf(buf, sizeof(buf),
                              "value 0x%llX exceeds %d-byte register %s (max 0x%llX)",
                              (unsigned long long)value, e->byteWidth,
                              e->name, (unsigned long long)maxV);
                r.ok = false; r.error = buf; return r;
            }
        }
        if (!Script::Register::Set(e->id, static_cast<duint>(value))) {
            r.ok = false;
            r.error = std::string("Script::Register::Set failed for ") + e->name;
            return r;
        }
        XAI_LOG_INFO("set_register: {} <- 0x{:X}", e->name, value);
        r.ok = true;
        r.data = {
            {"name",     e->name},
            {"value",    formatHexU64(value)},
            {"byte_width", e->byteWidth},
        };
        return r;
    }
};

// ============= T-09 write_string =============
class WriteStringTool : public ITool {
public:
    std::string name() const override { return "write_string"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Write a string into memory at VA, NUL-terminated. "
               "encoding: \"utf8\" (default) / \"utf16le\" / \"ascii\". "
               "Caller passes 'value' as a normal JSON string (already Unicode). "
               "ascii rejects bytes > 0x7F. utf16le emits 2-byte units little-endian. "
               "Both encodings auto-append the proper terminator (1 byte for utf8/ascii, "
               "2 bytes for utf16le).";
    }
    std::string descriptionZh() const override
    {
        return "向指定 VA 写入一个字符串（自动追加 NUL 结尾）。"
               "encoding：\"utf8\"（默认）/ \"utf16le\" / \"ascii\"。"
               "value 按普通 JSON 字符串传入（本就是 Unicode）。"
               "ascii 拒绝字节 > 0x7F；utf16le 按小端 2 字节单元写出。"
               "终结符自动追加（utf8/ascii 为 1 字节，utf16le 为 2 字节）。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"addr",     {{"type", "string"}, {"description", "VA"}}},
                {"value",    {{"type", "string"}, {"description", "String content (JSON-unescaped already)"}}},
                {"encoding", {{"type", "string"},
                              {"enum", nlohmann::json::array({"utf8", "utf16le", "ascii"})},
                              {"description", "Output encoding; default 'utf8'"}}},
            }},
            {"required", nlohmann::json::array({"addr", "value"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string err;
        if (!parseVa(args, "addr", va, err)) { r.ok = false; r.error = err; return r; }
        if (!args.contains("value") || !args["value"].is_string()) {
            r.ok = false; r.error = "'value' required (string)"; return r;
        }
        const std::string value = args["value"].get<std::string>();
        std::string enc = "utf8";
        if (args.contains("encoding") && args["encoding"].is_string()) {
            enc = args["encoding"].get<std::string>();
        }

        // value 上限：源串本身限 4 KB，编码后另算
        if (value.size() > 4096) {
            r.ok = false; r.error = "value too long (>4 KB before encoding)"; return r;
        }

        std::vector<std::uint8_t> bytes;
        if (enc == "utf8") {
            bytes.assign(value.begin(), value.end());
            bytes.push_back(0);
        } else if (enc == "ascii") {
            for (unsigned char c : value) {
                if (c > 0x7F) {
                    r.ok = false;
                    char buf[64];
                    std::snprintf(buf, sizeof(buf),
                                  "non-ASCII byte 0x%02X in 'value'; use utf8 or utf16le",
                                  c);
                    r.error = buf;
                    return r;
                }
                bytes.push_back(c);
            }
            bytes.push_back(0);
        } else if (enc == "utf16le") {
            // 把 value 当 UTF-8 解码 → UTF-16LE 字节流
            const int u8len = static_cast<int>(value.size());
            int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(), u8len, nullptr, 0);
            if (wlen <= 0 && u8len > 0) {
                r.ok = false; r.error = "value is not valid UTF-8 (cannot decode for utf16le)";
                return r;
            }
            std::wstring w(static_cast<std::size_t>(wlen), L'\0');
            if (wlen > 0) {
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    value.data(), u8len, w.data(), wlen);
            }
            // wchar_t 在 Windows 是 2 字节 → 直接 reinterpret 一字节一字节拷
            bytes.resize((w.size() + 1) * 2);  // +1 用于 \0\0
            std::memcpy(bytes.data(), w.data(), w.size() * 2);
            // 末尾自动 0x00 0x00（resize 已清零）
        } else {
            r.ok = false; r.error = "unknown encoding: " + enc + " (utf8|utf16le|ascii)";
            return r;
        }

        if (bytes.size() > 8192) {  // 编码后硬上限
            r.ok = false;
            r.error = "encoded size too large: " + std::to_string(bytes.size()) + " > 8192";
            return r;
        }
        if (!checkMemRangeWritable(va, bytes.size(), err)) {
            r.ok = false; r.error = err; return r;
        }
        if (!DbgMemWrite(static_cast<duint>(va), bytes.data(), bytes.size())) {
            r.ok = false;
            r.error = "DbgMemWrite failed at " + formatHexU64(va) +
                      " size=" + std::to_string(bytes.size());
            return r;
        }
        XAI_LOG_INFO("write_string: enc={} {} bytes at {}",
                     enc.c_str(), bytes.size(), formatHexU64(va).c_str());
        r.ok = true;
        r.data = {
            {"addr",          formatHexU64(va)},
            {"encoding",      enc},
            {"encoded_size",  bytes.size()},
            {"value_preview", value.substr(0, 80)},
        };
        return r;
    }
};

void registerDataWriteTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<PatchMemoryTool>(), "write-patch");
    reg.registerTool(std::make_unique<SetRegisterTool>(), "write-patch");
    reg.registerTool(std::make_unique<WriteStringTool>(), "write-patch");
}

}  // namespace x64ai
