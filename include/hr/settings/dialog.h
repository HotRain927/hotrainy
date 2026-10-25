// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/settings/dialog.h — 玻璃弹窗：输入 / 确认 / 提示（对照 ui/glass_dialog.py）
//
// 对外三个函数，形状与 tkinter 那套对齐：
//   askString(...)  -> 确定返回 true，文本写进 out；取消返回 false
//   askYesNo(...)   -> 确定返回 true
//   showWarning(...)
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

void setDialogDark(bool dark);

bool askString(HWND owner, const wchar_t* title, const wchar_t* message,
               const std::wstring& initial, std::wstring& out);
bool askYesNo(HWND owner, const wchar_t* title, const wchar_t* message);
void showWarning(HWND owner, const wchar_t* title, const wchar_t* message);

}  // namespace hr
