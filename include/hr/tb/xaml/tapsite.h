// hr/tb/xaml/tapsite.h — TAP 站点接口（DLL 侧）
//
// 本文件编译进 GlassTAP.dll，运行在 explorer.exe 进程里。
#pragma once

#include "hr/tb/xaml/interfaces.h"

#include <xamlOM.h>

#include <winrt/base.h>

namespace hr {
namespace tb {
namespace xaml {

// 系统通过 InitializeXamlDiagnosticsEx 回调 DllGetClassObject(CLSID_GlassTAPSite)，
// 我们返回 TAPSite；系统随即调用 SetSite 把 IXamlDiagnostics 交过来。
//
// 这是我们与 XAML 诊断之间的唯一接触点。
class TAPSite : public winrt::implements<TAPSite, IObjectWithSite, winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) noexcept override;
    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) noexcept override;
};

// 在 explorer 进程内完成 XAML 诊断初始化。
// 返回 S_OK 表示诊断通道建立成功（SetSite 会被系统回调）。
HRESULT InstallTap(const wchar_t* connectionName);

// 本 DLL 是否已成功装载并尝试过初始化
bool tapInstalled();

// 还原所有任务栏外观
void restoreAll();

// 注册跨进程服务对象（主程序 GetActiveObject 取用）
void registerServiceObject();

}  // namespace xaml
}  // namespace tb
}  // namespace hr