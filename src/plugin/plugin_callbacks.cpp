// plugin/plugin_callbacks.cpp
#include "plugin/plugin_callbacks.h"

#include <Windows.h>

#include "bridgemain.h"
#include "_plugins.h"

#include "plugin/plugin_menus.h"
#include "storage/project_context.h"
#include "trace/callstack_tracer.h"
#include "trace/trace_recorder.h"
#include "ui/assistant_panel.h"
#include "util/logging.h"

namespace x64ai {

namespace {

PLUG_CB_MENUENTRY g_lastEntry{};  // 占位，便于将来扩展

void cbInitDebug(CBTYPE, PLUG_CB_INITDEBUG* info)
{
    std::string path = (info && info->szFileName) ? info->szFileName : "";
    XAI_LOG_INFO("CB_INITDEBUG: {}", path);
    ProjectContext::instance().onDebugStart(path);
    AssistantPanel::onDebugStarted();
}

void cbStopDebug(CBTYPE, PLUG_CB_STOPDEBUG*)
{
    XAI_LOG_INFO("CB_STOPDEBUG");
    // 顺序敏感：先 UI 收口（取消 agent worker、停 trace），再落盘项目上下文。
    // S0-C1 (2026-05-24)：x64dbg SDK 同一 (plugin,CBTYPE) 后注册者覆盖前注册者，
    // 历史上 trace_recorder.cpp 单独注册了 CB_STOPDEBUG，会把本回调挤掉，
    // 导致 AssistantPanel::onDebugStopped + ProjectContext::onDebugStop 永不触发。
    // 现已把 trace 的 stop 逻辑改为被动调用（见下方），由本处单点分发。
    AssistantPanel::onDebugStopped();
    TraceRecorder::instance().onStopDebug();
    CallStackTracer::instance().onStopDebug();
    ProjectContext::instance().onDebugStop();
}

void cbMenuEntry(CBTYPE, PLUG_CB_MENUENTRY* info)
{
    if (!info) return;
    XAI_LOG_DEBUG("CB_MENUENTRY id={}", info->hEntry);
    handleMenuEntry(info->hEntry);
}

}  // namespace

void registerCallbacks(int pluginHandle)
{
    _plugin_registercallback(pluginHandle, CB_INITDEBUG,
                             reinterpret_cast<CBPLUGIN>(cbInitDebug));
    _plugin_registercallback(pluginHandle, CB_STOPDEBUG,
                             reinterpret_cast<CBPLUGIN>(cbStopDebug));
    _plugin_registercallback(pluginHandle, CB_MENUENTRY,
                             reinterpret_cast<CBPLUGIN>(cbMenuEntry));

    // M3.2: 调用链追溯录制器
    TraceRecorder::instance().registerCallbacks(pluginHandle);
}

void unregisterCallbacks(int pluginHandle)
{
    _plugin_unregistercallback(pluginHandle, CB_INITDEBUG);
    _plugin_unregistercallback(pluginHandle, CB_STOPDEBUG);
    _plugin_unregistercallback(pluginHandle, CB_MENUENTRY);

    TraceRecorder::instance().unregisterCallbacks(pluginHandle);
}

}  // namespace x64ai
