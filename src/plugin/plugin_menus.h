// plugin/plugin_menus.h
#pragma once

namespace x64ai {

// 在主菜单 + 反汇编右键菜单上注册 AI 助手菜单项。
void registerMenus(int pluginHandle, int hMenu, int hMenuDisasm);

// 处理 CB_MENUENTRY 事件，根据条目 id 分发动作。
void handleMenuEntry(int entryId);

// 重新构建反汇编 "AI ▶" 子菜单（按当前 PresetStore 内 showInContextMenu=true 的预设）。
// 调用时机：注册时 + 预设保存/恢复出厂后 PresetEditorDialog 回调。
void rebuildDisasmAiSubmenu();

}  // namespace x64ai
