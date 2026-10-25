#include "hr/settings/settings_wire.h"

#include <windows.h>

#include <string>

#include "hr/app/app_context.h"
#include "hr/settings/settings.h"

namespace hr {

// 设置项变更 -> 交给主程序内部的生效逻辑（applySettingKey 是它的外部包装）
// 退出 -> 给主消息窗发 WM_CLOSE
void settingsWire() {
    SettingsWindow& s = settings();
    s.onSettingChanged = [](const std::string& key) -> bool { return applySettingKey(key); };
    s.onQuit = []() { PostMessageW(msgWnd(), WM_CLOSE, 0, 0); };
}

}  // namespace hr
