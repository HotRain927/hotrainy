// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

#pragma once

#include <string>

namespace hr {

// 搜索模块的接线：搜索窗失去焦点自动收起时，把要关掉的对话框句柄记下来。
// 由 app/main.cpp 创建搜索窗之后调用一次。
void searchWire();

// 重新注册搜索热键（设置里改了立即重注册；失败返回 false -> 界面显示"已被占用"）。
bool registerSearchHotkey();

// 搜索相关的设置键生效（search_hotkey / search_enabled）。
bool search_applySetting(const std::string& key, bool& handled);

}  // namespace hr
