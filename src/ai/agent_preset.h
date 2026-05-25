// ai/agent_preset.h
//
// AgentPreset：一次"Agent 触发"的全部可配置项。
//
// 字段：
//   id              UUID / 短 slug；唯一键
//   name            UI 显示名（中文）
//   description     简短说明（鼠标 hover）
//   systemPrompt    role=system 的初始消息（可为空 → 用全局默认）
//   userTemplate    用户消息模板；支持占位符（见 expandTemplate 注释）
//   enabledTools    工具白名单；空表示"用所有已注册"
//   maxIter         上限 1-50；默认 20
//   temperature     0.0-1.5；默认 0.2
//   provider        "" / "deepseek" / "copilot"；空 → 用当前激活的
//   model           "" → provider->defaultModel()
//   showInContextMenu 是否出现在反汇编右键 AI 子菜单中
//   readonly        出厂预设（不可删，但可"另存为"复制一份编辑）
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace x64ai {

// 出厂预设的 schema 版本号。
// 每次修改 defaultPresets() 的语义（systemPrompt 措辞、enabledTools 列表等）
// 都要 ++ 此版本；PresetStore 加载时若检测到磁盘版本更低，会用新版 defaults
// 覆盖所有 readonly=true 的预设，但保留用户自定义（readonly=false）。
constexpr int kPresetSchemaVersion = 13;

struct AgentPreset {
    std::string id;
    std::string name;
    std::string description;
    std::string systemPrompt;
    std::string userTemplate;
    std::vector<std::string> enabledTools;
    int    maxIter     = 20;
    double temperature = 0.2;
    std::string provider;       // ""/"deepseek"/"copilot"
    std::string model;
    bool   showInContextMenu = true;
    bool   readonly          = false;

    // S9：UI 分类
    //   group: 单一分组键（general / exploration / cracking / tracing / scenarios）；
    //          空字符串视为 "general"（fromJson 兼容老盘）
    //   tags : 多标签，正交于 group，用于二级过滤
    //          (read-only / write / hw-bp / cfg / patch / annotation / dataflow / anti-debug ...)
    std::string              group;
    std::vector<std::string> tags;

    nlohmann::json toJson() const;
    static AgentPreset fromJson(const nlohmann::json& j);
};

// 模板占位符渲染：
//   {{cip}}        当前指令指针的十六进制（如 "0x0000000140001234"）
//   {{module}}     {{cip}} 所在模块名（无 ext）
//   {{selection}}  反汇编窗口选中范围 "0x.. - 0x.."；无选中为 "(none)"
//   {{disasm}}     {{cip}} 附近若干行反汇编（默认 32 行）
//   {{user}}       用户原始问句（在 AssistantPanel 触发时填用户输入；右键触发时为空）
//
// 未识别的 {{xxx}} 原样保留（方便调试）。
struct PresetTemplateContext {
    std::string cipHex;        // 必填
    std::string moduleName;
    std::string selectionRange;
    std::string disasmBlock;
    std::string userQuery;
};

std::string expandPresetTemplate(const std::string& tmpl,
                                 const PresetTemplateContext& ctx);

// 返回出厂预设列表（首次启动时灌入磁盘）
std::vector<AgentPreset> defaultPresets();

}  // namespace x64ai
