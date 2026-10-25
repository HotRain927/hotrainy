// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

/* hr/tb/xaml/glassboot.h - pure C bootstrap layer for GlassBoot.dll
 *
 * This header is C-compatible on purpose: glassboot.c must not pull in
 * any C++ runtime or C++/WinRT headers.
 */
#ifndef HR_TB_XAML_GLASSBOOT_H
#define HR_TB_XAML_GLASSBOOT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported so the injector can confirm the DLL identity if needed. */
HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv);

#ifdef __cplusplus
}
#endif

#endif /* HR_TB_XAML_GLASSBOOT_H */