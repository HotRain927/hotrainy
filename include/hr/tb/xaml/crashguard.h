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