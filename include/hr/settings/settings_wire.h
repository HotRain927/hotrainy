#pragma once

namespace hr {

// 设置模块的接线：把设置窗的回调接到应用上下文上。
// 由 app/main.cpp 在创建设置窗之后调用一次。
// （原实现散落在 main.cpp 里，按模块归位后见 docs/代码结构.md「main.cpp 瘦身方案」）
void settingsWire();

// 打开设置窗（原 main.cpp 的内部函数，按模块归位搬到这里）。
// 主程序里右键菜单/托盘菜单要用，所以对外声明。
void openSettings();

}  // namespace hr
