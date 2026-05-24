// plugin/plugin_callbacks.h
#pragma once

namespace x64ai {

// 注册 / 注销 x64dbg 事件回调（CB_INITDEBUG / CB_STOPDEBUG / CB_MENUENTRY 等）。
void registerCallbacks(int pluginHandle);
void unregisterCallbacks(int pluginHandle);

}  // namespace x64ai
