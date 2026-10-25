// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

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
