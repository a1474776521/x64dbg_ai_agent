// ai/tools/dbg_state_util.h
//
// K-43: 调试器子状态查询 + cip 取值的公共小工具。
//
// 起源：K-42 在 debug_navigation_tools.cpp 匿名 namespace 里加了 currentDbgStateStr()，
// 用来给 error 附 "current_state=running/paused/not_debugging"。K-43 扫描发现 step_in /
// step_over / run_until / set_register / set_flag / stack_push / run_script_file 等
// 工具都需要做"子状态判定 + error 附状态"的同款工作，干脆把 helper 抽出来。
//
// header-only inline：函数体很小，且 bridgemain.h 已经被绝大部分 tool cpp 包含；
// 走 inline 既不增加链接负担，也不需要改 CMakeLists.txt 加新源文件。
//
// 重要：这两个函数都假设调用方处于 worker 后台线程或主线程都行——x64dbg SDK 的
// DbgIsDebugging / DbgIsRunning / DbgValFromString 都是线程安全只读查询。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include <Windows.h>
#include "bridgemain.h"

namespace x64ai {

// 返回 debuggee 当前子状态字符串：
//   "not_debugging" : 未附加任何进程（DbgIsDebugging=false）
//   "running"       : 已附加且 debuggee 正在跑（DbgIsRunning=true）
//   "paused"        : 已附加但 debuggee 停下（断点/暂停/单步结束）
inline const char* currentDbgStateStr() noexcept
{
    if (!DbgIsDebugging()) return "not_debugging";
    return DbgIsRunning() ? "running" : "paused";
}

// 取 cip 并格式化成 "0x%llX"。
//   - paused 时返回真实 cip
//   - running 时返回 debugger 缓存的"上次 paused cip"（stale，给诊断用还行，
//     不要拿去算业务地址）
//   - not_debugging 时 DbgValFromString 行为是未定义/返回 0，所以这里返回空串
inline std::string currentCipHexOrEmpty()
{
    if (!DbgIsDebugging()) return std::string();
    duint cip = DbgValFromString("cip");
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX",
                  static_cast<unsigned long long>(cip));
    return std::string(buf);
}

}  // namespace x64ai
