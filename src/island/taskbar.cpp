// hr/taskbar.cpp — 任务栏透明服务
//
// 底层实现见 src/tb/xaml/ 与 include/hr/tb/xaml/：
//   GlassBoot.dll 注入 explorer → 建立 XAML 诊断通道
//   GlassTAP.dll 挂视觉树回调 → 定位 BackgroundFill
//   → 把该元素 Opacity 设为 0 → 背景透出壁纸，图标不受影响
//
// 本文件负责：
//   1. 开关透明
//   2. 状态展示
//   3. 退出时还原（不留副作用）
//   4. 兼容性检测
#include "hr/island/taskbar.h"

#include "hr/shared/common.h"      // 必须最先：windows.h
#include "hr/tb/xaml/channel.h"
#include "hr/tb/xaml/injector.h"

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>

namespace hr {

namespace {

bool iEquals(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

// 枚举进程名，判断某个会改写任务栏外观的工具是否在跑。
//
// 这些工具都直接操作 explorer 的 XAML 树或注入 explorer，
// 与我们的方案会在同一个对象上打架，需要提示用户。
bool isProcessRunning(const wchar_t* exeName) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (iEquals(pe.szExeFile, exeName)) { found = true; break; }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return found;
}

// 任务栏图标区的锐度指标：横向梯度的平均值。
// 用来证明"背景变了但图标没被弄糊"。
float measureIconSharpness() {
    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return 0.f;

    RECT r{};
    if (!::GetWindowRect(tray, &r)) return 0.f;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return 0.f;

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    ::GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);

    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    // 图标集中在中间偏右的按钮区
    const int x0 = w * 35 / 100;
    const int x1 = w * 66 / 100;
    double sum = 0;
    int n = 0;
    for (int y = h / 4; y < h * 3 / 4; ++y) {
        for (int x = x0; x + 1 < x1; ++x) {
            const double a = px[(static_cast<size_t>(y) * w + x) * 4 + 2];
            const double b = px[(static_cast<size_t>(y) * w + x + 1) * 4 + 2];
            sum += (a > b) ? (a - b) : (b - a);
            ++n;
        }
    }
    return n ? static_cast<float>(sum / n) : 0.f;
}

}  // namespace

// ── 实现 ─────────────────────────────────────────────────────────────

struct TaskbarService::Impl {
    std::atomic<bool> want{false};        // 用户期望：是否透明
    std::atomic<bool> active{false};      // 实际是否已应用
    mutable std::mutex mu;
    float baselineSharpness = 0.f;

    // 托盘文字颜色策略：auto / dark / light。
    // 任务栏透明后背景变成壁纸，白字在浅色壁纸上会看不清，
    // 需要按这个策略调整。
    std::wstring textColor = L"auto";

    Impl() { baselineSharpness = measureIconSharpness(); }
};

TaskbarService::TaskbarService() : impl_(new Impl()) {}

TaskbarService::~TaskbarService() { Stop(); }

void TaskbarService::Start() {
    // 开机启动时如果配置里是"开启"，这里就把效果应用上
    if (impl_->want.load()) {
        SetTransparent(true);
    }
}

void TaskbarService::Stop() {
    // 退出前一定还原，否则任务栏会留着被改过的外观。
    // shutdown() 内部已包含还原逻辑。
    tb::xaml::shutdown();
    impl_->active.store(false);
}

bool TaskbarService::running() const {
    return tb::xaml::isInjected();
}

void TaskbarService::SetTransparent(bool on) {
    impl_->want.store(on);

    if (on) {
        bool ok = tb::xaml::applyTransparent();
        impl_->active.store(ok);
        logf("taskbar: 开启透明 -> %s", ok ? "成功" : "失败");

        // 透明生效后调整托盘文字颜色。
        //
        // 这是这个功能的一半价值：背景透出壁纸后，
        // 若壁纸是浅色，系统默认的白字（时间/日期/输入法）
        // 会糊成一片看不清。按背景亮度自动切深浅色。
        //
        // 注意：DLL 侧在收到「应用透明」时已经自动做过一次。
        // 这里再调一次是为了覆盖"用户手动切策略"和
        // "壁纸换了"这两类情况 —— 重复执行是幂等的。
        if (ok) ApplyTextColor();
    } else {
        // ★ 用 restoreTransparencyOnly 而不是 restoreDefault。
        //
        //   restoreDefault 会全量还原（包括岛台按钮），
        //   但用户只是关透明化，没想关岛台。
        //
        //   restoreTransparencyOnly 只还原背景+边框，
        //   不碰岛台的开始/搜索按钮。
        bool ok = tb::xaml::restoreTransparencyOnly();
        impl_->active.store(false);
        logf("taskbar: 关闭透明（只还原透明化）-> %s", ok ? "成功" : "失败（可能本就没开）");
    }
}

bool TaskbarService::transparent() const {
    return impl_->want.load();
}

void TaskbarService::SetAlpha(int alpha) {
    // 新机制下透明是二值的：背景那层要么在要么不在，
    // 不存在"半透明程度"这个参数。
    // 保留接口是为了不动配置格式与调用点，
    // 但明确记日志，避免以后误以为它生效。
    (void)alpha;
    logf("taskbar: SetAlpha 在新的透明实现下不生效（透明为二值），已忽略 alpha=%d", alpha);
}

int TaskbarService::alpha() const {
    return 255;
}

float TaskbarService::iconSharpnessRatio() const {
    const float base = impl_->baselineSharpness;
    if (base <= 0.01f) return 0.f;
    return measureIconSharpness() / base;
}

void TaskbarService::SetTextColor(const std::wstring& mode) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (mode == L"dark" || mode == L"light" || mode == L"auto") {
        impl_->textColor = mode;
    } else {
        impl_->textColor = L"auto";   // 非法值回落
    }
    logw(L"taskbar: 文字颜色策略设为 %s", impl_->textColor.c_str());
}

std::wstring TaskbarService::textColor() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->textColor;
}

bool TaskbarService::ApplyTextColor() {
    std::wstring mode;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        mode = impl_->textColor;
    }

    // 文字颜色只有在注入成功（DLL 在 explorer 里）时才谈得上改。
    if (!tb::xaml::isInjected() && !tb::xaml::hasListener()) {
        logf("taskbar: 未注入，跳过文字颜色调整");
        return false;
    }

    using Cmd = tb::xaml::ChannelCmd;
    Cmd cmd = Cmd::AutoTextColor;
    if (mode == L"dark")  cmd = Cmd::SetTextDark;
    if (mode == L"light") cmd = Cmd::SetTextLight;

    tb::xaml::ChannelResult r = tb::xaml::channelSend(cmd, 0, 3000);
    bool ok = r.ok && SUCCEEDED(r.hr);
    logf("taskbar: 应用文字颜色(%s) -> %s",
         mode == L"dark" ? "dark" : (mode == L"light" ? "light" : "auto"),
         ok ? "成功" : "失败");
    return ok;
}

void TaskbarService::Refresh() {
    // explorer 重启后，之前注入的 DLL 与诊断通道都会失效，
    // 需要重新走一遍注入。
    if (tb::xaml::isInjected()) {
        tb::xaml::releaseService();
    }
    if (impl_->want.load()) {
        SetTransparent(true);
    }
}

void TaskbarService::ForceReset() {
    // 场景：上一个实例被强杀（任务管理器结束任务），它的 Stop() 没执行，
    // explorer 里的 DLL 还在跑、任务栏还是透明的。
    //
    // 处理办法：通过共享内存通道直接向那份残留的 DLL 发「还原」指令。
    //
    // ⚠ 为什么能成：通道是命名的（Local\GlassTAP_Channel_v1），
    //   跨进程共享。新实例即使自己没注入过，也能连上同一个通道，
    //   而 explorer 里那份 DLL 正在监听它。
    //
    // ⚠ 为什么不在这里做完整注入：实测那样会拖慢启动约 20 秒
    //   （注入要等引导就绪），而且即使注入成功、透明也生效了，
    //   用户看到的却是"启动卡住"。发一条通道指令只要几百毫秒。
    if (impl_->want.load()) {
        // 本次启动就要透明，后面的 SetTransparent 会覆盖任何残留状态。
        return;
    }

    // 先探一下通道对面有没有人在听。
    // 没有残留 DLL 时这一步会超时返回，属正常情况，不记成错误。
    if (!tb::xaml::hasListener()) {
        logf("taskbar: 启动检查完成（无残留透明状态）");
        return;
    }

    bool ok = tb::xaml::restoreDefault();
    logf("taskbar: 检测到上次运行残留的透明状态，已复位 -> %s",
         ok ? "成功" : "通道无响应");
    impl_->active.store(false);
}

std::wstring TaskbarService::StatusText() const {
    if (!impl_->want.load()) return L"跟随系统";

    if (impl_->active.load()) {
        return L"背景已完全透明";
    }

    // 期望开启但没生效 —— 给出可操作的原因
    if (!::FindWindowW(L"Shell_TrayWnd", nullptr)) {
        return L"未找到任务栏";
    }
    return L"透明未生效（详见日志）";
}

std::vector<std::wstring> TaskbarService::CompatNotes() const {
    std::vector<std::wstring> notes;

    // 这些工具同样会接管任务栏外观，与本方案冲突。
    if (isProcessRunning(L"TranslucentTB.exe")) {
        notes.push_back(L"检测到 TranslucentTB 正在运行，它同样会改写任务栏外观，请只保留一个");
    }
    if (isProcessRunning(L"RoundedTB.exe")) {
        notes.push_back(L"检测到 RoundedTB 正在运行，可能与透明效果冲突");
    }
    if (isProcessRunning(L"ExplorerPatcher.exe")) {
        notes.push_back(L"检测到 ExplorerPatcher 正在运行，它替换了任务栏实现，透明可能无效");
    }
    if (isProcessRunning(L"StartAllBack.exe")) {
        notes.push_back(L"检测到 StartAllBack 正在运行，它接管了任务栏绘制，透明可能无效");
    }

    return notes;
}

// ── 进程内唯一实例 ───────────────────────────────────────────────────

TaskbarService& taskbarService() {
    static TaskbarService instance;
    return instance;
}

}  // namespace hr