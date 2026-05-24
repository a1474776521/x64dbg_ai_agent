// ai/tools/script_tools.cpp
//
// S5：x64dbg 脚本工具三件套。
//
//   list_scripts        枚举 %APPDATA%\x64dbg-ai-plugin\scripts\ 下 *.txt / *.script
//   load_script         加载脚本到 x64dbg 的 Script 标签页，不执行
//   run_script_file     加载并触发 Run（异步，立即返回；脚本结束无法同步等待）
//
// 设计取舍：
//   - script 子系统是 x64dbg GUI 的功能，没暴露"执行完成"事件，
//     run_script_file 只能 fire-and-forget。agent 想确认结果应该用
//     wait_for_event(Paused/Breakpoint) 或后续手动查 register/memory。
//   - 路径解析：相对路径 → pluginScriptsDir() 拼接；绝对路径直接用。
//     绝对路径 confirm 弹窗时用户能看到完整路径，自行判断风险。
//   - load_script / run_script_file 都 category=Write（脚本能下断、改寄存器、
//     改内存，威胁面 = patch_memory + set_register 的并集）。
//   - list_scripts category=Read（只列文件名 + size + mtime，不读内容）。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <Windows.h>
#include "bridgemain.h"

#include "util/logging.h"
#include "util/paths.h"

namespace x64ai {

namespace fs = std::filesystem;

namespace {

bool hasScriptExt(const fs::path& p)
{
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return ext == ".txt" || ext == ".script";
}

// 路径解析：相对路径接 pluginScriptsDir/；绝对路径直接用。
// 返回 weakly canonical（不要求文件存在）以消除 .. 与 . 段。
// 若 normalized 结果还指向 scripts 目录之外但用户是 explicit 绝对路径，仍允许。
fs::path resolveScriptPath(const std::string& raw)
{
    fs::path p(raw);
    if (p.is_relative()) p = pluginScriptsDir() / p;
    std::error_code ec;
    auto norm = fs::weakly_canonical(p, ec);
    if (ec) norm = p.lexically_normal();
    return norm;
}

// 把文件 mtime 转 UNIX epoch ms（用 Win32 API 绕过 file_clock→system_clock 兼容坑）
std::int64_t fileMtimeMs(const fs::path& p)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    // FILETIME = 100ns 单位、起点 1601-01-01；UNIX epoch 是 1970-01-01
    // 偏移 = 11644473600 秒 = 116444736000000000 个 100ns
    ULARGE_INTEGER u;
    u.LowPart  = data.ftLastWriteTime.dwLowDateTime;
    u.HighPart = data.ftLastWriteTime.dwHighDateTime;
    constexpr std::uint64_t EPOCH_100NS = 116444736000000000ULL;
    if (u.QuadPart < EPOCH_100NS) return 0;
    return static_cast<std::int64_t>((u.QuadPart - EPOCH_100NS) / 10000ULL);  // → ms
}

}  // namespace

// ============= W-3 list_scripts（注意：只读）=============
class ListScriptsTool : public ITool {
public:
    std::string name() const override { return "list_scripts"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    std::string description() const override
    {
        return "List script files available in %APPDATA%\\x64dbg-ai-plugin\\scripts\\. "
               "Returns name / size_bytes / mtime_ms for each *.txt / *.script entry. "
               "Use load_script(path) to inspect contents (note: load opens the Script tab; "
               "you can read file contents with read_string or via the host file system).";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
        };
    }
    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        const auto dir = pluginScriptsDir();
        nlohmann::json arr = nlohmann::json::array();
        std::error_code ec;
        for (auto it = fs::directory_iterator(dir, ec);
             !ec && it != fs::directory_iterator();
             it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            if (!hasScriptExt(it->path()))  continue;
            const auto sz = it->file_size(ec);
            arr.push_back({
                {"name",       it->path().filename().string()},
                {"size_bytes", ec ? 0 : static_cast<std::uint64_t>(sz)},
                {"mtime_ms",   fileMtimeMs(it->path())},
            });
        }
        r.ok = true;
        r.data = {
            {"dir",     dir.string()},
            {"count",   arr.size()},
            {"scripts", std::move(arr)},
        };
        return r;
    }
};

// ============= W-1 load_script =============
class LoadScriptTool : public ITool {
public:
    std::string name() const override { return "load_script"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Load a script into the x64dbg Script tab WITHOUT running it. "
               "path: relative -> resolved against %APPDATA%\\x64dbg-ai-plugin\\scripts\\, "
               "absolute -> used as-is. Caller should preview the script via the file system "
               "before loading. Use run_script_file to also execute.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"},
                          {"description", "Script file path; *.txt or *.script"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        if (!args.contains("path") || !args["path"].is_string()) {
            r.ok = false; r.error = "'path' required (string)"; return r;
        }
        const auto raw = args["path"].get<std::string>();
        const auto resolved = resolveScriptPath(raw);
        if (!hasScriptExt(resolved)) {
            r.ok = false;
            r.error = "extension must be .txt or .script: " + resolved.string();
            return r;
        }
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec)) {
            r.ok = false; r.error = "file not found: " + resolved.string(); return r;
        }
        const auto sz = fs::file_size(resolved, ec);
        if (!ec && sz > 1 * 1024 * 1024) {  // 1 MB 上限
            r.ok = false;
            r.error = "script too large (" + std::to_string(sz) + " bytes > 1 MiB)";
            return r;
        }
        // DbgScriptUnload + DbgScriptLoad 是同步的（UI 切到 Script tab）
        DbgScriptUnload();
        DbgScriptLoad(resolved.string().c_str());
        XAI_LOG_INFO("load_script: loaded {} ({} bytes)",
                     resolved.string().c_str(),
                     ec ? 0 : static_cast<std::uint64_t>(sz));
        r.ok = true;
        r.data = {
            {"path",       resolved.string()},
            {"size_bytes", ec ? 0 : static_cast<std::uint64_t>(sz)},
            {"loaded",     true},
            {"note",       "loaded into Script tab; not executed. Use run_script_file to execute."},
        };
        return r;
    }
};

// ============= W-2 run_script_file =============
class RunScriptFileTool : public ITool {
public:
    std::string name() const override { return "run_script_file"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Load and EXECUTE a script in the x64dbg Script tab (fire-and-forget). "
               "x64dbg's script engine does not expose a 'finished' event; this tool returns "
               "immediately after triggering Run. To observe results, use wait_for_event "
               "(Paused/Breakpoint) or follow up with read_memory/get_registers. "
               "path resolution and size limit are identical to load_script.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"},
                          {"description", "Script file path; *.txt or *.script"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        if (!args.contains("path") || !args["path"].is_string()) {
            r.ok = false; r.error = "'path' required (string)"; return r;
        }
        const auto raw = args["path"].get<std::string>();
        const auto resolved = resolveScriptPath(raw);
        if (!hasScriptExt(resolved)) {
            r.ok = false;
            r.error = "extension must be .txt or .script: " + resolved.string();
            return r;
        }
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec)) {
            r.ok = false; r.error = "file not found: " + resolved.string(); return r;
        }
        const auto sz = fs::file_size(resolved, ec);
        if (!ec && sz > 1 * 1024 * 1024) {
            r.ok = false;
            r.error = "script too large (" + std::to_string(sz) + " bytes > 1 MiB)";
            return r;
        }
        DbgScriptUnload();
        DbgScriptLoad(resolved.string().c_str());
        // destline = 0 → 从第一行开始跑到 ret / end
        DbgScriptRun(0);
        XAI_LOG_INFO("run_script_file: triggered {} ({} bytes)",
                     resolved.string().c_str(),
                     ec ? 0 : static_cast<std::uint64_t>(sz));
        r.ok = true;
        r.data = {
            {"path",       resolved.string()},
            {"size_bytes", ec ? 0 : static_cast<std::uint64_t>(sz)},
            {"started",    true},
            {"note",       "fire-and-forget; use wait_for_event or follow-up tools to observe."},
        };
        return r;
    }
};

void registerScriptTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<ListScriptsTool>());
    reg.registerTool(std::make_unique<LoadScriptTool>());
    reg.registerTool(std::make_unique<RunScriptFileTool>());
}

}  // namespace x64ai
