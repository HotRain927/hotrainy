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
