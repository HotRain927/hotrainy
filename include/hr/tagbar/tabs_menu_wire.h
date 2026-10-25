// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/tagbar/tabs_menu_wire.h — 标签栏/面板右键菜单与托盘菜单的入口
//
// 实现见 src/tagbar/tabs_menu_wire.cpp（原来堆在 main.cpp 里的 CRUD 那一大块）。
#pragma once

#include <windows.h>

#include "hr/shared/menu.h"

namespace hr {

// 构造右键菜单的回调集合（增删改快捷方式、增删改标签、打开设置/退出）。
// main.cpp 里两处 showContextMenu 都用它（另有 makeHooksWire() 供接线文件用）。
ContextMenuHooks makeHooks();

// 托盘图标的右键菜单（显示/隐藏标签栏、设置、重新加载配置、退出）。
void trayMenu(HWND owner);

}  // namespace hr
