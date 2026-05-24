// plugin/plugin_main.cpp
//
// x64dbg 插件主入口。导出 pluginit/plugstop/plugsetup，
// 负责初始化日志、注册菜单、创建 GUI 面板，以及在卸载时清理。
//
// 由于 x64dbg 同时支持 x32 / x64 两套 SDK，这里通过 _plugin_logprintf
// 等桥接接口与宿主交互，所有符号宽度由 pluginsdk 头文件根据
// _WIN64 自动适配。

#include <Windows.h>

// x64dbg SDK
#include "bridgemain.h"
#include "_plugins.h"

#include "plugin/plugin_callbacks.h"
#include "plugin/plugin_menus.h"
#include "ai/tools/tool_registry.h"
#include "storage/project_browser.h"
#include "ui/assistant_panel.h"
#include "util/logging.h"
#include "util/paths.h"

// x64dbg 要求插件提供这些全局变量
int            pluginHandle = 0;
HWND           hwndDlg      = nullptr;
int            hMenu        = 0;
int            hMenuDisasm  = 0;
int            hMenuDump    = 0;
int            hMenuStack   = 0;
int            hMenuGraph   = 0;
int            hMenuMemmap  = 0;
int            hMenuSymmod  = 0;

namespace {

constexpr const char* kPluginDisplayName = "x64dbg AI Assistant";

}  // namespace

extern "C" __declspec(dllexport) bool pluginit(PLUG_INITSTRUCT* initStruct)
{
    initStruct->pluginVersion = 1;
    initStruct->sdkVersion    = PLUG_SDKVERSION;
    strncpy_s(initStruct->pluginName, kPluginDisplayName, _TRUNCATE);

    pluginHandle = initStruct->pluginHandle;

    x64ai::initLogging();
    XAI_LOG_INFO("[{}] pluginit, handle={}", kPluginDisplayName, pluginHandle);
    XAI_LOG_INFO("plugin root: {}", x64ai::pluginRootDir().string());

    x64ai::registerCallbacks(pluginHandle);

    // M4: 注册内置工具（12 个：basic_read 5 / static_analysis 3 / dynamic_context 4）。
    // 必须早于任何 Agent 调用，且只注册一次（registerBuiltinTools 内部自带 once 保护）。
    x64ai::ToolRegistry::instance().registerBuiltinTools();

    // K-02：启动时清理空 / 老旧 db 文件。0 字节直接删；非空但 sessions+chunks 都为空
    // 且 mtime 早于 7 天的也清掉。当前还没活动项目（pluginit 阶段未开始调试），
    // 故 activeSha 传空。耗时 IO 量很小（仅 projects 目录下的 .db），同步执行。
    int removed = x64ai::ProjectBrowser::cleanupEmptyDbs(/*minAgeDays=*/7);
    if (removed > 0) {
        XAI_LOG_INFO("startup cleanup: removed {} empty/stale db(s)", removed);
    }
    return true;
}

extern "C" __declspec(dllexport) bool plugstop()
{
    XAI_LOG_INFO("plugstop");
    x64ai::unregisterCallbacks(pluginHandle);
    x64ai::AssistantPanel::destroyInstance();
    x64ai::shutdownLogging();
    return true;
}

extern "C" __declspec(dllexport) void plugsetup(PLUG_SETUPSTRUCT* setupStruct)
{
    hwndDlg     = setupStruct->hwndDlg;
    hMenu       = setupStruct->hMenu;
    hMenuDisasm = setupStruct->hMenuDisasm;
    hMenuDump   = setupStruct->hMenuDump;
    hMenuStack  = setupStruct->hMenuStack;
    hMenuGraph  = setupStruct->hMenuGraph;
    hMenuMemmap = setupStruct->hMenuMemmap;
    hMenuSymmod = setupStruct->hMenuSymmod;

    XAI_LOG_INFO("plugsetup, hwndDlg={}", static_cast<void*>(hwndDlg));
    x64ai::registerMenus(pluginHandle, hMenu, hMenuDisasm);
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        // 暂不做工作，等待 pluginit
    }
    return TRUE;
}
