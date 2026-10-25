// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/tb/xaml/interfaces.h — COM 接口与 CLSID 定义
//
// 这是"注入 explorer 替换任务栏 XAML 背景"方案的跨进程契约。
// 架构（参考 Windows 官方 XAML 诊断机制，非 hack）：
//
//   hotrainy.exe
//     └─ 注入器：SetWindowsHookEx 让 GlassTAP.dll 进入 explorer
//          └─ GlassTAP.dll 的 DllMain 检测到已在 explorer → 启动 TAPSite
//               └─ InitializeXamlDiagnosticsEx(...)  ← Windows SDK 公开 API
//                    └─ 系统回调 DllGetClassObject(CLSID_TAPSite)
//                         └─ TAPSite::SetSite(IXamlDiagnostics*)  拿到诊断接口
//                              └─ AdviseVisualTreeChange 监听 XAML 树
//                                   └─ 找到名为 "BackgroundFill" 的 Rectangle
//                                        └─ 替换它的 Fill 画刷 → 任务栏背景变透明
//
// 为什么必须这样做（实测结论）：
//   Win11 build 22621+ 的任务栏由 Windows.UI.Composition 渲染，
//   背景是 explorer 进程内 XAML 树里的一个 Rectangle.Fill。
//   从外部用 DWM / accent / blur / backdrop 全部无效（已穷举验证）。
#pragma once

// ⚠ 这个头文件会被主程序各处包含，必须尽量"轻"。
//
// 不要加 <ocidl.h>：它会引入 IAudioMeterInformation 等定义，
// 与 audio.cpp 里手写的同名声明的冲突（实测报 C2011 重定义）。
// MIDL_INTERFACE 和 IUnknown 由 objbase.h 提供，够用了。
#include <windows.h>
#include <objbase.h>

namespace hr {
namespace tb {
namespace xaml {

// ── CLSID / IID ──────────────────────────────────────────────────────
// 自定义 GUID，不能用 TranslucentTB 的（避免冲突）。
// TAPSite：系统通过这个 CLSID 找到我们的 IObjectWithSite 实现
// TaskbarAppearanceService：hotrainy 通过它远程调用注入进去的服务

// {7A3F9C21-5B4D-4E8A-9F12-3C6D8E5A1B47}
static const GUID CLSID_GlassTAPSite = {
    0x7a3f9c21, 0x5b4d, 0x4e8a, {0x9f, 0x12, 0x3c, 0x6d, 0x8e, 0x5a, 0x1b, 0x47}};

// {7A3F9C22-5B4D-4E8A-9F12-3C6D8E5A1B48}
static const GUID CLSID_GlassAppearanceService = {
    0x7a3f9c22, 0x5b4d, 0x4e8a, {0x9f, 0x12, 0x3c, 0x6d, 0x8e, 0x5a, 0x1b, 0x48}};

// {7A3F9C23-5B4D-4E8A-9F12-3C6D8E5A1B49}
static const GUID IID_IGlassAppearanceService = {
    0x7a3f9c23, 0x5b4d, 0x4e8a, {0x9f, 0x12, 0x3c, 0x6d, 0x8e, 0x5a, 0x1b, 0x49}};

// {7A3F9C24-5B4D-4E8A-9F12-3C6D8E5A1B4A}
static const GUID IID_IGlassVersionedApi = {
    0x7a3f9c24, 0x5b4d, 0x4e8a, {0x9f, 0x12, 0x3c, 0x6d, 0x8e, 0x5a, 0x1b, 0x4a}};

// 接口版本。hotrainy 与 DLL 版本不匹配时拒绝调用，避免 ABI 错位。
constexpr DWORD kGlassTapApiVersion = 1;

// ── 画刷类型 ─────────────────────────────────────────────────────────
enum class GlassBrush : UINT {
    SolidColor = 0,   // 纯色
    Acrylic = 1,      // 亚克力（XAML AcrylicBrush，Backdrop 源）
    Blur = 2,         // 高斯模糊（走 SetTaskbarBlur）
    Transparent = 3,  // 真透明：Fill 设为完全透明的 SolidColorBrush
};

// ── 主服务接口 ───────────────────────────────────────────────────────
// 实现在 GlassTAP.dll（跑在 explorer 进程里），hotrainy 通过 COM 远程调用。
// 参数里用 HWND 而不是 XAML 指针：跨进程只能传值。
//
// ⚠ 必须用 MIDL_INTERFACE（不能自己写 struct __declspec(uuid())）：
//   实测那样写 MSVC 会报 C2447「{ : 缺少函数标题」，
//   因为 __declspec(uuid) 与继承列表的组合在非 MIDL_INTERFACE 下解析异常。
//   MIDL_INTERFACE 是 SDK 里所有 COM 接口的标准写法，它等价于
//   struct __declspec(uuid(...)) __declspec(novtable)。
//
// 参数用 UINT 而不是 enum class：跨进程 COM 封送对枚举支持不一致，
// 用 UINT 最稳（值约定见 GlassBrush 注释）。
MIDL_INTERFACE("7a3f9c23-5b4d-4e8a-9f12-3c6d8e5a1b49")
IGlassAppearanceService : public IUnknown {
    // 设置任务栏外观。
    //   brush: GlassBrush 的值（0=SolidColor 1=Acrylic 2=Blur 3=Transparent）
    //   color: 0xAABBGGRR（ABGR，与 ACCENT_POLICY 一致）
    virtual HRESULT STDMETHODCALLTYPE SetTaskbarAppearance(
        HWND taskbar, UINT brush, UINT color) = 0;

    // 高斯模糊。blurAmount 为模糊半径（像素）。
    virtual HRESULT STDMETHODCALLTYPE SetTaskbarBlur(
        HWND taskbar, UINT color, FLOAT blurAmount) = 0;

    // 还原成系统默认外观（把记录的 originalFill 写回去）。
    virtual HRESULT STDMETHODCALLTYPE ReturnTaskbarToDefault(HWND taskbar) = 0;

    // 全部还原。进程退出前必须调，否则任务栏会留着被改过的画刷。
    virtual HRESULT STDMETHODCALLTYPE RestoreAllToDefault() = 0;

    // explorer 重启后重新注册（旧的对象全失效）
    virtual HRESULT STDMETHODCALLTYPE ResetState() = 0;
};

// 版本接口：hotrainy 先查版本再决定是否调用
MIDL_INTERFACE("7a3f9c24-5b4d-4e8a-9f12-3c6d8e5a1b4a")
IGlassVersionedApi : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVersion(DWORD* apiVersion) = 0;
};

}  // namespace xaml
}  // namespace tb
}  // namespace hr