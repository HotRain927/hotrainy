// tools/win_shot.cpp — 截指定窗口（按类名找）
//
// 用法：win_shot.exe <输出路径> <窗口类名> [边距]
//
// ⚠ 必须先声明 DPI 感知：
//   不声明的话 GetWindowRect 返回的是虚拟化坐标，
//   本机 2560x1600@150% 会被报成 1707x1067，
//   截出来的区域完全不对，看起来就像"窗口位置或尺寸错了"。
#include <windows.h>
#include <gdiplus.h>

#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "gdiplus.lib")

static ULONG_PTR g_token = 0;

static bool savePng(HBITMAP bmp, const wchar_t* path) {
    Gdiplus::Bitmap image(bmp, nullptr);
    CLSID clsid{};
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &clsid);
    return image.Save(path, &clsid, nullptr) == Gdiplus::Ok;
}

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_token, &input, nullptr);

    if (argc < 3) {
        wprintf(L"用法: win_shot.exe <输出.png> <窗口类名> [边距]\n");
        return 1;
    }
    const wchar_t* out = argv[1];
    const wchar_t* cls = argv[2];
    const int margin = (argc > 3) ? _wtoi(argv[3]) : 0;

    // 支持两种模式：
    //   win_shot.exe out.png HotrainyIsland [margin]   按窗口类名
    //   win_shot.exe out.png rect:0,1019,600,48 [0]    按屏幕矩形
    RECT r{};
    if (wcsncmp(cls, L"rect:", 5) == 0) {
        int x = 0, y = 0, w = 0, hh = 0;
        if (swscanf_s(cls + 5, L"%d,%d,%d,%d", &x, &y, &w, &hh) != 4) {
            wprintf(L"rect 格式错误，应为 rect:x,y,w,h\n");
            return 1;
        }
        r.left = x; r.top = y; r.right = x + w; r.bottom = y + hh;
        wprintf(L"rect %dx%d @ (%d,%d)\n", w, hh, x, y);
    } else {
        HWND h = ::FindWindowW(cls, nullptr);
        if (!h) {
            wprintf(L"找不到窗口类 %s\n", cls);
            return 1;
        }
        ::GetWindowRect(h, &r);
    }
    wprintf(L"%s: %dx%d @ (%d,%d)\n",
            cls, r.right - r.left, r.bottom - r.top, r.left, r.top);

    RECT cap = {r.left - margin, r.top - margin, r.right + margin, r.bottom + margin};
    const int w = cap.right - cap.left;
    const int hh = cap.bottom - cap.top;
    if (w <= 0 || hh <= 0) {
        wprintf(L"尺寸无效\n");
        return 1;
    }

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, hh);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, hh, screen, cap.left, cap.top, SRCCOPY);
    ::SelectObject(mem, old);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    if (savePng(bmp, out)) wprintf(L"已保存 %s (%dx%d)\n", out, w, hh);
    else wprintf(L"保存失败\n");
    ::DeleteObject(bmp);

    Gdiplus::GdiplusShutdown(g_token);
    return 0;
}