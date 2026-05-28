// plugin/plugin_callbacks.cpp
#include "plugin/plugin_callbacks.h"

#include <Windows.h>

#include "bridgemain.h"
#include "_plugins.h"

#include "dbg/event_bus.h"
#include "plugin/plugin_menus.h"
#include "storage/project_context.h"
#include "trace/callstack_tracer.h"
#include "trace/trace_recorder.h"
#include "ui/assistant_panel.h"
#include "util/encoding.h"
#include "util/logging.h"

namespace x64ai {

namespace {

PLUG_CB_MENUENTRY g_lastEntry{};  // 占位，便于将来扩展

void cbInitDebug(CBTYPE, PLUG_CB_INITDEBUG* info)
{
    // x64dbg SDK 约定 char* 即 UTF-8（见 bridgemain.h: "code page is utf8"）。
    // 历史误判：曾把它当 ACP 强转 ansiToUtf8，导致 UTF-8 字节被当 GBK 二次解码 → 乱码 → fs 全失败。
    // 现在直接透传；ProjectContext::onDebugStart 内部用 isValidUtf8 兜底，能容忍少数非 UTF-8 来源。
    std::string path = (info && info->szFileName) ? info->szFileName : "";
    XAI_LOG_INFO("CB_INITDEBUG: {}", path);
    ProjectContext::instance().onDebugStart(path);
    AssistantPanel::onDebugStarted();
    // S2-B：广播；订阅者用 payload.raw 取 path 字符串
    DbgEventPayload p{};
    p.raw  = info;
    p.addr = 0;
    EventBus::instance().publish(DbgEvent::DebugStarted, p);
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

    // S2-B：通知所有 EventBus waiter，避免 wait_for_event 卡到超时
    EventBus::instance().cancelAllWaits();
    DbgEventPayload p{};
    EventBus::instance().publish(DbgEvent::DebugStopped, p);
}

void cbBreakpoint(CBTYPE, PLUG_CB_BREAKPOINT* info)
{
    if (!info || !info->breakpoint) return;
    const std::uint64_t addr = static_cast<std::uint64_t>(info->breakpoint->addr);
    DbgEventPayload p{};
    p.addr = addr;
    p.raw  = info->breakpoint;
    EventBus::instance().publish(DbgEvent::Breakpoint, p);
}

void cbPauseDebug(CBTYPE, PLUG_CB_PAUSEDEBUG*)
{
    DbgEventPayload p{};
    EventBus::instance().publish(DbgEvent::Paused, p);
}

void cbResumeDebug(CBTYPE, PLUG_CB_RESUMEDEBUG*)
{
    DbgEventPayload p{};
    EventBus::instance().publish(DbgEvent::Resumed, p);
}

void cbStepped(CBTYPE, PLUG_CB_STEPPED*)
{
    DbgEventPayload p{};
    EventBus::instance().publish(DbgEvent::Stepped, p);
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

    // S2-B：调试器事件总线统一来源；多消费者改走 EventBus::subscribe
    _plugin_registercallback(pluginHandle, CB_BREAKPOINT,
                             reinterpret_cast<CBPLUGIN>(cbBreakpoint));
    _plugin_registercallback(pluginHandle, CB_PAUSEDEBUG,
                             reinterpret_cast<CBPLUGIN>(cbPauseDebug));
    _plugin_registercallback(pluginHandle, CB_RESUMEDEBUG,
                             reinterpret_cast<CBPLUGIN>(cbResumeDebug));
    _plugin_registercallback(pluginHandle, CB_STEPPED,
                             reinterpret_cast<CBPLUGIN>(cbStepped));

    // S2-C：CallStackTracer 的 Breakpoint 接口也从 EventBus 订阅
    // （它没有 registerCallbacks；以前是被 trace_recorder 的 cbBreakpoint 顺手叫）
    EventBus::instance().subscribe(
        DbgEvent::Breakpoint,
        [](const DbgEventPayload& p) {
            CallStackTracer::instance().onBreakpoint(p.addr);
        });

    // M3.2: 调用链追溯录制器（CB_TRACEEXECUTE / CB_STARTTRACE / CB_STOPTRACE）
    // CB_BREAKPOINT 不再由 TraceRecorder 单独注册，它内部也走 EventBus 订阅
    TraceRecorder::instance().registerCallbacks(pluginHandle);
}

void unregisterCallbacks(int pluginHandle)
{
    _plugin_unregistercallback(pluginHandle, CB_INITDEBUG);
    _plugin_unregistercallback(pluginHandle, CB_STOPDEBUG);
    _plugin_unregistercallback(pluginHandle, CB_MENUENTRY);

    _plugin_unregistercallback(pluginHandle, CB_BREAKPOINT);
    _plugin_unregistercallback(pluginHandle, CB_PAUSEDEBUG);
    _plugin_unregistercallback(pluginHandle, CB_RESUMEDEBUG);
    _plugin_unregistercallback(pluginHandle, CB_STEPPED);

    TraceRecorder::instance().unregisterCallbacks(pluginHandle);
}

}  // namespace x64ai
