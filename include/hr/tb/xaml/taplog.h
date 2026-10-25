// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/tb/xaml/taplog.h — DLL 侧独立日志
#pragma once

namespace hr {
namespace tb {
namespace xaml {

void tapLogInit();
void tapLogf(const char* fmt, ...);

// 宽字符版本。
//
// ⚠ 需要它的原因：tapLogf 是 printf 风格（窄格式串），
//   往里传 %ls（宽字符串）会让 varargs 解析错位、破坏调用栈 ——
//   实测表现为日志出现大段乱码，而且后续的 API 调用看似成功实际没生效。
//   凡是打宽字符串（路径、类型名）都必须用这个函数。
void tapLogw(const wchar_t* fmt, ...);

}  // namespace xaml
}  // namespace tb
}  // namespace hr