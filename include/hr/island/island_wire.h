// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

#pragma once

#include <string>

namespace hr {

// 岛台模块的接线（由 app/main.cpp 调用一次）。
// 逐步把原先堆在 main.cpp 的岛台相关回调搬到这里，见 docs/代码结构.md「main.cpp 瘦身方案」。
void islandWire();

// 任务栏岛台按钮的点击处理（两处安装点共用同一实现）
void onIslandButtonClick();

// 岛台相关的设置键（taskbar_transparent / taskbar_alpha）—— 岛台与任务栏是同一摊。
bool island_applySetting(const std::string& key, bool& handled);

}  // namespace hr