// hr/app/menu_ids.h — 菜单命令 ID + 全局热键 ID
//
// 原来定义在 main.cpp 里。托盘菜单（现在在 src/tagbar/tabs_menu_wire.cpp）
// 和主消息循环的 WM_COMMAND 处理都要用，所以抽到头文件共享。
#pragma once

namespace hr {

// 全局热键 ID（RegisterHotKey 用）
const int kHotkeyHideBar = 1;
const int kHotkeySearch = 2;

enum {
    IDM_TOGGLE = 100,
    IDM_SETTINGS = 101,
    IDM_RELOAD = 102,
    IDM_EXIT = 103,
};

}  // namespace hr
