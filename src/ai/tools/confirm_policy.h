// ai/tools/confirm_policy.h
//
// K-33: 用户可配置「跳过 5s confirm」的工具集合。
//
// 设计：
//   - 默认：所有 ToolCategory::Write + requiresUserConfirmation()==true 的工具都弹 5s confirm
//   - 用户可在 config.json 加 "auto_approve_tools": ["set_label", ...] 让指定工具跳过弹窗
//   - 即使被加进 auto_approve_tools，黑名单内的工具仍强制弹窗（confirmHardEnforced 集合）
//   - 跳过弹窗时仍写 audit log，phase="auto_approved"
//
// 黑名单（强制 confirm 不可豁免）：
//   - run_dbg_command       命令逃生口，可执行任意 x64dbg 命令
//   - start_debug           启动新进程
//   - attach_debug          附加到任意 PID
//   - stop_debug            会杀目标进程（与 detach 区别）
//   - patch_file            落盘补丁不可撤销
//
// 暴露给：
//   - ToolRegistry::dispatch       判定 needConfirm
//   - SafetyBrowserDialog          Tab5 展示
//   - config.cpp                   加载时校验
#pragma once

#include <string>
#include <unordered_set>

namespace x64ai {

// 强制弹 confirm 的黑名单（不可被用户 auto_approve_tools 豁免）。
// 返回的引用稳定（static const），可直接迭代。
const std::unordered_set<std::string>& confirmHardEnforced();

// 给定工具名是否在黑名单内（命中即强制 confirm）。
bool isConfirmHardEnforced(const std::string& toolName);

// 用户在 config.json 配置的 auto_approve_tools 当前生效集（从 Config 读取，已小写化）。
// 黑名单项即使出现在配置中也会在 isAutoApproved() 中被忽略。
const std::unordered_set<std::string>& autoApproveTools();

// 综合判断：本次调用是否可跳过 confirm。
// 命中条件：toolName ∈ autoApproveTools() && toolName ∉ confirmHardEnforced()
bool isAutoApproved(const std::string& toolName);

}  // namespace x64ai
