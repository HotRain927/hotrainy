// hr/app/version.h — 版本号
//
// 原来写死在 main.cpp 里（`const char* kVersion = "11.2";`）。
// 结构重构后接线文件（settings_wire 等）也要用它，所以抽成头文件。
#pragma once

namespace hr {

constexpr const char* kVersion = "11.2";

}  // namespace hr