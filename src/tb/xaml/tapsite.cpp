// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/tb/xaml/tapsite.cpp — TAP 初始化与 COM 服务实现（DLL 侧）
//
// ═══════════════════════════════════════════════════════════════════════
// 这个文件运行在 explorer.exe 进程里，负责把任务栏背景改成透明。
//
// 【技术原理（公共 API 知识，实现为原创）】
//
//  Windows 有一个公开但少有人用的机制：XAML 诊断（XAML Diagnostics）。
//  它原本给 Visual Studio 的热重载用，由 xamlOM.h 声明，属于 Windows SDK。
//
//  通过它可以拿到运行中进程的 XAML 视觉树，并读写任意元素的属性。
//  任务栏背景在这棵树里是一个名为 "BackgroundFill" 的 Rectangle，
//  它的 Fill 属性就是那层挡住壁纸的画刷。
//
//  把它换成全透明画刷 → 背景消失、壁纸透出，而图标在别的节点上，
//  完全不受影响。这是"背景透明但图标清晰"唯一可行的实现方式。
//
// 【为什么必须注入】
//
//  这棵树只存在于 explorer 进程内部，没有任何跨进程 API 能访问。
//  DWM / SetWindowCompositionAttribute / accent / blur 都作用在合成层，
//  而任务栏的暗底是 explorer 自己用 XAML 画上去的，改不到。
//  （已实测穷举：DWM 属性 1..50 全部无效。）
//
// 【初始化参数（实测确定，非猜测）】
//
//  用参数矩阵工具逐一试出来的唯一可用组合：
//    · 必须用裸 CreateThread，不能是 std::thread
//    · 线程里**不能**调用 CoInitializeEx / RoInitialize
//      —— 设成 MTA 会让该 API 返回 0x80070490 (E_NOTFOUND)
//    · wszDllXamlDiagnostics 必须传 nullptr
//    · endPointName 用 "VisualDiagConnection1"
//    · 第一次调用就会成功，不需要换名重试
// ═══════════════════════════════════════════════════════════════════════
#include "hr/tb/xaml/tapsite.h"
#include "hr/tb/xaml/visualtree.h"

#include "hr/tb/xaml/taplog.h"

#include <xamlOM.h>
#include <winrt/base.h>

// CoRegisterActiveObject 声明在 ole2.h。
// 注意 WIN32_LEAN_AND_MEAN 会把 OLE 那部分排除掉，
// 必须显式包含，否则报 C3861 "找不到标识符"。
// ACTIVEOBJECT_STRONG 在 oleauto.h。
#include <ole2.h>
#include <oleauto.h>

#include <atomic>
#include <exception>

namespace hr {
namespace tb {
namespace xaml {

namespace {

std::atomic<bool> g_installed{false};
winrt::com_ptr<IUnknown> g_site;

const wchar_t* kReadyEventName = L"GlassTAP_Ready_v1";

// 事件由 DLL 创建并置位，主程序只打开不创建。
// 反过来会让 DLL 抢在事件创建前发信号，导致主程序空等。
void signalReady() {
    // ⚠ 不要 CloseHandle。
    //
    // 命名事件对象只有在还有句柄引用时才存在。
    // 创建后立刻关闭，事件就被销毁 —— 注入器随后 OpenEvent
    // 会找不到，只能空等超时。
    // （这正是"透明已生效但注入器报超时"的成因。）
    //
    // 有意不关这一个句柄：让它保持存活，注入器才能观察到。
    HANDLE h = ::CreateEventW(nullptr, TRUE, FALSE, kReadyEventName);
    if (h) {
        ::SetEvent(h);
    } else {
        tapLogf("tap: signalReady 失败 err=%lu", ::GetLastError());
    }
}

typedef HRESULT(WINAPI* PFN_InitXamlDiagEx)(
    LPCWSTR endPointName, DWORD pid, LPCWSTR wszDllXamlDiagnostics,
    LPCWSTR wszTAPDllName, CLSID tapClsid, LPCWSTR wszInitializationData);

// 初始化线程体。
//
// 为什么必须是裸 CreateThread + 什么都不初始化：
//   参数矩阵实测结果表明，加了 CoInitializeEx/RoInitialize 之后
//   同一个调用就变成 0x80070490。该 API 自己管理线程状态，
//   调用方把它设成 MTA 反而破坏前置条件。
DWORD WINAPI installThreadProc(LPVOID param) {
    auto* fn = static_cast<PFN_InitXamlDiagEx>(param);
    if (!fn) return (DWORD)E_POINTER;

    // 自身路径：系统要靠它 LoadLibrary 并调 DllGetClassObject
    wchar_t self[MAX_PATH] = {};
    HMODULE mod = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)&installThreadProc, &mod);
    ::GetModuleFileNameW(mod, self, MAX_PATH);

    // 固定连接名。不加序号 —— 实测第一次就会成功，
    // 递增反而在失败时留下大量未清理的端点。
    HRESULT hr = fn(L"VisualDiagConnection1", ::GetCurrentProcessId(),
                    nullptr, self, CLSID_GlassTAPSite, nullptr);

    tapLogf("tap: InitializeXamlDiagnosticsEx -> 0x%08X %s",
            (unsigned)hr, SUCCEEDED(hr) ? "(成功)" : "(失败)");

    if (FAILED(hr)) signalReady();   // 失败也要唤醒主程序，避免它空等
    return (DWORD)hr;
}

}  // namespace

// ── InstallTap ───────────────────────────────────────────────────────

HRESULT InstallTap(const wchar_t* connectionName) {
    (void)connectionName;   // 连接名内部固定，参数仅为接口兼容

    if (g_installed.exchange(true)) {
        tapLogf("tap: 已安装，跳过");
        return S_OK;
    }

    tapLogf("tap: 开始安装 XAML 诊断");

    try {
        HMODULE wux = ::LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr,
                                       LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!wux) {
            DWORD e = ::GetLastError();
            tapLogf("tap: 加载 Windows.UI.Xaml.dll 失败 err=%lu", e);
            signalReady();
            return HRESULT_FROM_WIN32(e);
        }

        auto fn = (PFN_InitXamlDiagEx)(void*)::GetProcAddress(
            wux, "InitializeXamlDiagnosticsEx");
        if (!fn) {
            DWORD e = ::GetLastError();
            tapLogf("tap: 找不到 InitializeXamlDiagnosticsEx err=%lu", e);
            signalReady();
            return HRESULT_FROM_WIN32(e);
        }
        tapLogf("tap: 函数地址 %p", (void*)fn);

        HANDLE th = ::CreateThread(nullptr, 0, installThreadProc, (LPVOID)fn, 0, nullptr);
        if (!th) {
            DWORD e = ::GetLastError();
            tapLogf("tap: CreateThread 失败 err=%lu", e);
            signalReady();
            return HRESULT_FROM_WIN32(e);
        }

        // 等它跑完。不设超时上限 —— 系统在忙时会慢，但一定会结束。
        ::WaitForSingleObject(th, 15000);
        DWORD code = 0;
        ::GetExitCodeThread(th, &code);
        ::CloseHandle(th);
        return (HRESULT)code;

    } catch (const std::exception& ex) {
        // C++ 异常不能逃出线程函数，否则会 terminate 掉 explorer
        tapLogf("tap: 安装异常: %s", ex.what());
        signalReady();
        return E_FAIL;
    } catch (...) {
        tapLogf("tap: 安装未知异常");
        signalReady();
        return E_FAIL;
    }
}

bool tapInstalled() { return g_installed.load(); }

void restoreAll() { restoreAllBars(nullptr); }

// ── COM 服务（主程序通过它远程控制）─────────────────────────────────

namespace {

class AppearanceService
    : public winrt::implements<AppearanceService,
                               IGlassAppearanceService,
                               IGlassVersionedApi,
                               winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE GetVersion(DWORD* v) noexcept override {
        if (v) *v = kGlassTapApiVersion;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetTaskbarAppearance(
        HWND taskbar, UINT brush, UINT color) noexcept override {
        return taskbar ? applyToOne(taskbar, (GlassBrush)brush, color)
                       : applyToAll((GlassBrush)brush, color);
    }

    HRESULT STDMETHODCALLTYPE SetTaskbarBlur(
        HWND taskbar, UINT color, FLOAT blurAmount) noexcept override {
        // 本版把"模糊"实现为半透明纯色。
        // 真正的模糊需要 CompositionEffectBrush + 高斯效果，
        // 接口已预留，后续版本可直接替换实现而不改 ABI。
        (void)blurAmount;
        return taskbar ? applyToOne(taskbar, GlassBrush::SolidColor, color)
                       : applyToAll(GlassBrush::SolidColor, color);
    }

    HRESULT STDMETHODCALLTYPE ReturnTaskbarToDefault(HWND taskbar) noexcept override {
        return restoreAllBars(taskbar);
    }

    HRESULT STDMETHODCALLTYPE RestoreAllToDefault() noexcept override {
        return restoreAllBars(nullptr);
    }

    HRESULT STDMETHODCALLTYPE ResetState() noexcept override {
        detachDiagnostics();
        return S_OK;
    }
};

// 类工厂。系统为 CLSID_GlassTAPSite 创建 TAPSite，
// 主程序为 CLSID_GlassAppearanceService 取服务接口。
class ClassFactory : public winrt::implements<ClassFactory, IClassFactory> {
public:
    explicit ClassFactory(bool isTapSite) : m_site(isTapSite) {}

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid,
                                             void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        tapLogf("feature: CreateInstance(%s) riid={%08lX-%04X-%04X}",
                m_site ? "TAPSite" : "Service",
                (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3);

        if (m_site) {
            auto site = winrt::make_self<TAPSite>();
            HRESULT hr = site->QueryInterface(riid, ppv);
            tapLogf("feature: TAPSite QI -> 0x%08X ptr=%p", (unsigned)hr, *ppv);
            return hr;
        }
        auto svc = winrt::make_self<AppearanceService>();
        return svc->QueryInterface(riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }

private:
    bool m_site;
};

}  // namespace

// ── TAPSite：系统通过它把 IXamlDiagnostics 交给我们 ──────────────────

HRESULT STDMETHODCALLTYPE TAPSite::SetSite(IUnknown* pUnkSite) noexcept {
    // 进入函数的第一行就记日志。
    // 这条日志是判断"系统是否把 IXamlDiagnostics 交给我们"的唯一依据，
    // 如果它不出现，说明问题在系统回调之前，而不是我们的处理逻辑里。
    tapLogf("feature: SetSite 被调用 pUnkSite=%p", (void*)pUnkSite);

    try {
        g_site.copy_from(pUnkSite);
        if (!g_site) {
            tapLogf("feature: SetSite 收到空指针，断开");
            detachDiagnostics();
            return S_OK;
        }
        auto diag = g_site.try_as<IXamlDiagnostics>();
        if (!diag) {
            tapLogf("feature: SetSite 未拿到 IXamlDiagnostics");
            return E_NOINTERFACE;
        }
        tapLogf("feature: 拿到 IXamlDiagnostics，开始挂视觉树回调");
        bool ok = attachDiagnostics(diag.get());
        tapLogf("feature: 视觉树回调 %s", ok ? "已挂上" : "挂载失败");

        // 视觉树挂上之后把服务对象登记到 ROT，
        // 主程序就能通过 GetActiveObject 拿到它来查询状态、执行还原。
        //
        // 放在这个时机而不是 DllMain，是因为 ROT 注册需要 COM 已初始化，
        // 而此刻系统已经把 COM 环境准备好了（SetSite 本身就走 COM 路径）。
        if (ok) registerServiceObject();

        // ★ 立刻建立命令通道（消息窗口 + 轮询定时器）。
        //
        //   不能等业务路径去懒创建 —— 岛台开关、透明开关都是
        //   通过这条通道下发的，通道不通就什么都做不了，
        //   而"建通道"又曾经只在透明化路径上被触发，形成死锁。
        //
        //   放在这里是因为此刻已经回到正常的消息循环语境，
        //   建窗口和 SetTimer 都安全（这不是 XAML 属性写入，
        //   不受下面那条"回调栈内不能改属性"的限制）。
        if (ok) ensureChannelReady();

        // ⚠ 不要在这里立即调用 applyToAll。
        //
        // 实测：在 SetSite -> attachDiagnostics 之后马上改 Fill 会得到
        // 0x80004005 (E_FAIL)。原因是此刻仍处于 XAML 的属性变更通知
        // 派发路径上（OnVisualTreeChange 正在被调用），
        // 在回调栈内改动同一批属性会被 XAML 拒绝。
        //
        // 正确做法是等这次派发结束。visualtree.cpp 会在收到第一批
        // 树变更、且栈已退干净之后自行应用一次透明。
        signalReady();
        return ok ? (HRESULT)S_OK : (HRESULT)E_FAIL;
    } catch (const winrt::hresult_error& ex) {
        tapLogf("feature: SetSite 异常 0x%08X", (unsigned)ex.code().value);
        return ex.code().value;
    } catch (...) {
        tapLogf("feature: SetSite 未知异常");
        return (HRESULT)E_FAIL;
    }
}

HRESULT STDMETHODCALLTYPE TAPSite::GetSite(REFIID riid, void** ppvSite) noexcept {
    if (!ppvSite) return E_POINTER;
    if (!g_site) return (HRESULT)E_FAIL;
    return g_site.as(riid, ppvSite);
}

// 把服务对象注册进 ROT（Running Object Table），供主程序取用。
//
// ⚠ 不能用 CoRegisterActiveObject：现代 Windows SDK 里**没有声明它**
//   （把 Include 目录全搜了一遍，ole2.h / oleauto.h 里都没有）。
//   直接写会报 C3861 "找不到标识符"。
//
// 标准替代是 IRunningObjectTable：
//   CoGetOSTable → GetRunningObjectTable
//   → Register(0, unk, moniker, &cookie)
//
// ⚠ 反过来的坑：之前用 CoRegisterClassObject 注册"类工厂"，
//   主程序 GetActiveObject 会返回 0x800401E3 (MK_E_UNAVAILABLE) ——
//   因为类工厂不等于"已存在的实例"，ROT 里查不到。
void registerServiceObject() {
    static DWORD cookie = 0;
    if (cookie) return;

    auto svc = winrt::make_self<AppearanceService>();
    IUnknown* unk = static_cast<IGlassAppearanceService*>(svc.get());

    IRunningObjectTable* rot = nullptr;
    HRESULT hr = ::GetRunningObjectTable(0, &rot);
    if (FAILED(hr) || !rot) {
        tapLogf("feature: GetRunningObjectTable 失败 0x%08X", (unsigned)hr);
        return;
    }

    // 用一个以 CLSID 为名的 moniker 作为 ROT 的键
    IBindCtx* ctx = nullptr;
    hr = ::CreateBindCtx(0, &ctx);
    if (FAILED(hr) || !ctx) {
        tapLogf("feature: CreateBindCtx 失败 0x%08X", (unsigned)hr);
        rot->Release();
        return;
    }

    wchar_t nameBuf[64] = {0};
    ::StringFromGUID2(CLSID_GlassAppearanceService, nameBuf, 64);

    IMoniker* moniker = nullptr;
    hr = ::CreateItemMoniker(L"!", nameBuf, &moniker);
    if (SUCCEEDED(hr) && moniker) {
        hr = rot->Register(ROTFLAGS_REGISTRATIONKEEPSALIVE, unk, moniker, &cookie);
        if (SUCCEEDED(hr)) {
            tapLogf("feature: 服务对象已登记到 ROT（cookie=%lu）", cookie);
        } else {
            tapLogf("feature: ROT Register 失败 0x%08X", (unsigned)hr);
        }
        moniker->Release();
    } else {
        tapLogf("feature: CreateItemMoniker 失败 0x%08X", (unsigned)hr);
    }

    ctx->Release();
    rot->Release();
}

}  // namespace xaml
}  // namespace tb
}  // namespace hr

// ── DLL 导出 ─────────────────────────────────────────────────────────
//
// 【重要】本 DLL 是 C++/WinRT 功能层，不是引导层。
//
// 引导（建立 XAML 诊断通道）由 GlassBoot.dll 完成 —— 那是纯 C 的，
// 因为 InitializeXamlDiagnosticsEx 要求进程无 COM/WinRT 初始化痕迹，
// 而本 DLL 的 C++/WinRT 静态初始化违反了这一点。
//
// 本 DLL 只负责：
//   · DllGetClassObject：被 GlassBoot 转发调用，提供 TAPSite 与服务对象
//   · 视觉树操作（visualtree.cpp）
//
// 因此这里没有 DllMain 引导线程 —— 本 DLL 是在诊断通道建立之后
// 才被 GlassBoot 加载的，那时 C++/WinRT 初始化不再有害。

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            ::DisableThreadLibraryCalls(inst);
            // 记一笔：功能层被载入说明引导层已成功建立诊断通道，
            // 且系统开始回调 DllGetClassObject。后续 SetSite 的结果
            // 决定了视觉树能否被接管，所以这条日志是排查的关键锚点。
            hr::tb::xaml::tapLogInit();
            hr::tb::xaml::tapLogf("feature: GlassTAP.dll 已载入");
            break;
        case DLL_PROCESS_DETACH:
            // 卸载前还原，别让任务栏留着被改过的画刷
            if (hr::tb::xaml::tapInstalled()) {
                hr::tb::xaml::restoreAll();
            }
            break;
    }
    return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    using namespace hr::tb::xaml;

    bool isSite = ::IsEqualGUID(rclsid, CLSID_GlassTAPSite);
    bool isSvc = ::IsEqualGUID(rclsid, CLSID_GlassAppearanceService);

    tapLogInit();
    tapLogf("feature: DllGetClassObject clsid=%s riid={%08lX-%04X-%04X}",
            isSite ? "TAPSite" : (isSvc ? "AppearanceService" : "未知"),
            (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3);

    if (!isSite && !isSvc) return CLASS_E_CLASSNOTAVAILABLE;

    auto factory = winrt::make_self<ClassFactory>(isSite);
    HRESULT hr = factory->QueryInterface(riid, ppv);
    tapLogf("feature: 类工厂返回 0x%08X", (unsigned)hr);
    return hr;
}