#pragma once

// ★ 应用上下文（app context）
//
// 目的：让各模块的接线文件（xxx_wire.cpp）能在**不依赖 main.cpp 内部匿名命名空间**的前提下
// 拿到共享对象，从而把 main.cpp 里的接线回调逐步搬回各模块自己家里。
//
// 实现：main.cpp 在匿名命名空间末尾之后提供这些薄访问器（只返回引用，不做任何事）。
// 新增模块时，在这里加一个访问器 + 在 main.cpp 的实现块里加一行即可。

#include <string>
#include <windows.h>

#include "hr/shared/config.h"
#include "hr/tagbar/bar.h"
#include "hr/tagbar/panel.h"
#include "hr/island/island.h"
#include "hr/island/islandbtn.h"
#include "hr/search/search.h"
#include "hr/settings/settings.h"

namespace hr {

Config&        cfg();         // 全局配置
BarWindow&     bar();         // 顶部标签栏
HoverPanel&    panel();       // 标签栏的悬停面板
IslandWindow&  island();      // 岛台面板
IslandButton&  islandBtn();   // 任务栏上的岛台按钮
SearchWindow&  search();      // 搜索窗
SettingsWindow& settings();   // 设置窗
HWND           msgWnd();      // 主消息窗

// 设置项生效入口（main.cpp 内部 applySetting 的外部包装，故意不同名以免重载歧义）
bool applySettingKey(const std::string& key);

}  // namespace hr
