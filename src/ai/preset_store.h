// ai/preset_store.h
//
// PresetStore：管理 AgentPreset 列表，磁盘文件位置
// %APPDATA%\x64dbg-ai-plugin\agent_presets.json
//
// 行为：
//   - 首次启动（文件不存在或为空数组）→ 写入 defaultPresets()
//   - 已存在 → 加载；若用户删了某个出厂预设也尊重选择
//   - save() 原子写（先写 .tmp 再 rename）
//   - 线程：UI 线程访问，方法非线程安全
#pragma once

#include "ai/agent_preset.h"

#include <optional>
#include <string>
#include <vector>

namespace x64ai {

class PresetStore {
public:
    static PresetStore& instance();

    // 加载磁盘文件（幂等，重复调用安全）
    void load();

    // 强制重新加载
    void reload();

    // 取全部
    const std::vector<AgentPreset>& presets() const { return presets_; }

    // 按 id 查
    std::optional<AgentPreset> findById(const std::string& id) const;

    // 增 / 改：以 id 为键 upsert
    void upsert(const AgentPreset& p);

    // 删（readonly 也允许删；用户要求 "可删")
    bool remove(const std::string& id);

    // 复位为出厂预设（用户在 UI 上点"恢复出厂"时）
    void resetToDefaults();

    // 持久化
    bool save() const;

private:
    PresetStore() = default;
    bool        loaded_ = false;
    std::vector<AgentPreset> presets_;
};

}  // namespace x64ai
