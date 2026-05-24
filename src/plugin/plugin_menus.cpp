// plugin/plugin_menus.cpp
#include "plugin/plugin_menus.h"

#include <Windows.h>

#include <mutex>
#include <unordered_map>
#include <vector>

#include <QString>

#include "bridgemain.h"
#include "_plugins.h"

#include "ai/agent_preset.h"
#include "ai/preset_store.h"
#include "ui/assistant_panel.h"
#include "util/logging.h"

namespace x64ai {

namespace {

// 菜单项 ID 枚举（任意值，仅在本插件内唯一）
enum MenuId : int {
    kMenuShowPanel       = 1001,
    kMenuTraceFunction   = 1102,

    // 反汇编右键 "AI ▶" 动态子菜单中的预设项 id 起始
    // 预留 100 个槽位（出厂 5 + 用户自定义留余地）
    kMenuPresetBase      = 2000,
    kMenuPresetMax       = 2099,
};

// 反汇编子菜单 handle，及 entryId → presetId 的反查表
int                                       g_hMenuDisasmAi   = 0;
int                                       g_pluginHandle    = 0;
std::mutex                                g_presetMapMutex;
std::unordered_map<int, std::string>      g_presetEntryMap;   // entryId -> presetId
std::vector<int>                          g_currentEntryIds;  // 当前已注册的预设 entryId，方便逐个 remove

// 取反汇编当前选中的起始 VA；未在调试或无选区返回 0。
uint64_t currentDisasmSelection()
{
    if (!DbgIsDebugging()) return 0;
    SELECTIONDATA sel{};
    if (!GuiSelectionGet(GUI_DISASSEMBLY, &sel)) return 0;
    return static_cast<uint64_t>(sel.start);
}

}  // namespace

void rebuildDisasmAiSubmenu()
{
    if (g_hMenuDisasmAi == 0) {
        // 还没注册主菜单（pluginit 之前）就被调，忽略
        return;
    }

    std::lock_guard<std::mutex> lk(g_presetMapMutex);

    // 1) 清空现有项（逐个 entryremove；_plugin_menuclear 会清掉子菜单本身的 entries）
    if (!_plugin_menuclear(g_hMenuDisasmAi)) {
        XAI_LOG_WARN("rebuildDisasmAiSubmenu: _plugin_menuclear failed for handle={}",
                     g_hMenuDisasmAi);
    }
    g_presetEntryMap.clear();
    g_currentEntryIds.clear();

    // 2) 读 PresetStore，按 showInContextMenu=true 重新填充
    //    （第一次进入时未加载过，主动 load；后续调用是幂等的）
    PresetStore::instance().load();
    const auto& presets = PresetStore::instance().presets();
    int nextId = kMenuPresetBase;
    int added  = 0;
    for (const auto& p : presets) {
        if (!p.showInContextMenu) continue;
        if (nextId > kMenuPresetMax) {
            XAI_LOG_WARN("rebuildDisasmAiSubmenu: preset slot exhausted at id={}", nextId);
            break;
        }
        // 显示名前加 "AI: " 前缀已经在子菜单标题下，这里直接用预设 name
        const std::string title = p.name;
        if (!_plugin_menuaddentry(g_hMenuDisasmAi, nextId, title.c_str())) {
            XAI_LOG_WARN("rebuildDisasmAiSubmenu: addentry failed: id={} name='{}'",
                         nextId, p.name);
            continue;
        }
        g_presetEntryMap.emplace(nextId, p.id);
        g_currentEntryIds.push_back(nextId);
        ++added;
        ++nextId;
    }

    XAI_LOG_INFO("rebuildDisasmAiSubmenu: registered {} preset entries", added);
}

void registerMenus(int pluginHandle, int hMenu, int hMenuDisasm)
{
    g_pluginHandle = pluginHandle;

    if (hMenu) {
        _plugin_menuaddentry(hMenu, kMenuShowPanel, "显示 AI 助手面板");
    }
    if (hMenuDisasm) {
        // 1) 创建 "AI ▶" 子菜单
        g_hMenuDisasmAi = _plugin_menuadd(hMenuDisasm, "AI");
        if (g_hMenuDisasmAi == 0) {
            XAI_LOG_ERROR("registerMenus: _plugin_menuadd('AI') returned 0");
        }
        // 2) 初次填充
        rebuildDisasmAiSubmenu();

        // 3) 保留原有"AI 追溯此函数调用链"（trace 逻辑独立，不走预设）
        _plugin_menuaddentry(hMenuDisasm, kMenuTraceFunction, "AI 追溯此函数调用链");
    }

    XAI_LOG_INFO("menus registered (hMenu={}, hMenuDisasm={}, hMenuDisasmAi={})",
                 hMenu, hMenuDisasm, g_hMenuDisasmAi);
}

void handleMenuEntry(int entryId)
{
    // 1) 预设动态菜单项
    if (entryId >= kMenuPresetBase && entryId <= kMenuPresetMax) {
        std::string presetId;
        {
            std::lock_guard<std::mutex> lk(g_presetMapMutex);
            auto it = g_presetEntryMap.find(entryId);
            if (it == g_presetEntryMap.end()) {
                XAI_LOG_WARN("handleMenuEntry: unknown preset entryId={}", entryId);
                return;
            }
            presetId = it->second;
        }
        XAI_LOG_INFO("disasm AI preset triggered: id={} preset='{}'", entryId, presetId);
        // panel 内部会自动 showInstance + marshal 到 UI 线程
        AssistantPanel::runPresetById(presetId, QString());
        return;
    }

    // 2) 固定菜单项
    switch (entryId) {
    case kMenuShowPanel:
        AssistantPanel::showInstance();
        break;
    case kMenuTraceFunction: {
        uint64_t va = currentDisasmSelection();
        if (va == 0) {
            XAI_LOG_WARN("traceFunction: no disasm selection / not debugging");
            return;
        }
        AssistantPanel::traceFunctionAt(va);
        break;
    }
    default:
        XAI_LOG_WARN("unknown menu entry: {}", entryId);
        break;
    }
}

}  // namespace x64ai
