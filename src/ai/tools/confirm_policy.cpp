// ai/tools/confirm_policy.cpp
#include "ai/tools/confirm_policy.h"

#include "util/config.h"

#include <algorithm>
#include <mutex>

namespace x64ai {

namespace {

std::string toLower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const std::unordered_set<std::string>& hardEnforcedSet()
{
    // 函数级 static + 首次访问惰性初始化，避免静态初始化顺序问题。
    static const std::unordered_set<std::string> kSet = {
        "run_dbg_command",
        "start_debug",
        "attach_debug",
        "stop_debug",
        "patch_file",
        // K-39 系统侧工具：永不豁免 auto-approve
        // shell_* 命令完全开放（Q2=B），必须人审；
        // fs_write_file / fs_create_file 涉及磁盘写，一并强制 confirm 保险
        "shell_cmd",
        "shell_pwsh",
        "fs_write_file",
        "fs_create_file",
    };
    return kSet;
}

// 用户配置的 auto_approve_tools。once_flag 合并避免运行时漂移。
// 与 K-32 的 dbgCmdWhitelist 设计一致：修改 config 必须重启插件。
const std::unordered_set<std::string>& userAutoApproveSet()
{
    static std::unordered_set<std::string> kSet;
    static std::once_flag                  kOnce;
    std::call_once(kOnce, [] {
        const auto& cfg = Config::instance().get();
        for (const auto& s : cfg.autoApproveTools) {
            kSet.insert(toLower(s));
        }
    });
    return kSet;
}

}  // namespace

const std::unordered_set<std::string>& confirmHardEnforced()
{
    return hardEnforcedSet();
}

bool isConfirmHardEnforced(const std::string& toolName)
{
    return hardEnforcedSet().count(toLower(toolName)) > 0;
}

const std::unordered_set<std::string>& autoApproveTools()
{
    return userAutoApproveSet();
}

bool isAutoApproved(const std::string& toolName)
{
    const auto name = toLower(toolName);
    if (hardEnforcedSet().count(name)) return false;  // 黑名单覆盖一切
    return userAutoApproveSet().count(name) > 0;
}

}  // namespace x64ai
