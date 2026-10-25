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