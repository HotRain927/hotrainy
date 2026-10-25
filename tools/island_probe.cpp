// tools/island_probe.cpp — 探测岛台的交互（点按钮 / 右键 / 拖边框）
//
// 用法：island_probe.exe <动作>
//   power   点右下角电源按钮
//   user    点左下角用户按钮
//   add     在网格空白处右键（应弹出"添加快捷方式"）
//   resize  拖右下角改大小
//   close   点右上角 ×
#include <windows.h>

#include <cstdio>
#include <cstring>

static HWND findIsland() {
    return ::FindWindowW(L"HotrainyIsland", nullptr);
}

static void clickClient(HWND h, int x, int y, bool right) {
    POINT pt = {x, y};
    ::SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x, pt.y));
    if (right) {
        ::SendMessageW(h, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(pt.x, pt.y));
        ::SendMessageW(h, WM_RBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    } else {
        ::SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
        ::SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    }
}

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (argc < 2) {
        wprintf(L"用法: island_probe.exe <power|user|add|close>\n");
        return 1;
    }

    HWND h = findIsland();
    if (!h) {
        wprintf(L"岛台没开\n");
        return 1;
    }

    RECT r{};
    ::GetWindowRect(h, &r);
    const int w = r.right - r.left;
    const int hh = r.bottom - r.top;

    // 检查 DPI：按 100% 算的位置在 150% 下会偏
    HDC dc = ::GetDC(nullptr);
    const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
    ::ReleaseDC(nullptr, dc);
    const int S = dpi / 96;   // 缩放倍数

    wprintf(L"岛台 %dx%d @ (%d,%d)  DPI=%d (缩放 %d%%)\n",
            w, hh, r.left, r.top, dpi, dpi * 100 / 96);

    const wchar_t* act = argv[1];

    if (wcscmp(act, L"power") == 0) {
        // 右下角按钮：右边距 12*S，按钮 120x32*S
        const int x = w - (12 + 60) * S;
        const int y = hh - (12 + 16) * S;
        wprintf(L"点电源按钮 (%d,%d)\n", x, y);
        clickClient(h, x, y, false);
    } else if (wcscmp(act, L"user") == 0) {
        const int x = (12 + 60) * S;
        const int y = hh - (12 + 16) * S;
        wprintf(L"点用户按钮 (%d,%d)\n", x, y);
        clickClient(h, x, y, false);
    } else if (wcscmp(act, L"close") == 0) {
        const int x = w - (10 + 12) * S;
        const int y = (10 + 12) * S;
        wprintf(L"点关闭按钮 (%d,%d)\n", x, y);
        clickClient(h, x, y, false);
    } else if (wcscmp(act, L"add") == 0) {
        // 网格中间空白处右键
        const int x = (132 + 300) * S;
        const int y = (52 + 200) * S;
        wprintf(L"网格空白处右键 (%d,%d)\n", x, y);
        clickClient(h, x, y, true);
    } else {
        wprintf(L"未知动作 %s\n", act);
        return 1;
    }

    ::Sleep(900);

    // 菜单是独立的玻璃窗口，看看有没有新窗口冒出来
    HWND menu = ::FindWindowW(L"HotrainyGlassMenu", nullptr);
    if (!menu) menu = ::FindWindowW(L"HotrainyMenu", nullptr);
    if (menu && ::IsWindowVisible(menu)) {
        RECT mr{};
        ::GetWindowRect(menu, &mr);
        wprintf(L"菜单已弹出 %dx%d @ (%d,%d)\n",
                mr.right - mr.left, mr.bottom - mr.top, mr.left, mr.top);
    } else {
        wprintf(L"（没有检测到菜单窗口，可能类名不同）\n");
    }

    // 岛台是否还在
    HWND still = findIsland();
    wprintf(L"岛台可见: %d\n", still && ::IsWindowVisible(still));
    return 0;
}