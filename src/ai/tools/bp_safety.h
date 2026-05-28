// ai/tools/bp_safety.h
//
// K-30 / K-31 的"断点安全护栏"共享头。
//
// 历史：debug_write_tools.cpp（软件断点）和 advanced_bp_tools.cpp（硬件断点）
// 一开始各自维护一套 isSystemModule / isHighFreqApi 副本（两份相同）。
// 实测中需要把这些名单也展示在 UI"安全护栏"窗口里，借机把重复抽到这里。
//
// 设计原则：
//   - 名单本体作为函数级 static 集合存在，不做 const extern（避免 ODR/初始化顺序问题）；
//   - 暴露 const & 访问器供 UI 遍历；
//   - 提供 isSystemModule / isHighFreqApi / classifyBpAddr 给断点工具复用；
//   - dbgCmdWhitelist 同样在这里集中管理（默认硬集 + Config::extraDbgCmdWhitelist 合并）。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace x64ai {

// ============= 系统模块 + 高频 API 名单（断点防卡死） =============

// 系统模块名（不含扩展名，小写）。例：ntdll / kernel32 / user32 / ...
const std::unordered_set<std::string>& bpSysModules();

// 高频 API 名（小写、无模块前缀）。例：loadlibrarya / virtualalloc / ...
const std::unordered_set<std::string>& bpHotApis();

// 谓词：传入的字符串若是系统模块名（可带扩展名，大小写不敏感）返回 true。
bool isSystemModule(std::string modName);

// 谓词：传入的字符串若在高频 API 名单内（大小写不敏感）返回 true。
bool isHighFreqApi(std::string apiName);

// 分类结果。dangerous=true 表示该 VA 命中"系统模块∧热 API"，
// 软件断点 / 硬件执行断点应拒绝以免冻结系统。
struct HotSpotInfo {
    bool dangerous = false;
    std::string moduleName;   // 反查到的模块名（含扩展名）
    std::string symbolName;   // 反查到的最近符号（可空）
    bool sysModule = false;
    bool hotApi    = false;
};

// 用 x64dbg SDK 把 VA 反查模块 / 符号，再判定 dangerous。
// 调用前必须已 debugger active；否则 moduleName 为空、dangerous=false。
HotSpotInfo classifyBpAddr(std::uint64_t va);

// ============= run_dbg_command 白名单 =============

// 返回 "默认硬集 + Config::extraDbgCmdWhitelist 用户追加" 的合并集（懒加载、小写）。
const std::unordered_set<std::string>& dbgCmdWhitelist();

// 仅默认硬集（UI 用来展示"默认部分" vs "用户追加部分"）。
const std::unordered_set<std::string>& dbgCmdWhitelistDefaults();

}  // namespace x64ai
