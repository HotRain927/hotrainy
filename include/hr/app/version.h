// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/app/version.h — 版本号
//
// 原来写死在 main.cpp 里（`const char* kVersion = "11.2";`）。
// 结构重构后接线文件（settings_wire 等）也要用它，所以抽成头文件。
#pragma once

namespace hr {

constexpr const char* kVersion = "11.2";

}  // namespace hr