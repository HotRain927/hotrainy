// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/shared/hotkey.h — 快捷键文本 ↔ (mods, vk)，对照 core/hotkey.py
//
// 键名表与 Python 版保持一致：'Ctrl+Alt+Space' 这种写法两边都能读。
#pragma once

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hr {

// 解析 'Ctrl+Alt+Space'；成功返回 true
bool parseHotkey(const std::string& spec, UINT& mods, UINT& vk);

// (mods, vk) → 'Ctrl+Alt+Space'
std::string formatHotkey(UINT mods, UINT vk);

}  // namespace hr
