// ai/tools/scan_strings_tool.cpp
//
// K-34 scan_strings  - 内存范围 / 模块字符串扫描，恶意代码取证强相关
//
// 设计要点：
//   - 双编码：ASCII (0x20..0x7E + 常见空白) + UTF-16LE（成对低字节 0、高字节可见）
//   - 范围上限：64 MB（硬上限，超过 truncate 并附 hint）；分块 1 MB 读避免大块 alloc
//   - 输出上限：2000 条；超过返回 truncated=true + 已扫描区段提示
//   - only_suspicious=true：仅返回命中 IOC 模式的字符串（默认 false 全部返回）
//   - IOC 分类：c2 / path / cmd / crypto / registry / mutex / generic（启发式）
//   - 完全只读
//
// 字符串边界规则：
//   ASCII: 连续 >=min_len 个可打印字符；遇到不可打印或结束截断
//   UTF-16LE: low byte 可打印 + high byte == 0；同样 >=min_len
//
// 性能：64MB 全扫预计 <500ms；DbgMemRead 失败页跳过到下一可读段
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include <Windows.h>
#include "bridgemain.h"
#include "_scriptapi_module.h"

#include "util/logging.h"

namespace x64ai {

namespace {

constexpr std::uint64_t kMaxScanBytes   = 64ULL * 1024 * 1024;  // 64 MB
constexpr std::size_t   kMaxResultItems = 2000;
constexpr std::size_t   kReadChunkBytes = 1ULL * 1024 * 1024;   // 1 MB
constexpr std::size_t   kMinStrLen      = 4;
constexpr std::size_t   kMaxStrLen      = 512;  // 单串过长截断（防垃圾数据爆 token）

std::string formatHexU64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
    return buf;
}

bool parseUInt64(const nlohmann::json& v, std::uint64_t& out)
{
    if (v.is_number_unsigned()) { out = v.get<std::uint64_t>(); return true; }
    if (v.is_number_integer()) {
        auto i = v.get<std::int64_t>();
        if (i < 0) return false;
        out = static_cast<std::uint64_t>(i);
        return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s.empty()) return false;
        try {
            std::size_t pos = 0;
            std::uint64_t r;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                r = std::stoull(s.substr(2), &pos, 16);
                if (pos + 2 != s.size()) return false;
            } else {
                r = std::stoull(s, &pos, 0);
                if (pos != s.size()) return false;
            }
            out = r;
            return true;
        } catch (...) { return false; }
    }
    return false;
}

inline bool isAsciiPrintable(unsigned char c)
{
    // 0x20..0x7E 可打印；额外接受 \t \r \n 以兼容路径/json/cmd 块
    return (c >= 0x20 && c < 0x7F) || c == '\t' || c == '\r' || c == '\n';
}

// ====== IOC 启发式分类 ======
//
// 命中任意分类即视为 "suspicious"；同条字符串返回首个命中的标签。
// 不做精确 family 归属，只用于过滤、便于 LLM 优先关注。
struct IocClassifier {
    // 注意 std::regex 在 MSVC 下 ECMAScript 默认；用 case-insensitive
    std::regex c2_url      {R"((https?|ftp|wss?)://[\w\-._~:/?#\[\]@!$&'()*+,;=%]+)",
                            std::regex::ECMAScript | std::regex::icase};
    std::regex c2_ipport   {R"(\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(:\d{1,5})?\b)"};
    std::regex c2_onion    {R"(\b[a-z2-7]{16,56}\.onion\b)",
                            std::regex::ECMAScript | std::regex::icase};
    std::regex path_env    {R"(%(APPDATA|LOCALAPPDATA|TEMP|USERPROFILE|PROGRAMDATA|SYSTEMROOT|WINDIR|PUBLIC)%)",
                            std::regex::ECMAScript | std::regex::icase};
    std::regex cmd_exec    {R"(\b(cmd(\.exe)?\s+/c|powershell(\.exe)?(\s+-(enc|encodedcommand|nop|w|noni))*|rundll32(\.exe)?|wmic|regsvr32(\.exe)?|mshta(\.exe)?|certutil(\.exe)?\s+-(urlcache|decode|encode))\b)",
                            std::regex::ECMAScript | std::regex::icase};
    std::regex reg_run     {R"(Software\\(Microsoft\\Windows\\CurrentVersion\\(Run|RunOnce|Explorer\\Run)|Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run)|CurrentControlSet\\Services\\)",
                            std::regex::ECMAScript | std::regex::icase};
    std::regex crypto_kw   {R"(\b(aes[-_]?(128|192|256)?|rc4|chacha20|salsa20|xor[-_ ]key|rsa[-_]?(1024|2048|4096))\b)",
                            std::regex::ECMAScript | std::regex::icase};
    std::regex mutex_marker{R"(\b(Global\\|Local\\|Session\\)[a-zA-Z0-9_\-{}]{6,})"};
    std::regex base64_blk  {R"([A-Za-z0-9+/]{64,}={0,2})"};  // 长 base64 块（>64 字符）

    // 返回类别字符串；空字符串表示无命中
    const char* classify(const std::string& s) const
    {
        // 顺序敏感：高置信优先
        if (std::regex_search(s, c2_url))       return "c2_url";
        if (std::regex_search(s, c2_onion))     return "c2_onion";
        if (std::regex_search(s, c2_ipport))    return "c2_ip";
        if (std::regex_search(s, cmd_exec))     return "cmd_exec";
        if (std::regex_search(s, reg_run))      return "registry_persist";
        if (std::regex_search(s, path_env))     return "path_env";
        if (std::regex_search(s, mutex_marker)) return "mutex_marker";
        if (std::regex_search(s, crypto_kw))    return "crypto";
        if (std::regex_search(s, base64_blk))   return "base64_blob";
        return nullptr;
    }
};

struct ScanRange {
    std::uint64_t start;
    std::uint64_t size;  // 实际扫描字节数（已截断到 kMaxScanBytes）
    bool          truncatedByLimit;
};

bool resolveRange(const nlohmann::json& args, ScanRange& out, std::string& err)
{
    // 三种模式：
    //   1) module="xxx.dll"  -> 整个模块映像
    //   2) start + size      -> 指定范围
    //   3) 都不给            -> 报错（避免无意义全地址空间扫）
    if (args.contains("module") && args["module"].is_string() && !args["module"].get<std::string>().empty()) {
        Script::Module::ModuleInfo mi{};
        const auto modName = args["module"].get<std::string>();
        if (!Script::Module::InfoFromName(modName.c_str(), &mi)) {
            err = "module not loaded: " + modName;
            return false;
        }
        out.start = static_cast<std::uint64_t>(mi.base);
        out.size  = static_cast<std::uint64_t>(mi.size);
        out.truncatedByLimit = false;
        if (out.size > kMaxScanBytes) {
            out.size = kMaxScanBytes;
            out.truncatedByLimit = true;
        }
        return true;
    }
    if (args.contains("start") && args.contains("size")) {
        std::uint64_t s = 0, n = 0;
        if (!parseUInt64(args["start"], s)) { err = "invalid 'start'"; return false; }
        if (!parseUInt64(args["size"],  n)) { err = "invalid 'size'";  return false; }
        if (n == 0) { err = "'size' must be > 0"; return false; }
        out.start = s;
        out.size  = n;
        out.truncatedByLimit = false;
        if (out.size > kMaxScanBytes) {
            out.size = kMaxScanBytes;
            out.truncatedByLimit = true;
        }
        return true;
    }
    err = "must specify either 'module' OR ('start' + 'size')";
    return false;
}

// 从已读 chunk 中提取字符串，追加到 results
// baseVa: chunk 在被调试进程中的起始 VA
// 返回是否仍允许继续（false = 已达 kMaxResultItems）
struct StringItem {
    std::uint64_t va;
    const char*   encoding;  // "ascii" or "utf16le"
    std::string   text;
    const char*   category;  // nullptr = 无 IOC 命中
};

void extractAscii(std::uint64_t baseVa, const unsigned char* buf, std::size_t n,
                  std::size_t min_len, std::vector<StringItem>& out,
                  std::size_t max_items)
{
    std::size_t i = 0;
    while (i < n) {
        if (out.size() >= max_items) return;
        if (!isAsciiPrintable(buf[i])) { ++i; continue; }
        std::size_t j = i;
        while (j < n && isAsciiPrintable(buf[j]) && (j - i) < kMaxStrLen) ++j;
        if ((j - i) >= min_len) {
            StringItem it;
            it.va       = baseVa + i;
            it.encoding = "ascii";
            it.text.assign(reinterpret_cast<const char*>(buf + i), j - i);
            it.category = nullptr;
            out.push_back(std::move(it));
        }
        i = (j == i) ? (i + 1) : j;
    }
}

void extractUtf16(std::uint64_t baseVa, const unsigned char* buf, std::size_t n,
                  std::size_t min_len, std::vector<StringItem>& out,
                  std::size_t max_items)
{
    if (n < 2) return;
    // 2 字节步长扫；要求 low 可打印 + high == 0
    std::size_t i = 0;
    while (i + 1 < n) {
        if (out.size() >= max_items) return;
        if (!(buf[i + 1] == 0 && isAsciiPrintable(buf[i]))) { i += 2; continue; }
        std::size_t j = i;
        std::string s;
        while (j + 1 < n && buf[j + 1] == 0 && isAsciiPrintable(buf[j]) && s.size() < kMaxStrLen) {
            s.push_back(static_cast<char>(buf[j]));
            j += 2;
        }
        if (s.size() >= min_len) {
            StringItem it;
            it.va       = baseVa + i;
            it.encoding = "utf16le";
            it.text     = std::move(s);
            it.category = nullptr;
            out.push_back(std::move(it));
        }
        i = (j == i) ? (i + 2) : j;
    }
}

// ====== ScanStringsTool ======

class ScanStringsTool : public ITool {
public:
    std::string name() const override { return "scan_strings"; }
    std::string description() const override
    {
        return "Scan a memory range or whole module for printable strings (ASCII + UTF-16LE). "
               "Heuristically tags each hit with an IOC category: c2_url / c2_ip / c2_onion / "
               "cmd_exec / registry_persist / path_env / mutex_marker / crypto / base64_blob. "
               "Use this in malware triage to surface hard-coded C2 endpoints, persistence keys, "
               "PowerShell launchers, mutex fingerprints, or encoded payloads. "
               "Specify EITHER module=<name> OR start=<va>+size=<bytes>. "
               "Max 64 MB scanned per call, max 2000 strings returned. "
               "Set only_suspicious=true to drop strings that didn't match any IOC category."
               " (zh-CN: 在指定内存范围或模块映像中扫描 ASCII / UTF-16LE 可打印字符串，并自动给"
               "每条串打上 IOC 类别标签——C2 地址 / 持久化注册表键 / cmd&PowerShell / 路径环境变量 / "
               "互斥名 / 加密关键字 / base64 大块。恶意样本取证必备工具，可一次性挖出硬编码 C2/配置/"
               "marker。)";
    }
    std::string descriptionZh() const override
    {
        return "在指定内存范围或整个模块映像中扫描 ASCII / UTF-16LE 可打印字符串，"
               "并按启发式给每条串打 IOC 标签（c2_url / c2_ip / cmd_exec / registry_persist / "
               "path_env / mutex_marker / crypto / base64_blob 等）。"
               "用法：参数二选一——指定 module（扫整个模块），或同时指定 start + size。"
               "硬上限：单次扫描 ≤64 MB、返回 ≤2000 条；only_suspicious=true 时仅返回命中 IOC 的串。"
               "恶意代码取证可用此挖出硬编码 C2 端点、持久化键名、PowerShell 启动器、互斥指纹、Base64 配置块。";
    }

    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"module", {{"type", "string"},
                            {"description", "Module name (e.g. 'malware.exe'). Scans the whole module image. Mutually exclusive with start+size."}}},
                {"start",  {{"type", "string"},
                            {"description", "Range start VA (hex string). Use with 'size'."}}},
                {"size",   {{"type", "string"},
                            {"description", "Range size in bytes (decimal or 0x hex). Capped at 64 MB."}}},
                {"min_len",{{"type", "integer"},
                            {"description", "Minimum string length (4-64, default 6)."},
                            {"minimum", 4}, {"maximum", 64}}},
                {"encoding",{{"type", "string"},
                             {"description", "'ascii' | 'utf16' | 'both' (default 'both')."},
                             {"enum", nlohmann::json::array({"ascii", "utf16", "both"})}}},
                {"only_suspicious", {{"type", "boolean"},
                                     {"description", "If true, only return strings that hit an IOC category. Default false."}}},
            }},
        };
    }

    // 内存范围 64MB scan 可能产生 ~MB 级 JSON；上调到 1MB
    std::size_t maxResultBytes() const override { return 1024 * 1024; }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }

        ScanRange range{};
        {
            std::string err;
            if (!resolveRange(args, range, err)) {
                r.ok = false; r.error = err;
                return r;
            }
        }

        std::size_t min_len = 6;
        if (args.contains("min_len") && args["min_len"].is_number_integer()) {
            int v = args["min_len"].get<int>();
            if (v < 4 || v > 64) { r.ok = false; r.error = "'min_len' out of range [4,64]"; return r; }
            min_len = static_cast<std::size_t>(v);
        }

        bool wantAscii = true, wantUtf16 = true;
        if (args.contains("encoding") && args["encoding"].is_string()) {
            const auto e = args["encoding"].get<std::string>();
            if      (e == "ascii") { wantAscii = true;  wantUtf16 = false; }
            else if (e == "utf16") { wantAscii = false; wantUtf16 = true;  }
            else if (e == "both")  { /* both */ }
            else { r.ok = false; r.error = "'encoding' must be ascii/utf16/both"; return r; }
        }

        const bool onlySus = args.value("only_suspicious", false);

        // 分块读 + 抽取
        std::vector<StringItem> items;
        items.reserve(256);
        std::unique_ptr<unsigned char[]> chunk(new unsigned char[kReadChunkBytes]);

        std::uint64_t pos       = range.start;
        const std::uint64_t end = range.start + range.size;
        std::size_t okPages = 0, badPages = 0;

        // 软上限留一点 headroom（max_items * 2，最后再过滤截到 max）
        const std::size_t softCap = kMaxResultItems * 2;

        while (pos < end && items.size() < softCap) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(kReadChunkBytes, end - pos));
            std::memset(chunk.get(), 0, want);
            if (!DbgMemRead(static_cast<duint>(pos), chunk.get(), static_cast<duint>(want))) {
                // 试小粒度按 4KB 页探测，跳过坏页
                badPages++;
                constexpr std::size_t kPg = 0x1000;
                for (std::size_t off = 0; off < want; off += kPg) {
                    const std::size_t pgLen = std::min<std::size_t>(kPg, want - off);
                    if (DbgMemRead(static_cast<duint>(pos + off), chunk.get() + off,
                                   static_cast<duint>(pgLen))) {
                        if (wantAscii) extractAscii(pos + off, chunk.get() + off, pgLen, min_len, items, softCap);
                        if (wantUtf16) extractUtf16(pos + off, chunk.get() + off, pgLen, min_len, items, softCap);
                        okPages++;
                    } else {
                        std::memset(chunk.get() + off, 0, pgLen);  // 防上面 alloc 残留误判
                    }
                }
            } else {
                okPages++;
                if (wantAscii) extractAscii(pos, chunk.get(), want, min_len, items, softCap);
                if (wantUtf16) extractUtf16(pos, chunk.get(), want, min_len, items, softCap);
            }
            pos += want;
        }

        // 分类
        IocClassifier clf;
        std::size_t classifiedHits = 0;
        for (auto& it : items) {
            it.category = clf.classify(it.text);
            if (it.category) ++classifiedHits;
        }

        // only_suspicious 过滤
        if (onlySus) {
            items.erase(std::remove_if(items.begin(), items.end(),
                                       [](const StringItem& x){ return x.category == nullptr; }),
                        items.end());
        }

        // 截断到硬上限
        const std::size_t totalFound = items.size();
        bool truncatedItems = false;
        if (items.size() > kMaxResultItems) {
            items.resize(kMaxResultItems);
            truncatedItems = true;
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& it : items) {
            nlohmann::json o = {
                {"va",       formatHexU64(it.va)},
                {"encoding", it.encoding},
                {"text",     it.text},
            };
            if (it.category) o["category"] = it.category;
            arr.push_back(std::move(o));
        }

        XAI_LOG_INFO("scan_strings: range=[{},+{}KB] items_total={} returned={} ioc_hits={} "
                     "ok_pages={} bad_pages={} truncated_by_limit={} truncated_items={}",
                     formatHexU64(range.start), range.size / 1024,
                     totalFound, items.size(), classifiedHits,
                     okPages, badPages, range.truncatedByLimit, truncatedItems);

        r.ok = true;
        r.data = {
            {"start",              formatHexU64(range.start)},
            {"size",               range.size},
            {"truncated_by_limit", range.truncatedByLimit},  // 扫描区段被 64MB 截
            {"truncated_items",    truncatedItems},          // 返回条目被 2000 截
            {"total",              totalFound},
            {"count",              items.size()},
            {"ioc_hits",           classifiedHits},
            {"only_suspicious",    onlySus},
            {"ok_pages",           okPages},
            {"bad_pages",          badPages},
            {"strings",            std::move(arr)},
        };
        return r;
    }
};

}  // namespace

void registerScanStringsTool(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<ScanStringsTool>(), "forensics");
}

}  // namespace x64ai
