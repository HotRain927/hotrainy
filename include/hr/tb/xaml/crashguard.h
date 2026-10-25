// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

/* hr/tb/xaml/crashguard.h - in-process crash reporting for the bootstrap layer
 *
 * Pure C on purpose: see crashguard.c. Included by glassboot.c.
 */
#ifndef HR_TB_XAML_CRASHGUARD_H
#define HR_TB_XAML_CRASHGUARD_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the unhandled exception filter. Safe to call more than once. */
void CrashGuard_Install(void);

#ifdef __cplusplus
}
#endif

#endif /* HR_TB_XAML_CRASHGUARD_H */