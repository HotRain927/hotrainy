// tools/settings_shot.cpp — 单独把设置窗口跑起来并截图，用来核对版式
// 只链接绘制相关的几个模块，不碰标签栏 / 托盘 / 热键，可以和应用实例同时跑。
#include "hr/common.h"   // 必须最先：引入 windows.h

#include <objidl.h>      // 必须在 gdiplus.h 之前，否则 PROPID 未定义
#include <gdiplus.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "hr/config.h"
#include "hr/settings.h"
#include "hr/taskbar.h"

using namespace hr;

namespace {

int getEncoderClsid(const wchar_t* mime, CLSID* out) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *out = codecs[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

bool savePng(const wchar_t* path, int x, int y, int w, int h) {
    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = ::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
    ::SelectObject(mem, old);

    bool ok = false;
    {
        Gdiplus::Bitmap gb(bmp, nullptr);
        CLSID clsid;
        if (getEncoderClsid(L"image/png", &clsid) >= 0) {
            ok = gb.Save(path, &clsid, nullptr) == Gdiplus::Ok;
        }
    }
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return ok;
}

SettingsWindow* g_win = nullptr;
ULONG_PTR g_gdi = 0;

}  // namespace

int main(int argc, char** argv) {
    std::wstring path = L"settings.png";
    if (argc > 1) path = toWide(argv[1]);

    logInit();
    enablePerMonitorDpi();
    dpiScale();

    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdi, &gsi, nullptr);

    Config cfg;
    std::vector<std::string> warns;
    loadConfig(cfg, warns);
    cfg.taskbarTransparent = true;

    // 真把引擎跑起来，这样状态行显示的是实测结果而不是"跟随系统"
    taskbarService().Start();
    taskbarService().SetTransparent(true);
    ::Sleep(1200);

    SettingsWindow win;
    win.onSettingChanged = [](const std::string&) -> bool { return true; };
    win.onQuit = []() { PostQuitMessage(0); };
    win.setActiveTab(0);
    if (!win.open(GetModuleHandleW(nullptr), &cfg, true, "11.2")) {
        printf("open failed\n");
        return 1;
    }
    g_win = &win;

    // 跑 900ms 消息循环让玻璃底图合成完成，再整屏抓窗体的区域
    ULONGLONG t0 = ::GetTickCount64();
    MSG msg;
    while (::GetTickCount64() - t0 < 900) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(10);
    }

    RECT rc{};
    ::GetWindowRect(win.hwnd(), &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    bool ok = savePng(path.c_str(), rc.left, rc.top, w, h);
    printf("window %dx%d at (%ld,%ld) -> %s %s\n", w, h, (long)rc.left, (long)rc.top,
           toUtf8(path).c_str(), ok ? "OK" : "FAIL");

    win.close();
    ::Sleep(120);
    taskbarService().Stop();   // 恢复系统默认任务栏
    Gdiplus::GdiplusShutdown(g_gdi);
    return ok ? 0 : 2;
}
