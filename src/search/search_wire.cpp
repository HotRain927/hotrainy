// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

#include "hr/search/search_wire.h"

#include <windows.h>

#include "hr/app/app_context.h"
#include "hr/app/menu_ids.h"    // kHotkeySearch
#include "hr/search/search.h"
#include "hr/shared/common.h"     // logf
#include "hr/shared/hotkey.h"     // parseHotkey

namespace hr {

// 搜索热键：设置里改了立即重注册，失败返回 false（设置窗口会显示「已被占用」）
//
// 原来在 main.cpp 里。它只依赖 search()/cfg()/msgWnd() + 一个"当前是否注册成功"的
// 标志，标志走 app_context 的 searchHotkeyOk() -> 整段可以归位到搜索模块。
bool registerSearchHotkey() {
    if (searchHotkeyOk()) {
        UnregisterHotKey(msgWnd(), kHotkeySearch);
        searchHotkeyOk() = false;
    }
    if (!cfg().searchEnabled) {
        logf("hotkey[搜索]: 功能已关闭");
        return true;
    }
    UINT mods = 0, vk = 0;
    if (!parseHotkey(cfg().searchHotkey, mods, vk)) {
        logf("hotkey[搜索]: 解析失败 %s", cfg().searchHotkey.c_str());
        return false;
    }
    searchHotkeyOk() =
        RegisterHotKey(msgWnd(), kHotkeySearch, mods | MOD_NOREPEAT, vk) != FALSE;
    // ★ 失败时把 Windows 错误码也写下来：
    //   1409 = ERROR_HOTKEY_ALREADY_REGISTERED（被别的窗口占了）；
    //   其它值说明是参数/解析层面的问题 —— 只写"可能被占用"是分辨不出来的。
    const DWORD hkErr = searchHotkeyOk() ? 0 : ::GetLastError();
    logf("hotkey[搜索] %s (mods=0x%X vk=0x%X): %s%s", cfg().searchHotkey.c_str(),
         (unsigned)mods, (unsigned)vk,
         searchHotkeyOk() ? "已注册" : "注册失败",
         searchHotkeyOk() ? "" : (hkErr == 1409 ? " error=1409 已被别的窗口注册" : " error=?"));
    if (!searchHotkeyOk()) logf("hotkey[搜索]: GetLastError=%lu", (unsigned long)hkErr);
    return searchHotkeyOk();
}

// 搜索相关的设置键
bool search_applySetting(const std::string& key, bool& handled) {
    handled = true;
    if (key == "search_hotkey") {
        return registerSearchHotkey();
    } else if (key == "search_enabled") {
        // 对照 main.py:_set_search_enabled：关掉就收窗、停索引；打开就重建
        registerSearchHotkey();
        if (cfg().searchEnabled) {
            search().reloadIndex();
        } else {
            search().hide();
        }
    } else {
        handled = false;
    }
    return true;
}

void searchWire() {
    search().onAutoClosed = [](HWND hwnd) { dialogDismissed() = hwnd; };
}

}  // namespace hr
