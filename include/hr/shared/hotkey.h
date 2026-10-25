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
