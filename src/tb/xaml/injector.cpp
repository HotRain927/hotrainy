// hr/tb/xaml/injector.cpp — 把 GlassBoot.dll 注入 explorer 并维持其常驻
//
// ═══════════════════════════════════════════════════════════════════════
// 【注入方式与常驻要求】
//
// 1. SetWindowsHookEx(WH_CALLWNDPROC, <DLL 导出的钩子过程>, GlassBoot.dll, explorer线程)
//    系统随即把 GlassBoot.dll 映射进 explorer 并调用它的 DllMain。
//
//    ⚠ 钩子过程必须来自 DLL（GlassBoot_HookProc），不能是本程序里的函数。
//      用本程序的函数时，Windows 无法把钩子投递进 explorer，
//      只会短暂映射 DLL 就卸载 —— 引导线程来不及跑，什么都不发生。
//      （实测现象：boot.log 停在 "thread started"。）
//
// 2. GlassBoot.dll 的 DllMain 在 explorer 里同步调用
//    InitializeXamlDiagnosticsEx，建立 XAML 诊断通道。
//
// 3. 系统接着回调它的 DllGetClassObject(CLSID_GlassTAPSite)，
//    该函数转发给 GlassTAP.dll，由后者提供 TAPSite。
//
// 4. 系统调用 TAPSite::SetSite(IXamlDiagnostics*)，
//    GlassTAP 挂上视觉树回调并定位任务栏的 BackgroundFill。
//
// 5. 定位成功后，把该元素的 Opacity 设为 0 —— 背景消失，图标不动。
//
// 【为什么必须保持钩子常驻】
//   释放钩子会让系统卸载 GlassBoot.dll，进而 GlassTAP.dll 和
//   TAPSite 一起失效，任务栏会回到原样。
//   所以钩子的生命周期 = 透明效果的生命周期。
// ═══════════════════════════════════════════════════════════════════════
#include "hr/tb/xaml/injector.h"

#include "hr/shared/common.h"
#include "hr/tb/xaml/interfaces.h"
#include "hr/tb/xaml/channel.h"

#include <objbase.h>

#include <mutex>
#include <string>

namespace hr {
namespace tb {
namespace xaml {

namespace {

const wchar_t* kBootDllName = L"GlassBoot.dll";
const wchar_t* kReadyEventName = L"GlassTAP_Ready_v1";
const char* kHookProcName = "GlassBoot_HookProc";

typedef LRESULT(CALLBACK* PFN_HookProc)(int, WPARAM, LPARAM);

std::mutex g_mu;
HMODULE g_bootModule = nullptr;
HHOOK g_hook = nullptr;
IGlassAppearanceService* g_service = nullptr;
bool g_injected = false;

std::wstring bootDllPath() {
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring p(self);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos + 1);
    return p + kBootDllName;
}

// 等待引导层报告结果。事件由 DLL 侧创建并置位，
// 这里只轮询打开，避免 DLL 抢在事件创建前发信号导致丢信号。
bool readyEventSet() {
    HANDLE ev = ::OpenEventW(SYNCHRONIZE, FALSE, kReadyEventName);
    if (!ev) return false;
    DWORD r = ::WaitForSingleObject(ev, 0);
    ::CloseHandle(ev);
    return r == WAIT_OBJECT_0;
}

void clearReady() {
    HANDLE ev = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kReadyEventName);
    if (ev) {
        ::ResetEvent(ev);
        ::CloseHandle(ev);
    }
}

// 给目标线程喂消息，让钩子有机会被投递、DLL 保持映射。
void pump(HWND tray, DWORD tid) {
    if (tray) {
        ::SendMessageTimeoutW(tray, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 500, nullptr);
    }
    if (tid) {
        ::PostThreadMessageW(tid, WM_NULL, 0, 0);
    }
}

// 取回跨进程服务接口。
//
// TAPSite 只是引导用的钩子，真正对外的是 AppearanceService。
// 它由 GlassTAP.dll 在 SetSite 成功后登记进 ROT。
//
// ⚠ 用 ROT 而不是 GetActiveObject：
//   GetActiveObject 依赖 CoRegisterActiveObject，而现代 Windows SDK
//   里没有声明那个 API。ROT（Running Object Table）是等价的公开途径，
//   而且更明确 —— 键就是我们自己的 CLSID。
IGlassAppearanceService* acquireService() {
    IRunningObjectTable* rot = nullptr;
    HRESULT hr = ::GetRunningObjectTable(0, &rot);
    if (FAILED(hr) || !rot) {
        logf("inject: GetRunningObjectTable 失败 0x%08X", (unsigned)hr);
        return nullptr;
    }

    IBindCtx* ctx = nullptr;
    hr = ::CreateBindCtx(0, &ctx);
    if (FAILED(hr) || !ctx) {
        rot->Release();
        return nullptr;
    }

    wchar_t nameBuf[64] = {0};
    ::StringFromGUID2(CLSID_GlassAppearanceService, nameBuf, 64);

    IMoniker* moniker = nullptr;
    IUnknown* unk = nullptr;
    hr = ::CreateItemMoniker(L"!", nameBuf, &moniker);
    if (SUCCEEDED(hr) && moniker) {
        hr = rot->GetObject(moniker, &unk);
        moniker->Release();
    }
    ctx->Release();
    rot->Release();

    if (FAILED(hr) || !unk) {
        logf("inject: ROT 里查不到服务对象 0x%08X", (unsigned)hr);
        return nullptr;
    }

    IGlassAppearanceService* svc = nullptr;
    hr = unk->QueryInterface(__uuidof(IGlassAppearanceService), (void**)&svc);
    unk->Release();
    if (FAILED(hr) || !svc) {
        logf("inject: QI IGlassAppearanceService 失败 0x%08X", (unsigned)hr);
        return nullptr;
    }

    DWORD ver = 0;
    IGlassVersionedApi* verApi = nullptr;
    if (SUCCEEDED(svc->QueryInterface(__uuidof(IGlassVersionedApi), (void**)&verApi)) && verApi) {
        verApi->GetVersion(&ver);
        verApi->Release();
    }
    if (ver != kGlassTapApiVersion) {
        logf("inject: 版本不匹配 DLL=%lu 期望=%lu", ver, (unsigned)kGlassTapApiVersion);
        svc->Release();
        return nullptr;
    }

    return svc;
}

// 探测通道对面是否有 DLL 在监听（不记日志的版本，供注入流程内部使用）。
bool channelHasListener() {
    ChannelResult r = channelSend(ChannelCmd::QueryState, 0, /*timeoutMs=*/800);
    return r.ok;
}

}  // namespace

// ── 注入 ─────────────────────────────────────────────────────────────

bool injectIntoExplorer() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_injected && g_hook) return true;

    // ⚠ 先检查 explorer 里是否已有我们残留的 DLL 在监听通道。
    //
    //   场景：上一个实例被强杀，它的 DLL 还留在 explorer 里、
    //   诊断通道也还开着。这时如果直接走完整注入流程，
    //   SetWindowsHookEx 会失败（同样的模块/钩子已存在），
    //   结果是"明明有可用的 DLL，却报注入失败"。
    //
    //   通道能应答就说明那份 DLL 是活的，直接复用它。
    //   XAML 诊断每个连接只能建立一次，重复注入本来也建不起来。
    if (channelHasListener()) {
        logf("inject: 检测到 explorer 内已有的 DLL，直接复用");
        g_injected = true;
        return true;
    }

    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        logf("inject: 找不到任务栏（explorer 未运行？）");
        return false;
    }
    DWORD pid = 0;
    DWORD tid = ::GetWindowThreadProcessId(tray, &pid);
    if (!tid) {
        logf("inject: 取不到 explorer 线程");
        return false;
    }

    if (!g_bootModule) {
        std::wstring path = bootDllPath();
        g_bootModule = ::LoadLibraryExW(path.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!g_bootModule) {
            logw(L"inject: 加载 %s 失败 err=%lu", path.c_str(), ::GetLastError());
            return false;
        }
        logw(L"inject: 已加载 %s", path.c_str());
    }

    // 钩子过程必须来自 DLL，否则 explorer 拿不到它，
    // DLL 只会被短暂映射后卸载。
    auto hookProc = (PFN_HookProc)(void*)::GetProcAddress(g_bootModule, kHookProcName);
    if (!hookProc) {
        logf("inject: %hs 未导出，无法保持 DLL 常驻", kHookProcName);
        return false;
    }

    // ★★ 重试循环：装钩子 + 等待就绪，最多 3 次 ★★
    //
    //   踩过的坑：引导等待是偶发超时的 ——
    //   explorer 刚启动、目标线程正忙的时候，钩子投递不进去，
    //   8 秒等完就失败。而一旦失败，透明化和岛台**全部失效**，
    //   用户看到的就是"开关没反应"，非常难查。
    //
    //   单次失败不代表环境坏了，重试一次通常就好。
    //   每次失败先卸钩子（不留驻留 DLL），等 1.5 秒再试。
    bool ready = false;
    for (int attempt = 1; attempt <= 3 && !ready; ++attempt) {
        clearReady();

        g_hook = ::SetWindowsHookExW(WH_CALLWNDPROC, hookProc, g_bootModule, tid);
        if (!g_hook) {
            logf("inject: SetWindowsHookEx 失败 err=%lu（第 %d 次）",
                 ::GetLastError(), attempt);
            ::Sleep(1500);
            continue;
        }
        logf("inject: 钩子已装载 %p（explorer pid=%lu tid=%lu，第 %d 次）",
             (void*)g_hook, pid, tid, attempt);

        // 持续喂消息，直到引导层报告结果。
        // 钩子只在有消息经过时才被投递，不喂消息 DLL 就不会真正跑起来。
        //
        // 单次超时取 8 秒：正常情况下一两百毫秒内就会就绪
        // （引导层在 DllMain 里同步完成初始化）。
        for (int i = 0; i < 40; ++i) {       // 40 × 200ms = 8s
            pump(tray, tid);
            if (readyEventSet()) { ready = true; break; }
            ::Sleep(200);
        }

        logf("inject: 等待引导就绪 -> %s（第 %d 次）",
             ready ? "成功" : "超时", attempt);

        if (!ready) {
            // 失败必须把钩子卸掉。
            // 留着它会让 DLL 继续驻留 explorer，状态不明。
            ::UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
            ::Sleep(1500);
        }
    }

    if (!ready) {
        return false;
    }

    // 继续喂一会儿消息，让 SetSite 与视觉树挂载完成。
    for (int i = 0; i < 15; ++i) {
        pump(tray, tid);
        ::Sleep(100);
    }

    g_service = acquireService();
    g_injected = true;

    if (g_service) {
        logf("inject: 服务接口已就绪");
    } else {
        // 服务接口拿不到不影响透明效果 ——
        // 引导层在挂上视觉树后已经自动应用过一次。
        // 这里只记日志，仍视为注入成功，以便还原时不漏掉清理。
        logf("inject: 未取到服务接口，透明效果仍由 DLL 内部维持");
    }
    return true;
}

bool isInjected() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_injected && g_hook != nullptr;
}

IGlassAppearanceService* service() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_service;
}

// ── 高层操作 ─────────────────────────────────────────────────────────

namespace {

template <typename Fn>
bool forEachTaskbar(Fn&& fn) {
    bool any = false;
    if (HWND h = ::FindWindowW(L"Shell_TrayWnd", nullptr)) {
        if (fn(h)) any = true;
    }
    HWND prev = nullptr;
    while (true) {
        HWND h = ::FindWindowExW(nullptr, prev, L"Shell_SecondaryTrayWnd", nullptr);
        if (!h) break;
        if (fn(h)) any = true;
        prev = h;
    }
    return any;
}

}  // namespace

bool applyTransparent() {
    if (!injectIntoExplorer()) {
        logf("xaml: 注入失败，无法应用透明");
        return false;
    }

    // 透明效果在引导层挂上视觉树时已自动应用过一次。
    //
    // 这里再通过通道显式下发一次，覆盖"注入时任务栏尚未创建"、
    // 或"用户手动关掉后又打开"这类情况。
    ChannelResult r = channelSend(ChannelCmd::ApplyTransparent);
    if (!r.ok) {
        // 通道没响应不代表透明没生效 —— 引导层已经自动应用过。
        // 所以这里只记日志，不返回失败。
        logf("xaml: 透明指令未获应答，依赖 DLL 内部已应用的效果");
        return true;
    }
    logf("xaml: 透明指令完成 hr=0x%08X", (unsigned)r.hr);
    return SUCCEEDED(r.hr);
}

bool restoreDefault() {
    // 通过共享内存通道把「还原」指令送给 explorer 里的 DLL。
    //
    // 不走 COM：自定义接口跨进程需要代理桩，代价远大于收益。
    // 详见 channel.h 顶部的说明。
    //
    // ⚠ 这里**不能**用 "if (!isInjected()) return false" 提前退出。
    //
    //   通道是命名的（Local\GlassTAP_Channel_v1），跨进程共享。
    //   常见场景：上一个 hotrainy 实例被强杀（任务管理器结束任务），
    //   它的 shutdown() 没执行 —— 钩子还挂在 explorer 里、DLL 还在跑、
    //   任务栏还是透明的。而本次启动的实例自己没注入过，isInjected()
    //   为假。
    //
    //   如果这时直接返回，那份残留的透明就永远清不掉，
    //   用户会看到"关掉开关也没用"。
    //
    //   所以无论自己有没有注入，都先试着发一次指令 ——
    //   通道另一头有 DLL 在听就会回应，没人听就超时返回。
    ChannelResult r = channelSend(ChannelCmd::RestoreDefault, 0, /*timeoutMs=*/2000);
    if (!r.ok) {
        logf("xaml: 还原指令无响应（explorer 内无 DLL 在监听，属正常情况）");
        return false;
    }
    logf("xaml: 还原指令完成 hr=0x%08X", (unsigned)r.hr);
    return SUCCEEDED(r.hr);
}

bool restoreTransparencyOnly() {
    // ⚠ 没注入就直接返回 —— 没有 DLL 就没有任何残留状态要还原。
    //
    //   踩过的坑：冷启动时这里会在注入**之前**发指令，
    //   通道另一头没人听，白白等 2 秒超时，日志里一条"无响应"。
    //
    //   和 restoreDefault() 不同：restoreDefault 要清理"上一个实例
    //   强杀后残留的 DLL"，所以即使自己没注入也要发；
    //   而 restoreTransparencyOnly 只服务于"运行时关透明化"，
    //   那种场景下 DLL 必然已经在（本实例注入过）。
    if (!isInjected()) {
        return true;
    }

    // 只还原透明化，不碰岛台按钮。
    //
    // 用途：用户在设置里关透明化时调用。
    // 和 restoreDefault() 的区别：
    //   restoreDefault()          → 全量还原（启动清理/退出清理）
    //   restoreTransparencyOnly() → 只还原透明化（运行时切换）
    ChannelResult r = channelSend(ChannelCmd::RestoreTransparencyOnly, 0, 2000);
    if (!r.ok) {
        logf("xaml: 只还原透明化指令无响应");
        return false;
    }
    logf("xaml: 只还原透明化完成 hr=0x%08X", (unsigned)r.hr);
    return SUCCEEDED(r.hr);
}

bool hasListener() {
    // 探测 explorer 里有没有我们残留的 DLL 在监听通道。
    //
    // 用途：上一个实例被强杀时，它的 DLL 仍留在 explorer 里、
    // 任务栏还是透明的。新实例启动时用这个判断要不要先清理。
    //
    // 超时取 800ms：DLL 侧轮询间隔 120ms，正常一两个周期内就有回应。
    ChannelResult r = channelSend(ChannelCmd::QueryState, 0, /*timeoutMs=*/800);
    if (r.ok) {
        logf("xaml: 通道探测到残留 DLL（已识别 %d 个任务栏）", (int)r.hr);
    }
    return r.ok;
}

bool setIslandTakeover(bool on) {
    // 前置条件：DLL 必须在 explorer 里才谈得上改任务栏元素。
    //
    // 与 restoreDefault 不同，这里**不**尝试对"残留 DLL"发指令 ——
    // 岛台接管是本次会话的状态，不该由上一个实例的残留 DLL 承担。
    // 没有注入就先注入。
    if (!isInjected()) {
        if (!injectIntoExplorer()) {
            logf("xaml: 岛台接管失败 —— 无法注入 explorer");
            return false;
        }
    }

    ChannelResult r = channelSend(ChannelCmd::SetIslandTakeover, on ? 1u : 0u, 3000);
    if (!r.ok) {
        logf("xaml: 岛台接管指令无响应");
        return false;
    }
    logf("xaml: 岛台接管=%d 完成 hr=0x%08X", (int)on, (unsigned)r.hr);
    return SUCCEEDED(r.hr);
}

bool consumeIslandTap() {
    // 按钮的 Tapped 回调在 explorer 里置位，这里取走。
    //
    // ⚠ 超时给 100ms。
    //   这是每 150ms 一次的轮询，如果给 500ms，
    //   一旦 DLL 忙就会把 UI 线程卡住（WM_TIMER 在 UI 线程串行跑），
    //   表现出来就是"拖面板边框一顿一顿的"。
    //   正常情况 DLL 毫秒级应答，100ms 足够。
    ChannelResult r = channelSend(ChannelCmd::ConsumeIslandTap, 0, 100);
    return r.ok && (int)r.hr == 1;
}

bool islandButtonRect(RECT& out) {
    out = {0, 0, 0, 0};

    // ⚠ 这个查询要拿 XAML 的 ActualWidth / TransformToVisual，
    //   必须在 XAML 线程上做 —— 所以走通道让 DLL 去查。
    //
    //   拿到的是 XAML 树内坐标，还要叠加任务栏窗口的屏幕位置才能用。
    //   （实测 TransformToVisual(nullptr) 返回的是树内坐标，
    //     不是屏幕坐标 —— 详见 docs/岛台注入任务栏实现记录.md）
    ChannelResult r = channelSend(ChannelCmd::QueryIslandButtonRect, 0, 1500);
    if (!r.ok) return false;

    const unsigned packed = (unsigned)r.hr;
    const int w = (int)(packed >> 16);
    const int h = (int)(packed & 0xFFFF);
    if (w <= 0 || h <= 0) return false;

    // 任务栏窗口的屏幕位置
    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;
    RECT tr{};
    if (!::GetWindowRect(tray, &tr)) return false;

    // 按钮贴任务栏左端（我们在 XAML 里设的 Left + Margin）
    out.left = tr.left + 12;      // Margin 8 DIP @150% ≈ 12px
    out.top = tr.top + (tr.bottom - tr.top - h) / 2;
    out.right = out.left + w;
    out.bottom = out.top + h;
    return true;
}

void releaseService() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_service) {
        g_service->Release();
        g_service = nullptr;
    }
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);

    // 顺序很重要：
    //   1. 先还原外观（通道还通着，DLL 还在 explorer 里）
    //   2. 再关通道
    //   3. 最后卸钩子 —— 一卸 DLL 就被系统回收
    if (g_hook) {
        ChannelResult r = channelSend(ChannelCmd::RestoreDefault);
        logf("xaml: 退出前还原 -> %s (hr=0x%08X)",
             r.ok ? "已下发" : "通道无响应", (unsigned)r.hr);
    }

    channelShutdown();

    if (g_service) {
        g_service->Release();
        g_service = nullptr;
    }

    if (g_hook) {
        ::UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }

    g_injected = false;
    logf("xaml: 已关闭（外观已还原，钩子已卸）");
}

// 查「开始按钮原来占的那个坑」的屏幕位置和尺寸。
//
// 岛台按钮就摆在这里 —— 位置是从开始按钮实测出来的，
// 所以视觉上和图标组天然对齐，不需要猜任何像素。
//
// ⚠ 必须在把开始按钮 Collapsed 之前查询。
//   隐藏之后 ActualWidth 变 0，坐标也不再更新。
bool startButtonSlot(RECT& out) {
    out = {0, 0, 0, 0};

    // ⚠ 超时降到 120ms —— 这是**每 500ms 一次的轮询**，
    //   绝不能阻塞 UI 线程。
    //
    //   踩过的坑：原来给 1500ms，DLL 稍慢就把消息循环卡住 1.5 秒。
    //   表现是"鼠标移到屏幕顶部唤出标签栏时一顿一顿的"——
    //   因为标签栏的自动唤出跑在同一个线程上。
    //
    //   正常情况 DLL 毫秒级应答。真超时就跳过这轮，
    //   500ms 后再问 —— 位置跟踪慢一拍无所谓。
    ChannelResult r = channelSend(ChannelCmd::QueryStartSlot, 0, 120);
    if (!r.ok || (int)r.hr != 1) return false;

    // 坐标不在 HRESULT 里，在共享内存的四个字段上
    ChannelShared* sh = channelShared();
    if (!sh) return false;

    out.left = sh->rectLeft;
    out.top = sh->rectTop;
    out.right = sh->rectRight;
    out.bottom = sh->rectBottom;

    if (out.right <= out.left || out.bottom <= out.top) return false;

    // 只在位置变化时打日志。
    //
    // ⚠ 这个函数被 500ms 的跟踪定时器反复调用，
    //   每次都打会把日志刷爆（实测 1 秒 2 条，几分钟就几 MB）。
    //   但完全不打又不利于排查 —— 所以记下上次的值，变了才打。
    static RECT lastSlot = {0, 0, 0, 0};
    if (out.left != lastSlot.left || out.top != lastSlot.top ||
        out.right != lastSlot.right || out.bottom != lastSlot.bottom) {
        logf("inject: 开始按钮坑位 (%d,%d)-(%d,%d) %dx%d%s",
             out.left, out.top, out.right, out.bottom,
             out.right - out.left, out.bottom - out.top,
             (lastSlot.right == 0) ? "" : "  ← 位置有变化");
        lastSlot = out;
    }
    return true;
}



// 取走"任务栏布局变了"的标志。
//
// 图标增减时，DLL 的视觉树回调会看到
// TaskListButton 被 Add/Remove，立刻置位。
// 这里取走 —— 比 500ms 轮询快得多，而且不会漏掉一次变化。
bool consumeLayoutChanged() {
    // 超时 120ms —— 和 startButtonSlot 一样，
    // 这是高频轮询，绝不能阻塞 UI 线程。
    ChannelResult r = channelSend(ChannelCmd::ConsumeLayoutChanged, 0, 120);
    return r.ok && (int)r.hr == 1;
}
}  // namespace xaml
}  // namespace tb
}  // namespace hr