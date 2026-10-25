// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/shared/icons.h — 文件图标提取（带缓存）
#pragma once

#include <string>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hr {

// 规范化路径：SHGetFileInfoW / SHCreateItemFromParsingName 都不认正斜杠，
// 传 "D:/hot" 会直接失败（配置里存的正是正斜杠）
std::wstring shellPath(const std::wstring& in);

// 取图标：优先 IShellItemImageFactory（大图 + 带 alpha），退回 SHGetFileInfoW
// 返回的 HBITMAP 由缓存持有，不要释放
HBITMAP iconFor(const std::wstring& path, int px);

// HICON → 32bpp 位图（无 alpha 的老图标自动补成不透明）
HBITMAP iconToBitmap(HICON icon, int px);

void clearIconCache();
void pruneIconCache(size_t keepAtMost);

}  // namespace hr
