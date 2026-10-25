#pragma once

// ★ 应用上下文（app context）
//
// 目的：让各模块的接线文件（xxx_wire.cpp）能在**不依赖 main.cpp 内部匿名命名空间**的前提下
// 拿到共享对象，从而把 main.cpp 里的接线回调逐步搬回各模块自己家里。
//
// 实现：main.cpp 在匿名命名空间末尾之后提供这些薄访问器（只返回引用，不做任何事）。
// 新增模块时，在这里加一个访问器 + 在 main.cpp 的实现块里加一行即可。

#include <string>
#include <windows.h>

#include "hr/shared/config.h"
#include "hr/shared/menu.h"
#include "hr/tagbar/bar.h"
#include "hr/tagbar/panel.h"
#include "hr/island/island.h"
#include "hr/island/islandbtn.h"
#include "hr/search/search.h"
#include "hr/settings/settings.h"

namespace hr {

Config&        cfg();         // 全局配置
BarWindow&     bar();         // 顶部标签栏
HoverPanel&    panel();       // 标签栏的悬停面板
IslandWindow&  island();      // 岛台面板
IslandButton&  islandBtn();   // 任务栏上的岛台按钮
SearchWindow&  search();      // 搜索窗
SettingsWindow& settings();   // 设置窗
HWND           msgWnd();      // 主消息窗
HWND&          dialogDismissed();  // 搜索窗自动收起时要关掉的那个对话框句柄
DWORD&         iconClickTick();    // 任务栏岛台按钮最近一次点击的时刻
DWORD&         pendingCloseTick(); // 挂起的"点外收起"时刻
bool&          dark();             // 全局深浅（面板/搜索窗等共用）
bool&          searchHotkeyOk();   // 搜索热键当前是否注册成功（search_wire 维护）
void           hidePanelWire();    // 收起悬停面板（main.cpp 内部 hidePanel 的包装）
// 在某标签下展开悬停面板（main.cpp 内部 showPanelForTab 的包装）
void           showPanelForTabWire(int idx, RECT bounds, bool force = false);

// 打开/收起岛台面板（原 main.cpp 内部 toggleIslandPanel 的外部包装；故意不同名以免歧义）
void toggleIslandPanelWire(RECT btn);

// 设置项生效入口（main.cpp 内部 applySetting 的外部包装，故意不同名以免重载歧义）
bool applySettingKey(const std::string& key);

// 包装 main.cpp 内部的 makeHooks()（返回自定义类型 ContextMenuHooks，自动生成器抓不到，手工补）
ContextMenuHooks makeHooksWire();

}  // namespace hr
