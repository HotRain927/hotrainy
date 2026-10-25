// hr/tagbar/tagbar_wire.h — 标签栏 + 悬停面板的接线入口
//
// 实现见 src/tagbar/tagbar_wire.cpp。
#pragma once

#include <string>

#include <windows.h>

namespace hr {

// 把标签栏 / 悬停面板的回调接到模块上（main.cpp 初始化时调一次）。
void tagbarWire();

// 标签栏右侧四个小图标：截图 / 回收站 / 计算机 / 设置。
void launchRightIcon(int idx);

// 标签栏/悬停面板相关的设置键（hover_mode / panel_opacity / glass_mode /
// wallpaper_mode / shortcut_font_size / wallpaper）。
bool tagbar_applySetting(const std::string& key, bool& handled);

}  // namespace hr
