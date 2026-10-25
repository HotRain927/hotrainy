#include "hr/settings/settings_wire.h"

#include <windows.h>

#include <string>

#include "hr/app/app_context.h"
#include "hr/app/version.h"
#include "hr/shared/common.h"   // logf（不包含的话会被解析成 C 数学库的 logf(float)）
#include "hr/settings/settings.h"

namespace hr {

// ★ 打开设置窗。
//
//   原来是 main.cpp 里的 openSettings()，按"设置的东西写一块"搬到这里。
//   依赖的 main 内部量走 app_context 的薄访问器（bar/settings/cfg/dark/hidePanelWire），
//   版本号走 hr/app/version.h。
void openSettings() {
    logf("settings: 准备打开设置窗口（active tab=%d）", bar().active());
    hidePanelWire();
    if (settings().isOpen()) {
        settings().setActiveTab(bar().active());
        return;
    }
    settings().setActiveTab(bar().active());
    logf("settings: 设置回调");
    settingsWire();   // 接线（本文件内，紧跟着下面定义）
    logf("settings: 调 open()");
    const bool ok = settings().open(GetModuleHandleW(nullptr), &cfg(), dark(), kVersion);
    logf("settings: open() 返回 %d", (int)ok);
}

// 设置项变更 -> 交给主程序内部的生效逻辑（applySettingKey 是它的外部包装）
// 退出 -> 给主消息窗发 WM_CLOSE
void settingsWire() {
    SettingsWindow& s = settings();
    s.onSettingChanged = [](const std::string& key) -> bool { return applySettingKey(key); };
    s.onQuit = []() { PostMessageW(msgWnd(), WM_CLOSE, 0, 0); };
}

}  // namespace hr
