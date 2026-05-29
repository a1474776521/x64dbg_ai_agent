// ai/tools/system_tools.cpp
//
// K-39 引入的 5 个"系统侧"工具：
//   - fs_read_file       (Read)        读磁盘文件文本（白名单内）
//   - fs_write_file      (Write+confirm) 覆盖/新建写文件（白名单内）
//   - fs_create_file     (Write+confirm) 仅当不存在时创建（不覆盖）
//   - shell_cmd          (Write+confirm，hardEnforced) cmd.exe /c <line>
//   - shell_pwsh         (Write+confirm，hardEnforced) pwsh.exe (fallback powershell.exe)
//
// 安全模型（按用户决策 Q1..Q6）：
//   1. Q1=A 严格路径白名单 fsAllowedDirs，默认 [{plugin_workdir},{plugin_temp}]
//   2. Q2=B Shell 命令完全开放，但**永不豁免 confirm**（confirm_policy::hardEnforcedSet）
//   3. Q3=C 提供 shell_cmd + shell_pwsh 两个工具
//   4. Q4=B cwd 可参数指定（须在 fsAllowedDirs 内）
//   5. Q5 大小/超时/编码全部参数化（含 default + cap）
//   6. Q6 全部进 audit log（由 ToolRegistry::dispatch 统一负责，本文件无需手动写）
//
// 路径白名单细节（pathValidate）：
//   - 不允许原始路径含 ".."（即使解析后落在白名单也拒，防绕过）
//   - 绝对路径化 + 大小写不敏感前缀匹配（白名单目录 + '\\'）
//   - 拒绝 reparse point（junction / symlink）
//   - 拒绝 UNC / 设备名
//
// 占位符（白名单目录条目可写）：
//   {plugin_workdir}  ->  %APPDATA%/x64dbg-ai-plugin/
//   {plugin_temp}     ->  %TEMP%/x64dbg-ai-plugin/
//   {debuggee_dir}    ->  当前主调试模块所在目录（无调试 -> 跳过此条）
//   {user_home}       ->  %USERPROFILE%/
//
// Shell 子进程细节：
//   - CreateProcessW + CREATE_NO_WINDOW（无黑窗）
//   - JobObject + JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE（超时整组杀，防 pwsh 起子进程逃跑）
//   - stdin 关闭（非交互）
//   - stdout/stderr 各 anonymous pipe + 独立线程读，防 pipe 满死锁
//   - shell_cmd：stdout/stderr 默认按 ACP（中文机 = GBK）解码 → UTF-8
//   - shell_pwsh：注入 [Console]::OutputEncoding=[Text.UTF8Encoding]::new()，按 UTF-8 解
//   - 末尾走 sanitizeUtf8（K-38 防御层）兜底
//   - 发 EventBus::ShellStarted / ShellFinished / ShellTimeout 三个事件
//
// 注册 + 强制 confirm：
//   - registerSystemTools(reg) 在 tool_registry.cpp::registerBuiltinTools() 末尾调
//   - shell_cmd / shell_pwsh 加入 confirm_policy.cpp::hardEnforcedSet()，永不豁免

#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_policy.h"
#include "ai/tools/tool_registry.h"

#include "dbg/event_bus.h"
#include "storage/project_context.h"
#include "util/config.h"
#include "util/logging.h"
#include "util/paths.h"
#include "util/utf8_safe.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace x64ai {

namespace {

namespace fs = std::filesystem;

// ============================================================================
// 通用 Win32 帮助
// ============================================================================

// UTF-8 -> UTF-16
std::wstring u8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// UTF-16 -> UTF-8
std::string wideToU8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                 nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// CP -> UTF-8（用于把 shell 子进程的 stdout/stderr 字节流按指定 codepage 解码）
std::string cpBytesToU8(const std::string& bytes, UINT codepage)
{
    if (bytes.empty()) return std::string();
    int n = MultiByteToWideChar(codepage, 0, bytes.data(), static_cast<int>(bytes.size()),
                                 nullptr, 0);
    if (n <= 0) return bytes;  // 解失败：原样回吐，sanitizeUtf8 再兜底
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(codepage, 0, bytes.data(), static_cast<int>(bytes.size()),
                        w.data(), n);
    return wideToU8(w);
}

std::string getEnvU8(const wchar_t* name)
{
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    if (n == 0 || n >= 32768) return std::string();
    return wideToU8(std::wstring(buf, n));
}

// ============================================================================
// 路径白名单解析 + 校验
// ============================================================================

// 路径占位符展开。返回展开后的绝对路径（无尾分隔符）；空字符串 = 此条目应跳过。
fs::path expandPlaceholder(const std::string& entry)
{
    std::string s = entry;

    auto replace = [&](const std::string& tok, const std::string& val) {
        size_t pos = s.find(tok);
        if (pos != std::string::npos) {
            if (val.empty()) { s.clear(); return; }
            s.replace(pos, tok.size(), val);
        }
    };

    // {plugin_workdir}
    if (s.find("{plugin_workdir}") != std::string::npos) {
        replace("{plugin_workdir}", pluginRootDir().string());
    }
    if (s.empty()) return {};
    // {plugin_temp}
    if (s.find("{plugin_temp}") != std::string::npos) {
        std::string tempBase = getEnvU8(L"TEMP");
        if (tempBase.empty()) tempBase = getEnvU8(L"TMP");
        if (tempBase.empty()) return {};
        // 末尾追加我们自己的子目录避免污染 TEMP
        std::string val = tempBase;
        if (!val.empty() && val.back() != '\\' && val.back() != '/') val.push_back('\\');
        val += "x64dbg-ai-plugin";
        replace("{plugin_temp}", val);
    }
    if (s.empty()) return {};
    // {debuggee_dir}
    if (s.find("{debuggee_dir}") != std::string::npos) {
        std::string mp = ProjectContext::instance().mainModulePath();
        if (mp.empty()) { s.clear(); return {}; }
        try {
            fs::path p(u8ToWide(mp));
            if (!p.has_parent_path()) return {};
            replace("{debuggee_dir}", wideToU8(p.parent_path().wstring()));
        } catch (...) { return {}; }
    }
    if (s.empty()) return {};
    // {user_home}
    if (s.find("{user_home}") != std::string::npos) {
        std::string home = getEnvU8(L"USERPROFILE");
        if (home.empty()) return {};
        replace("{user_home}", home);
    }
    if (s.empty()) return {};

    try {
        fs::path p(u8ToWide(s));
        std::error_code ec;
        fs::path abs = fs::weakly_canonical(p, ec);
        if (ec) abs = fs::absolute(p, ec);
        if (ec) return {};
        // 去尾分隔符
        std::wstring ws = abs.wstring();
        while (!ws.empty() && (ws.back() == L'\\' || ws.back() == L'/')) ws.pop_back();
        return fs::path(ws);
    } catch (...) { return {}; }
}

// 把 config.fsAllowedDirs 展开为绝对路径集合（运行时每次校验调一次，开销可忽略）。
// 留空 → 用默认集合 [{plugin_workdir}, {plugin_temp}]（按 Q4=a）。
std::vector<fs::path> resolveAllowedDirs()
{
    const auto& cfg = Config::instance().get();
    std::vector<std::string> entries = cfg.fsAllowedDirs;
    if (entries.empty()) {
        entries = { "{plugin_workdir}", "{plugin_temp}" };
    }
    std::vector<fs::path> out;
    out.reserve(entries.size());
    for (const auto& e : entries) {
        fs::path p = expandPlaceholder(e);
        if (!p.empty()) out.push_back(std::move(p));
    }
    return out;
}

// Windows 设备名（NUL/CON/PRN/AUX/COM1..9/LPT1..9）reserve 字符串
bool isReservedDeviceName(const std::wstring& name)
{
    static const std::array<std::wstring_view, 22> kDev = {
        L"CON",L"PRN",L"AUX",L"NUL",
        L"COM1",L"COM2",L"COM3",L"COM4",L"COM5",L"COM6",L"COM7",L"COM8",L"COM9",
        L"LPT1",L"LPT2",L"LPT3",L"LPT4",L"LPT5",L"LPT6",L"LPT7",L"LPT8",L"LPT9",
    };
    std::wstring upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towupper(c)); });
    // 去掉可能的扩展名
    auto dot = upper.find(L'.');
    if (dot != std::wstring::npos) upper.resize(dot);
    for (auto d : kDev) if (upper == d) return true;
    return false;
}

// 路径校验主入口。
//   userPath：LLM 传入的 UTF-8 路径（可相对/绝对）
//   mustExist：true=要求文件已存在（read 用）；false=允许新建（write/create 用）
//   outAbs：成功时填规范化后的绝对路径
//   outErr：失败时填用户可读原因
// 返回 true = 通过
bool validatePath(const std::string& userPath, bool mustExist,
                  fs::path& outAbs, std::string& outErr)
{
    if (userPath.empty()) { outErr = "'path' is empty"; return false; }
    if (userPath.size() > 1024) { outErr = "'path' too long (>1024)"; return false; }

    // 拒绝含 ".." 的原始路径（防绕过；即使解析后落在白名单也拒）
    if (userPath.find("..") != std::string::npos) {
        outErr = "'path' contains '..' which is not allowed";
        return false;
    }
    // 拒绝 UNC 前缀
    if (userPath.rfind("\\\\", 0) == 0 || userPath.rfind("//", 0) == 0) {
        outErr = "UNC paths are not allowed";
        return false;
    }
    // 拒绝设备命名空间 \\?\ 和 \\.\ 前缀
    if (userPath.find("\\\\?\\") == 0 || userPath.find("\\\\.\\") == 0) {
        outErr = "device namespace paths are not allowed";
        return false;
    }

    std::wstring wpath = u8ToWide(userPath);
    if (wpath.empty()) { outErr = "'path' not valid UTF-8"; return false; }

    // 绝对化
    fs::path inputP(wpath);
    std::error_code ec;
    fs::path absP;
    try {
        absP = fs::weakly_canonical(inputP, ec);
        if (ec) absP = fs::absolute(inputP, ec);
        if (ec || absP.empty()) {
            outErr = "failed to resolve absolute path";
            return false;
        }
    } catch (...) { outErr = "path resolve threw"; return false; }

    // 文件名设备名检查
    auto fname = absP.filename().wstring();
    if (isReservedDeviceName(fname)) {
        outErr = "filename is a reserved Windows device name";
        return false;
    }

    // 白名单前缀匹配
    auto allowed = resolveAllowedDirs();
    if (allowed.empty()) {
        outErr = "no allowed directories configured (config.fs_allowed_dirs and default both empty)";
        return false;
    }
    std::wstring absLower = absP.wstring();
    std::transform(absLower.begin(), absLower.end(), absLower.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });

    bool inside = false;
    for (const auto& dir : allowed) {
        std::wstring dl = dir.wstring();
        std::transform(dl.begin(), dl.end(), dl.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        // 完全等于 或 以 dir + sep 开头
        if (absLower == dl) { inside = true; break; }
        std::wstring prefix = dl;
        if (prefix.empty()) continue;
        if (prefix.back() != L'\\' && prefix.back() != L'/') prefix.push_back(L'\\');
        if (absLower.size() > prefix.size() && absLower.compare(0, prefix.size(), prefix) == 0) {
            inside = true; break;
        }
    }
    if (!inside) {
        outErr = "path is outside fs_allowed_dirs whitelist";
        return false;
    }

    // 存在性检查
    DWORD attrs = GetFileAttributesW(absP.wstring().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (mustExist) {
            outErr = "file does not exist";
            return false;
        }
        // 不存在但允许新建：往上找父目录看是否在白名单（必然在，因为 absP 已经过校验，
        // 父目录绝对在同一个白名单根下）
        // 但父目录可能要先 mkdir，这里不做，写工具自己创建
    } else {
        // 拒绝 reparse point（junction / symlink）
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            outErr = "path is a reparse point (symlink/junction), not allowed";
            return false;
        }
        // mustExist=true 时若为目录而非文件，read 工具应拒
        if (mustExist && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            outErr = "path is a directory, not a file";
            return false;
        }
    }

    outAbs = absP;
    return true;
}

// 把白名单展开成 ASCII 列表（给 error message 用，便于 LLM 知道允许哪里）
std::string formatAllowedDirs()
{
    auto dirs = resolveAllowedDirs();
    if (dirs.empty()) return "(none)";
    std::ostringstream os;
    for (size_t i = 0; i < dirs.size(); ++i) {
        if (i) os << " | ";
        os << wideToU8(dirs[i].wstring());
    }
    return os.str();
}

// ============================================================================
// 文件编码读取/写入
// ============================================================================

enum class FileEncoding { Utf8, Gbk, Auto };

FileEncoding parseEncoding(const std::string& s, FileEncoding dflt)
{
    if (s == "utf-8" || s == "utf8") return FileEncoding::Utf8;
    if (s == "gbk" || s == "gb18030" || s == "cp936") return FileEncoding::Gbk;
    if (s == "auto") return FileEncoding::Auto;
    return dflt;
}

// 简单的 UTF-8 校验（粗略，仅用于 Auto 模式区分）
bool looksLikeUtf8(const std::string& s)
{
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        size_t need = 0;
        if ((b & 0x80) == 0) need = 0;
        else if ((b & 0xE0) == 0xC0) need = 1;
        else if ((b & 0xF0) == 0xE0) need = 2;
        else if ((b & 0xF8) == 0xF0) need = 3;
        else return false;
        if (i + need >= n) return false;
        for (size_t k = 1; k <= need; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

// 读文件原始字节（含 size limit）
bool readFileBytes(const fs::path& path, std::size_t maxBytes,
                   std::string& outBytes, bool& outTruncated, std::uint64_t& outFullSize,
                   std::string& outErr)
{
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        outErr = "CreateFile failed (Win32 err=" + std::to_string(e) + ")";
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        outErr = "GetFileSizeEx failed";
        return false;
    }
    outFullSize = static_cast<std::uint64_t>(sz.QuadPart);
    std::size_t toRead = static_cast<std::size_t>(std::min<std::uint64_t>(maxBytes, outFullSize));
    outTruncated = (outFullSize > toRead);
    outBytes.resize(toRead);
    std::size_t off = 0;
    while (off < toRead) {
        DWORD got = 0;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(toRead - off, 1u << 20));  // 1MB chunk
        if (!ReadFile(h, outBytes.data() + off, want, &got, nullptr) || got == 0) {
            CloseHandle(h);
            outErr = "ReadFile failed at offset " + std::to_string(off);
            return false;
        }
        off += got;
    }
    CloseHandle(h);
    return true;
}

// 写文件（覆盖或新建；不存在时创建父目录）
// 注意：调用方必须已经 validatePath 通过
bool writeFileBytes(const fs::path& path, const std::string& bytes, bool createNewOnly,
                    std::string& outErr)
{
    // 创建父目录
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // ignore ec：父目录已存在不算错

    DWORD disp = createNewOnly ? CREATE_NEW : CREATE_ALWAYS;
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0,
                           nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (createNewOnly && e == ERROR_FILE_EXISTS) {
            outErr = "file already exists (use fs_write_file to overwrite)";
        } else {
            outErr = "CreateFile failed (Win32 err=" + std::to_string(e) + ")";
        }
        return false;
    }
    std::size_t off = 0;
    while (off < bytes.size()) {
        DWORD wrote = 0;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - off, 1u << 20));
        if (!WriteFile(h, bytes.data() + off, want, &wrote, nullptr) || wrote == 0) {
            CloseHandle(h);
            outErr = "WriteFile failed at offset " + std::to_string(off);
            return false;
        }
        off += wrote;
    }
    CloseHandle(h);
    return true;
}

// ============================================================================
// 工具：fs_read_file
// ============================================================================

class FsReadFileTool : public ITool {
public:
    std::string name() const override { return "fs_read_file"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    std::size_t maxResultBytes() const override { return 4 * 1024 * 1024; }  // 4MB，与 cap 一致

    std::string description() const override {
        return "Read a text file from disk. Path must be within fs_allowed_dirs whitelist "
               "(default: %APPDATA%\\x64dbg-ai-plugin\\ and %TEMP%\\x64dbg-ai-plugin\\). "
               "Returns text content decoded as utf-8/gbk/auto. Binary files are not supported "
               "(use a hex viewer tool instead). Returns: {path, size, bytes_read, truncated, encoding, content}.";
    }
    std::string descriptionZh() const override {
        return "从磁盘读取文本文件。path 必须在 fs_allowed_dirs 白名单内"
               "（默认：%APPDATA%\\x64dbg-ai-plugin\\ 与 %TEMP%\\x64dbg-ai-plugin\\）。"
               "按 utf-8/gbk/auto 解码。不支持二进制文件。"
               "返回 {path, size, bytes_read, truncated, encoding, content}。";
    }

    nlohmann::json parametersSchema() const override {
        const auto& cfg = Config::instance().get();
        return {
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"},
                               {"description", "Absolute or relative path (within fs_allowed_dirs)."}}},
                {"encoding",  {{"type", "string"},
                               {"enum", nlohmann::json::array({"utf-8", "gbk", "auto"})},
                               {"description", "Text encoding. Default 'auto' (detect UTF-8 first, fallback GBK)."}}},
                {"max_bytes", {{"type", "integer"},
                               {"description", "Max bytes to read; default "
                                + std::to_string(cfg.fsReadMaxBytesDefault)
                                + ", cap " + std::to_string(cfg.fsReadMaxBytesCap)
                                + ". Excess is truncated and truncated=true is set."}}},
            }},
            {"required", nlohmann::json::array({"path"})},
        };
    }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override {
        (void)ctx;
        ToolResult r;
        const auto& cfg = Config::instance().get();

        // path
        if (!args.contains("path") || !args["path"].is_string()) {
            r.ok = false; r.error = "'path' required (string)"; return r;
        }
        std::string userPath = args["path"].get<std::string>();
        fs::path absP;
        std::string err;
        if (!validatePath(userPath, /*mustExist*/true, absP, err)) {
            r.ok = false;
            r.error = err + " | allowed dirs: " + formatAllowedDirs();
            return r;
        }

        // encoding
        FileEncoding enc = FileEncoding::Auto;
        if (args.contains("encoding") && args["encoding"].is_string()) {
            enc = parseEncoding(args["encoding"].get<std::string>(), FileEncoding::Auto);
        }

        // max_bytes
        int maxBytes = cfg.fsReadMaxBytesDefault;
        if (args.contains("max_bytes")) {
            std::string ie;
            if (!tryGetInt32Hint(args, "max_bytes", 1, cfg.fsReadMaxBytesCap, maxBytes, ie)) {
                r.ok = false; r.error = ie; return r;
            }
        }

        // 读
        std::string bytes;
        bool truncated = false;
        std::uint64_t fullSize = 0;
        if (!readFileBytes(absP, static_cast<std::size_t>(maxBytes),
                            bytes, truncated, fullSize, err)) {
            r.ok = false; r.error = err; return r;
        }

        // 解码
        std::string text;
        const char* usedEnc = "utf-8";
        if (enc == FileEncoding::Auto) {
            if (looksLikeUtf8(bytes)) { text = bytes; usedEnc = "utf-8"; }
            else                       { text = cpBytesToU8(bytes, 936); usedEnc = "gbk"; }
        } else if (enc == FileEncoding::Utf8) {
            text = bytes; usedEnc = "utf-8";
        } else {
            text = cpBytesToU8(bytes, 936); usedEnc = "gbk";
        }
        // 防御层：sanitize 兜底（K-38）
        text = util::sanitizeUtf8(text);

        r.ok = true;
        r.data = {
            {"path",       wideToU8(absP.wstring())},
            {"size",       fullSize},
            {"bytes_read", bytes.size()},
            {"truncated",  truncated},
            {"encoding",   usedEnc},
            {"content",    text},
        };
        return r;
    }
};

// ============================================================================
// 工具：fs_write_file （覆盖或新建）
// ============================================================================

class FsWriteFileTool : public ITool {
public:
    std::string name() const override { return "fs_write_file"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::size_t maxResultBytes() const override { return 8 * 1024; }

    std::string description() const override {
        return "Write a text file to disk (creates or overwrites). Path must be within "
               "fs_allowed_dirs whitelist. content is encoded by 'encoding' (utf-8/gbk). "
               "Parent directory is created automatically if missing. "
               "Default overwrite_existing=true (set false to fail on existing file). "
               "Requires user confirm. Returns: {path, bytes_written}.";
    }
    std::string descriptionZh() const override {
        return "写磁盘文本文件（覆盖或新建）。path 必须在 fs_allowed_dirs 白名单内。"
               "content 按 encoding 编码（utf-8/gbk）。父目录自动创建。"
               "默认 overwrite_existing=true，传 false 则文件已存在时报错。"
               "需要用户 confirm。返回 {path, bytes_written}。";
    }

    nlohmann::json parametersSchema() const override {
        const auto& cfg = Config::instance().get();
        return {
            {"type", "object"},
            {"properties", {
                {"path",                {{"type", "string"},
                                         {"description", "Target path (within fs_allowed_dirs)."}}},
                {"content",             {{"type", "string"},
                                         {"description", "Text content. Max "
                                          + std::to_string(cfg.fsWriteMaxBytesCap) + " bytes after encoding."}}},
                {"encoding",            {{"type", "string"},
                                         {"enum", nlohmann::json::array({"utf-8", "gbk"})},
                                         {"description", "Encoding to write. Default 'utf-8'."}}},
                {"overwrite_existing",  {{"type", "boolean"},
                                         {"description", "Default true. Set false to fail when file exists."}}},
            }},
            {"required", nlohmann::json::array({"path", "content"})},
        };
    }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override {
        (void)ctx;
        ToolResult r;
        const auto& cfg = Config::instance().get();

        if (!args.contains("path") || !args["path"].is_string()) {
            r.ok = false; r.error = "'path' required (string)"; return r;
        }
        if (!args.contains("content") || !args["content"].is_string()) {
            r.ok = false; r.error = "'content' required (string)"; return r;
        }
        std::string userPath = args["path"].get<std::string>();
        std::string content  = args["content"].get<std::string>();

        FileEncoding enc = FileEncoding::Utf8;
        if (args.contains("encoding") && args["encoding"].is_string()) {
            enc = parseEncoding(args["encoding"].get<std::string>(), FileEncoding::Utf8);
            if (enc == FileEncoding::Auto) enc = FileEncoding::Utf8;
        }

        bool overwrite = true;
        if (args.contains("overwrite_existing") && args["overwrite_existing"].is_boolean()) {
            overwrite = args["overwrite_existing"].get<bool>();
        }

        fs::path absP;
        std::string err;
        if (!validatePath(userPath, /*mustExist*/false, absP, err)) {
            r.ok = false; r.error = err + " | allowed dirs: " + formatAllowedDirs(); return r;
        }

        // 编码
        std::string bytes;
        if (enc == FileEncoding::Utf8) {
            bytes = content;
        } else {
            // utf-8 -> gbk
            std::wstring w = u8ToWide(content);
            int n = WideCharToMultiByte(936, 0, w.data(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
            if (n <= 0) { r.ok = false; r.error = "content failed to encode as GBK"; return r; }
            bytes.resize(static_cast<size_t>(n));
            WideCharToMultiByte(936, 0, w.data(), static_cast<int>(w.size()),
                                bytes.data(), n, nullptr, nullptr);
        }

        if (bytes.size() > static_cast<std::size_t>(cfg.fsWriteMaxBytesCap)) {
            r.ok = false;
            r.error = "encoded content size " + std::to_string(bytes.size())
                    + " exceeds cap " + std::to_string(cfg.fsWriteMaxBytesCap);
            return r;
        }

        // 若不允许覆盖且已存在
        if (!overwrite) {
            DWORD attrs = GetFileAttributesW(absP.wstring().c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                r.ok = false; r.error = "file already exists (overwrite_existing=false)"; return r;
            }
        }

        if (!writeFileBytes(absP, bytes, /*createNewOnly*/false, err)) {
            r.ok = false; r.error = err; return r;
        }

        r.ok = true;
        r.data = {
            {"path",          wideToU8(absP.wstring())},
            {"bytes_written", bytes.size()},
            {"encoding",      enc == FileEncoding::Utf8 ? "utf-8" : "gbk"},
            {"overwritten",   overwrite},
        };
        return r;
    }
};

// ============================================================================
// 工具：fs_create_file （仅当不存在时创建）
// ============================================================================

class FsCreateFileTool : public ITool {
public:
    std::string name() const override { return "fs_create_file"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::size_t maxResultBytes() const override { return 8 * 1024; }

    std::string description() const override {
        return "Create a new text file (fails if file already exists). Path must be within "
               "fs_allowed_dirs whitelist. Parent directory is created automatically. "
               "Use fs_write_file if you need to overwrite. Requires user confirm. "
               "Returns: {path, bytes_written}.";
    }
    std::string descriptionZh() const override {
        return "创建新文件（文件已存在则报错）。path 必须在 fs_allowed_dirs 白名单内。"
               "父目录自动创建。需要覆盖请用 fs_write_file。需要用户 confirm。"
               "返回 {path, bytes_written}。";
    }

    nlohmann::json parametersSchema() const override {
        const auto& cfg = Config::instance().get();
        return {
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"},
                              {"description", "Target path (within fs_allowed_dirs). Must NOT exist."}}},
                {"content",  {{"type", "string"},
                              {"description", "Text content. Max "
                               + std::to_string(cfg.fsWriteMaxBytesCap) + " bytes after encoding."}}},
                {"encoding", {{"type", "string"},
                              {"enum", nlohmann::json::array({"utf-8", "gbk"})},
                              {"description", "Encoding to write. Default 'utf-8'."}}},
            }},
            {"required", nlohmann::json::array({"path", "content"})},
        };
    }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override {
        (void)ctx;
        ToolResult r;
        const auto& cfg = Config::instance().get();

        if (!args.contains("path") || !args["path"].is_string()) {
            r.ok = false; r.error = "'path' required (string)"; return r;
        }
        if (!args.contains("content") || !args["content"].is_string()) {
            r.ok = false; r.error = "'content' required (string)"; return r;
        }
        std::string userPath = args["path"].get<std::string>();
        std::string content  = args["content"].get<std::string>();

        FileEncoding enc = FileEncoding::Utf8;
        if (args.contains("encoding") && args["encoding"].is_string()) {
            enc = parseEncoding(args["encoding"].get<std::string>(), FileEncoding::Utf8);
            if (enc == FileEncoding::Auto) enc = FileEncoding::Utf8;
        }

        fs::path absP;
        std::string err;
        if (!validatePath(userPath, /*mustExist*/false, absP, err)) {
            r.ok = false; r.error = err + " | allowed dirs: " + formatAllowedDirs(); return r;
        }

        std::string bytes;
        if (enc == FileEncoding::Utf8) {
            bytes = content;
        } else {
            std::wstring w = u8ToWide(content);
            int n = WideCharToMultiByte(936, 0, w.data(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
            if (n <= 0) { r.ok = false; r.error = "content failed to encode as GBK"; return r; }
            bytes.resize(static_cast<size_t>(n));
            WideCharToMultiByte(936, 0, w.data(), static_cast<int>(w.size()),
                                bytes.data(), n, nullptr, nullptr);
        }
        if (bytes.size() > static_cast<std::size_t>(cfg.fsWriteMaxBytesCap)) {
            r.ok = false;
            r.error = "encoded content size " + std::to_string(bytes.size())
                    + " exceeds cap " + std::to_string(cfg.fsWriteMaxBytesCap);
            return r;
        }

        if (!writeFileBytes(absP, bytes, /*createNewOnly*/true, err)) {
            r.ok = false; r.error = err; return r;
        }

        r.ok = true;
        r.data = {
            {"path",          wideToU8(absP.wstring())},
            {"bytes_written", bytes.size()},
            {"encoding",      enc == FileEncoding::Utf8 ? "utf-8" : "gbk"},
        };
        return r;
    }
};

// ============================================================================
// Shell 执行核心
// ============================================================================

// 探测 pwsh.exe 优先，找不到 fallback powershell.exe（Q3=c）。
// 启动时仅查一次，缓存结果。
std::wstring& pwshExePathCached()
{
    static std::wstring s_path;
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        // 1) PATH 上找 pwsh.exe
        wchar_t buf[MAX_PATH] = {};
        DWORD n = SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr);
        if (n > 0 && n < MAX_PATH) { s_path = buf; return; }
        // 2) ProgramFiles\PowerShell\7\pwsh.exe
        std::string pf = getEnvU8(L"ProgramFiles");
        if (!pf.empty()) {
            fs::path p = fs::path(u8ToWide(pf)) / L"PowerShell" / L"7" / L"pwsh.exe";
            if (fs::exists(p)) { s_path = p.wstring(); return; }
        }
        // 3) fallback Windows PowerShell 5
        n = SearchPathW(nullptr, L"powershell.exe", nullptr, MAX_PATH, buf, nullptr);
        if (n > 0 && n < MAX_PATH) { s_path = buf; return; }
        // 4) System32\WindowsPowerShell\v1.0\powershell.exe
        std::string sys = getEnvU8(L"SystemRoot");
        if (!sys.empty()) {
            fs::path p = fs::path(u8ToWide(sys)) / L"System32" / L"WindowsPowerShell"
                       / L"v1.0" / L"powershell.exe";
            if (fs::exists(p)) { s_path = p.wstring(); return; }
        }
        // 啥都没找到
    });
    return s_path;
}

std::wstring cmdExePathCached()
{
    static std::wstring s_path;
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        wchar_t buf[MAX_PATH] = {};
        DWORD n = SearchPathW(nullptr, L"cmd.exe", nullptr, MAX_PATH, buf, nullptr);
        if (n > 0 && n < MAX_PATH) { s_path = buf; return; }
        std::string sys = getEnvU8(L"SystemRoot");
        if (!sys.empty()) {
            fs::path p = fs::path(u8ToWide(sys)) / L"System32" / L"cmd.exe";
            if (fs::exists(p)) s_path = p.wstring();
        }
    });
    return s_path;
}

struct ShellRunResult {
    int  exitCode = 0;
    std::string stdoutText;
    std::string stderrText;
    bool stdoutTruncated = false;
    bool stderrTruncated = false;
    bool killedByTimeout = false;
    std::uint64_t pid = 0;
    long long elapsedMs = 0;
    std::string startError;  // 启动失败时填这里
};

// 后台读 pipe 线程：累积上限 capBytes，超出丢弃但 truncated=true
void readPipeToBuffer(HANDLE pipe, std::string& out, std::size_t capBytes, bool& truncated)
{
    char buf[4096];
    DWORD got = 0;
    while (true) {
        BOOL ok = ReadFile(pipe, buf, sizeof(buf), &got, nullptr);
        if (!ok || got == 0) break;
        if (out.size() + got <= capBytes) {
            out.append(buf, got);
        } else if (out.size() < capBytes) {
            out.append(buf, capBytes - out.size());
            truncated = true;
        } else {
            truncated = true;
        }
    }
}

// 核心 shell runner。
//   - exePath：cmd.exe / powershell.exe / pwsh.exe 绝对路径
//   - argsAfterExe：传给 exe 的参数（含 /c 或 -Command 等）；命令行整体由 CreateProcessW 拼
//   - cwd：工作目录绝对路径
//   - timeoutMs：硬超时
//   - stdoutCap / stderrCap：捕获上限
//   - outputCodepage：解码 stdout/stderr 字节流用的 codepage（cmd=936; pwsh=65001）
//   - friendlyToolName：发 EventBus 事件用
//   - friendlyCmdLine：发 EventBus 事件用 + audit
// 返回：result
ShellRunResult runShell(const std::wstring& exePath,
                        const std::wstring& argsAfterExe,
                        const std::wstring& cwd,
                        std::uint32_t timeoutMs,
                        std::size_t stdoutCap, std::size_t stderrCap,
                        UINT outputCodepage,
                        const std::string& friendlyToolName,
                        const std::string& friendlyCmdLine)
{
    ShellRunResult res;
    auto t0 = std::chrono::steady_clock::now();

    if (exePath.empty()) { res.startError = "shell executable not found on this system"; return res; }

    // Pipes：stdout/stderr 各一对（child 写端 inheritable）
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0)) {
        res.startError = "CreatePipe(stdout) failed"; return res;
    }
    if (!CreatePipe(&errR, &errW, &sa, 0)) {
        CloseHandle(outR); CloseHandle(outW);
        res.startError = "CreatePipe(stderr) failed"; return res;
    }
    // 父端不被 child 继承
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    // 命令行
    std::wstring cmdLine;
    cmdLine.reserve(exePath.size() + argsAfterExe.size() + 4);
    cmdLine.push_back(L'"'); cmdLine += exePath; cmdLine.push_back(L'"');
    if (!argsAfterExe.empty()) { cmdLine.push_back(L' '); cmdLine += argsAfterExe; }
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    // Job 包子进程（防 pwsh 起子进程逃跑超时）
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji{};
        ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &ji, sizeof(ji));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);  // 不交互；child 一读 stdin 即 EOF
    si.hStdOutput = outW;
    si.hStdError  = errW;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
                              nullptr,
                              cwd.empty() ? nullptr : cwd.c_str(),
                              &si, &pi);
    if (!ok) {
        DWORD e = GetLastError();
        CloseHandle(outR); CloseHandle(outW);
        CloseHandle(errR); CloseHandle(errW);
        if (hJob) CloseHandle(hJob);
        res.startError = "CreateProcess failed (Win32 err=" + std::to_string(e) + ")";
        return res;
    }

    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);
    res.pid = pi.dwProcessId;

    // 发 ShellStarted 事件
    {
        DbgEventPayload p{};
        p.kind = DbgEvent::ShellStarted;
        p.addr = res.pid;
        p.raw  = const_cast<std::string*>(&friendlyCmdLine);
        EventBus::instance().publish(DbgEvent::ShellStarted, p);
    }
    XAI_LOG_INFO("{}: started pid={} cmd={}", friendlyToolName, res.pid, friendlyCmdLine);

    // 父端关闭 child 写端，否则 ReadFile 不会 EOF
    CloseHandle(outW);
    CloseHandle(errW);

    // 收 stdout / stderr 独立线程
    std::string outBytes, errBytes;
    std::thread tOut(readPipeToBuffer, outR, std::ref(outBytes), stdoutCap, std::ref(res.stdoutTruncated));
    std::thread tErr(readPipeToBuffer, errR, std::ref(errBytes), stderrCap, std::ref(res.stderrTruncated));

    // 等待
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        res.killedByTimeout = true;
        if (hJob) TerminateJobObject(hJob, 1);
        else      TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    // 线程清理（pipe 关了它们就 EOF）
    if (tOut.joinable()) tOut.join();
    if (tErr.joinable()) tErr.join();

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    res.exitCode = static_cast<int>(exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outR);
    CloseHandle(errR);
    if (hJob) CloseHandle(hJob);

    auto t1 = std::chrono::steady_clock::now();
    res.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // 解码
    res.stdoutText = util::sanitizeUtf8(cpBytesToU8(outBytes, outputCodepage));
    res.stderrText = util::sanitizeUtf8(cpBytesToU8(errBytes, outputCodepage));

    // 发 ShellFinished / ShellTimeout
    {
        DbgEventPayload p{};
        p.kind = res.killedByTimeout ? DbgEvent::ShellTimeout : DbgEvent::ShellFinished;
        p.addr = res.pid;
        p.raw  = const_cast<std::string*>(&friendlyCmdLine);
        EventBus::instance().publish(p.kind, p);
    }
    XAI_LOG_INFO("{}: pid={} exit={} elapsed={}ms killed_by_timeout={}",
                 friendlyToolName, res.pid, res.exitCode, res.elapsedMs, res.killedByTimeout);

    return res;
}

// 通用：解析 shell_* 参数，校验 cwd
bool parseShellCommonArgs(const nlohmann::json& args, const AppConfig& cfg,
                          std::string& outCmdLine,
                          fs::path&    outCwdAbs,        // 空 = 用默认
                          int&         outTimeoutMs,
                          int&         outStdoutCap,
                          int&         outStderrCap,
                          std::string& outErr)
{
    if (!args.contains("cmd_line") || !args["cmd_line"].is_string()) {
        outErr = "'cmd_line' required (string)"; return false;
    }
    outCmdLine = args["cmd_line"].get<std::string>();
    if (outCmdLine.empty()) { outErr = "'cmd_line' is empty"; return false; }
    if (outCmdLine.size() > 8192) { outErr = "'cmd_line' too long (>8192)"; return false; }

    outTimeoutMs = cfg.shellTimeoutMsDefault;
    if (args.contains("timeout_ms")) {
        std::string ie;
        if (!tryGetInt32Hint(args, "timeout_ms", 100, cfg.shellTimeoutMsCap, outTimeoutMs, ie)) {
            outErr = ie; return false;
        }
    }
    outStdoutCap = cfg.shellStdoutMaxBytesDefault;
    if (args.contains("max_stdout_bytes")) {
        std::string ie;
        if (!tryGetInt32Hint(args, "max_stdout_bytes", 1024, cfg.shellStdoutMaxBytesCap, outStdoutCap, ie)) {
            outErr = ie; return false;
        }
    }
    outStderrCap = cfg.shellStderrMaxBytesDefault;
    if (args.contains("max_stderr_bytes")) {
        std::string ie;
        if (!tryGetInt32Hint(args, "max_stderr_bytes", 1024, cfg.shellStderrMaxBytesCap, outStderrCap, ie)) {
            outErr = ie; return false;
        }
    }

    outCwdAbs.clear();
    if (args.contains("cwd") && args["cwd"].is_string()) {
        std::string cwdU = args["cwd"].get<std::string>();
        if (!cwdU.empty()) {
            // cwd 必须是已存在的目录 + 在白名单内
            std::string ce;
            if (!validatePath(cwdU, /*mustExist*/true, outCwdAbs, ce)) {
                outErr = "'cwd' rejected: " + ce + " | allowed dirs: " + formatAllowedDirs();
                return false;
            }
            DWORD attrs = GetFileAttributesW(outCwdAbs.wstring().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                outErr = "'cwd' is not a directory";
                return false;
            }
        }
    }
    return true;
}

// ============================================================================
// 工具：shell_cmd  ( cmd.exe /c <cmd_line> )
// ============================================================================

class ShellCmdTool : public ITool {
public:
    std::string name() const override { return "shell_cmd"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::size_t maxResultBytes() const override { return 2 * 1024 * 1024; }

    std::string description() const override {
        return "Run a command line via cmd.exe /c. Command is unrestricted (any builtin or exe). "
               "Always requires user confirm (cannot be auto-approved). "
               "Optional cwd must be within fs_allowed_dirs. Output decoded as system ANSI (e.g. GBK on zh-CN). "
               "Returns: {exit_code, stdout, stderr, stdout_truncated, stderr_truncated, killed_by_timeout, elapsed_ms, pid}.";
    }
    std::string descriptionZh() const override {
        return "通过 cmd.exe /c 执行命令行。命令不受限制（任意内置/可执行）。"
               "**必须**用户确认（不可被 auto-approve 豁免）。"
               "可选 cwd 必须在 fs_allowed_dirs 白名单内。输出按系统 ANSI（中文机 = GBK）解码。"
               "返回 {exit_code, stdout, stderr, stdout_truncated, stderr_truncated, killed_by_timeout, elapsed_ms, pid}。";
    }

    nlohmann::json parametersSchema() const override {
        const auto& cfg = Config::instance().get();
        return {
            {"type", "object"},
            {"properties", {
                {"cmd_line",         {{"type", "string"},
                                      {"description", "Full cmd.exe command line (everything after '/c'). "
                                                      "Examples: 'dir C:\\Windows\\System32 /b', "
                                                      "'where python', 'git log --oneline -5'."}}},
                {"cwd",              {{"type", "string"},
                                      {"description", "Working directory (must be within fs_allowed_dirs). "
                                                      "Default: insufficient guess; recommend always set explicitly."}}},
                {"timeout_ms",       {{"type", "integer"},
                                      {"description", "Hard timeout; default "
                                       + std::to_string(cfg.shellTimeoutMsDefault)
                                       + "ms, cap " + std::to_string(cfg.shellTimeoutMsCap) + "ms."}}},
                {"max_stdout_bytes", {{"type", "integer"},
                                      {"description", "Stdout capture cap; default "
                                       + std::to_string(cfg.shellStdoutMaxBytesDefault)
                                       + ", cap " + std::to_string(cfg.shellStdoutMaxBytesCap) + "."}}},
                {"max_stderr_bytes", {{"type", "integer"},
                                      {"description", "Stderr capture cap; default "
                                       + std::to_string(cfg.shellStderrMaxBytesDefault)
                                       + ", cap " + std::to_string(cfg.shellStderrMaxBytesCap) + "."}}},
            }},
            {"required", nlohmann::json::array({"cmd_line"})},
        };
    }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override {
        (void)ctx;
        ToolResult r;
        const auto& cfg = Config::instance().get();

        std::string cmdLine, err;
        fs::path cwdAbs;
        int timeoutMs = 0, stdoutCap = 0, stderrCap = 0;
        if (!parseShellCommonArgs(args, cfg, cmdLine, cwdAbs, timeoutMs, stdoutCap, stderrCap, err)) {
            r.ok = false; r.error = err; return r;
        }

        std::wstring exe = cmdExePathCached();
        if (exe.empty()) { r.ok = false; r.error = "cmd.exe not found"; return r; }

        // /c "<cmd_line>"
        std::wstring argsW = L"/c " + u8ToWide(cmdLine);

        std::string friendly = "cmd.exe /c " + cmdLine;
        ShellRunResult sr = runShell(exe, argsW,
                                      cwdAbs.empty() ? std::wstring() : cwdAbs.wstring(),
                                      static_cast<std::uint32_t>(timeoutMs),
                                      static_cast<std::size_t>(stdoutCap),
                                      static_cast<std::size_t>(stderrCap),
                                      /*output cp = ACP 936 for zh-CN; 1252 otherwise. */
                                      GetACP(),
                                      name(), friendly);

        if (!sr.startError.empty()) {
            r.ok = false; r.error = sr.startError; return r;
        }
        r.ok = true;
        r.data = {
            {"exit_code",         sr.exitCode},
            {"stdout",            sr.stdoutText},
            {"stderr",            sr.stderrText},
            {"stdout_truncated",  sr.stdoutTruncated},
            {"stderr_truncated",  sr.stderrTruncated},
            {"killed_by_timeout", sr.killedByTimeout},
            {"elapsed_ms",        sr.elapsedMs},
            {"pid",               sr.pid},
        };
        return r;
    }
};

// ============================================================================
// 工具：shell_pwsh  ( pwsh.exe / powershell.exe -NoProfile -NonInteractive
//                    -Command "[Console]::OutputEncoding=...; & { <cmd_line> }" )
// ============================================================================

class ShellPwshTool : public ITool {
public:
    std::string name() const override { return "shell_pwsh"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::size_t maxResultBytes() const override { return 2 * 1024 * 1024; }

    std::string description() const override {
        return "Run a command via PowerShell (pwsh.exe preferred, fallback powershell.exe). "
               "Command is unrestricted. Always requires user confirm (cannot be auto-approved). "
               "Optional cwd must be within fs_allowed_dirs. Output decoded as UTF-8 "
               "(tool injects [Console]::OutputEncoding before your command). "
               "Returns: {exit_code, stdout, stderr, stdout_truncated, stderr_truncated, "
               "killed_by_timeout, elapsed_ms, pid, runner}.";
    }
    std::string descriptionZh() const override {
        return "通过 PowerShell（优先 pwsh.exe，fallback powershell.exe）执行命令。命令不受限制。"
               "**必须**用户确认。可选 cwd 必须在 fs_allowed_dirs 内。输出按 UTF-8 解码"
               "（工具会先注入 [Console]::OutputEncoding）。"
               "返回 {exit_code, stdout, stderr, stdout_truncated, stderr_truncated, "
               "killed_by_timeout, elapsed_ms, pid, runner}。";
    }

    nlohmann::json parametersSchema() const override {
        const auto& cfg = Config::instance().get();
        return {
            {"type", "object"},
            {"properties", {
                {"cmd_line",         {{"type", "string"},
                                      {"description", "PowerShell expression. Examples: "
                                                      "'Get-Process | Select -First 5', "
                                                      "'Test-Path C:\\foo', "
                                                      "'(Invoke-WebRequest -Uri https://x).Content'."}}},
                {"cwd",              {{"type", "string"},
                                      {"description", "Working directory (must be within fs_allowed_dirs)."}}},
                {"timeout_ms",       {{"type", "integer"},
                                      {"description", "Hard timeout; default "
                                       + std::to_string(cfg.shellTimeoutMsDefault)
                                       + "ms, cap " + std::to_string(cfg.shellTimeoutMsCap) + "ms."}}},
                {"max_stdout_bytes", {{"type", "integer"},
                                      {"description", "Default "
                                       + std::to_string(cfg.shellStdoutMaxBytesDefault)
                                       + ", cap " + std::to_string(cfg.shellStdoutMaxBytesCap) + "."}}},
                {"max_stderr_bytes", {{"type", "integer"},
                                      {"description", "Default "
                                       + std::to_string(cfg.shellStderrMaxBytesDefault)
                                       + ", cap " + std::to_string(cfg.shellStderrMaxBytesCap) + "."}}},
            }},
            {"required", nlohmann::json::array({"cmd_line"})},
        };
    }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override {
        (void)ctx;
        ToolResult r;
        const auto& cfg = Config::instance().get();

        std::string cmdLine, err;
        fs::path cwdAbs;
        int timeoutMs = 0, stdoutCap = 0, stderrCap = 0;
        if (!parseShellCommonArgs(args, cfg, cmdLine, cwdAbs, timeoutMs, stdoutCap, stderrCap, err)) {
            r.ok = false; r.error = err; return r;
        }

        std::wstring exe = pwshExePathCached();
        if (exe.empty()) { r.ok = false; r.error = "neither pwsh.exe nor powershell.exe found"; return r; }

        // 判定 runner（看 exe 文件名）
        bool isPwsh7 = false;
        {
            std::wstring fn = fs::path(exe).filename().wstring();
            std::transform(fn.begin(), fn.end(), fn.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
            isPwsh7 = (fn == L"pwsh.exe");
        }
        std::string runnerName = isPwsh7 ? "pwsh" : "powershell";

        // 注入 UTF-8 输出编码，然后跑用户命令
        // & { ... } 把用户命令包成 script block 避免特殊字符干扰
        std::string injected =
            "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new(); "
            "$OutputEncoding=[System.Text.UTF8Encoding]::new(); "
            "& { " + cmdLine + " }";

        std::wstring argsW = L"-NoProfile -NonInteractive -OutputFormat Text -Command \""
                            + u8ToWide(injected) + L"\"";

        std::string friendly = runnerName + " -Command " + cmdLine;
        ShellRunResult sr = runShell(exe, argsW,
                                      cwdAbs.empty() ? std::wstring() : cwdAbs.wstring(),
                                      static_cast<std::uint32_t>(timeoutMs),
                                      static_cast<std::size_t>(stdoutCap),
                                      static_cast<std::size_t>(stderrCap),
                                      /* output cp */ CP_UTF8,
                                      name(), friendly);

        if (!sr.startError.empty()) {
            r.ok = false; r.error = sr.startError; return r;
        }
        r.ok = true;
        r.data = {
            {"exit_code",         sr.exitCode},
            {"stdout",            sr.stdoutText},
            {"stderr",            sr.stderrText},
            {"stdout_truncated",  sr.stdoutTruncated},
            {"stderr_truncated",  sr.stderrTruncated},
            {"killed_by_timeout", sr.killedByTimeout},
            {"elapsed_ms",        sr.elapsedMs},
            {"pid",               sr.pid},
            {"runner",            runnerName},
        };
        return r;
    }
};

}  // namespace

// ============================================================================
// 注册（在 builtin_tools.h 声明、tool_registry.cpp 调用）
// ============================================================================
void registerSystemTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<FsReadFileTool>(),    "system");
    reg.registerTool(std::make_unique<FsWriteFileTool>(),   "system");
    reg.registerTool(std::make_unique<FsCreateFileTool>(),  "system");
    reg.registerTool(std::make_unique<ShellCmdTool>(),      "system");
    reg.registerTool(std::make_unique<ShellPwshTool>(),     "system");
}

}  // namespace x64ai
