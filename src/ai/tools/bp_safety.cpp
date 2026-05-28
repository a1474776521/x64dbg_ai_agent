// ai/tools/bp_safety.cpp
//
// 见 bp_safety.h 头注释。
#include "ai/tools/bp_safety.h"

#include <algorithm>
#include <cctype>
#include <mutex>

#include <Windows.h>
#include "bridgemain.h"

#include "util/config.h"
#include "util/logging.h"

namespace x64ai {

namespace {

inline std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 去掉 ".dll" / ".sys" 等扩展。
inline std::string stripExtLower(std::string s)
{
    s = toLower(std::move(s));
    auto pos = s.rfind('.');
    if (pos != std::string::npos) s.resize(pos);
    return s;
}

}  // namespace

// ============= 系统模块名单 =============
const std::unordered_set<std::string>& bpSysModules()
{
    static const std::unordered_set<std::string> kSysMods = {
        "ntdll", "kernel32", "kernelbase", "user32", "gdi32", "advapi32",
        "ws2_32", "wininet", "shell32", "ole32", "oleaut32", "msvcrt",
        "win32u", "rpcrt4", "combase", "shlwapi", "msvcp_win", "ucrtbase",
        "sechost", "bcrypt", "bcryptprimitives", "cryptbase", "imm32",
    };
    return kSysMods;
}

bool isSystemModule(std::string modName)
{
    return bpSysModules().count(stripExtLower(std::move(modName))) > 0;
}

// ============= 高频 API 名单 =============
const std::unordered_set<std::string>& bpHotApis()
{
    static const std::unordered_set<std::string> kHotApis = {
        // 模块/符号
        "loadlibrarya", "loadlibraryw", "loadlibraryexa", "loadlibraryexw",
        "getprocaddress", "getmodulehandlea", "getmodulehandlew",
        "getmodulefilenamea", "getmodulefilenamew",
        // 内存
        "virtualalloc", "virtualallocex", "virtualfree", "virtualprotect",
        "virtualquery", "heapalloc", "heapfree", "rtlallocateheap", "rtlfreeheap",
        // 文件 I/O
        "createfilea", "createfilew", "readfile", "writefile", "closehandle",
        "ntcreatefile", "ntreadfile", "ntwritefile", "ntclose",
        // 注册表
        "regopenkeyexa", "regopenkeyexw", "regqueryvalueexa", "regqueryvalueexw",
        // CRT 字符串
        "strlen", "strcmp", "strcpy", "memcpy", "memset", "memcmp",
        "lstrlena", "lstrlenw", "lstrcmpa", "lstrcmpw",
        // 同步原语
        "entercriticalsection", "leavecriticalsection",
        "waitforsingleobject", "waitformultipleobjects",
        "ntwaitforsingleobject", "sleep", "sleepex",
        // GUI 消息泵
        "peekmessagea", "peekmessagew", "getmessagea", "getmessagew",
        "dispatchmessagea", "dispatchmessagew", "translatemessage",
        "sendmessagea", "sendmessagew", "postmessagea", "postmessagew",
        // 时间/性能
        "queryperformancecounter", "gettickcount", "gettickcount64",
        "getsystemtimeasfiletime", "ntquerysysteminformation",
    };
    return kHotApis;
}

bool isHighFreqApi(std::string apiName)
{
    return bpHotApis().count(toLower(std::move(apiName))) > 0;
}

// ============= classifyBpAddr =============
HotSpotInfo classifyBpAddr(std::uint64_t va)
{
    HotSpotInfo info;
    char modBuf[MAX_MODULE_SIZE] = {0};
    if (!DbgGetModuleAt(static_cast<duint>(va), modBuf) || !modBuf[0]) {
        return info;
    }
    info.moduleName = modBuf;

    char labelBuf[MAX_LABEL_SIZE] = {0};
    DbgGetLabelAt(static_cast<duint>(va), SEG_DEFAULT, labelBuf);
    info.symbolName = labelBuf;

    info.sysModule = isSystemModule(info.moduleName);
    info.hotApi    = isHighFreqApi(info.symbolName);
    info.dangerous = info.sysModule && info.hotApi;
    return info;
}

// ============= run_dbg_command 白名单 =============
const std::unordered_set<std::string>& dbgCmdWhitelistDefaults()
{
    static const std::unordered_set<std::string> kDefaults = {
        // 断点
        "bp", "bpc", "bphwc", "bpd", "bpe",
        // 执行控制
        "run", "stepinto", "stepover", "stepout", "pause",
        // 数据读取
        "db", "dw", "dd", "dq",
    };
    return kDefaults;
}

const std::unordered_set<std::string>& dbgCmdWhitelist()
{
    // 合并集只构建一次；Config::reload() 后想刷新需要重启插件
    // （UI 层会强提示）。这里追求稳：避免每次调用都重建。
    static std::unordered_set<std::string> merged;
    static std::once_flag once;
    std::call_once(once, []{
        merged = dbgCmdWhitelistDefaults();
        const auto& extra = Config::instance().get().extraDbgCmdWhitelist;
        for (const auto& s : extra) {
            merged.insert(s);  // Config 已转小写
        }
        if (!extra.empty()) {
            XAI_LOG_INFO("dbgCmdWhitelist: merged {} extra entries from config (total {})",
                         extra.size(), merged.size());
        }
    });
    return merged;
}

}  // namespace x64ai
