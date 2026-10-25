// tools/island_drag.cpp — 拖动测试 + 位置/尺寸持久化验证
//
// 用法：
//   island_drag.exe info         打印岛台当前位置尺寸
//   island_drag.exe resize dx dy 拖右下角，尺寸增量为 dx,dy
//   island_drag.exe move   dx dy 拖标题区移动
//
// ⚠ 必须先声明 DPI 感知，否则坐标全是虚拟化的，拖动会落空。
#include <windows.h>

#include <cstdio>

static HWND findIsland() {
    return ::FindWindowW(L"HotrainyIsland", nullptr);
}

static void dump(const char* tag) {
    HWND h = findIsland();
    if (!h) { printf("  %s: 找不到岛台\n", tag); return; }
    RECT r{};
    ::GetWindowRect(h, &r);
    printf("  %s: %dx%d @ (%d,%d) 可见=%d\n",
           tag, r.right - r.left, r.bottom - r.top, r.left, r.top,
           (int)::IsWindowVisible(h));
}

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (argc < 2) {
        wprintf(L"用法: island_drag.exe <info|resize|move> [dx dy]\n");
        return 1;
    }

    const wchar_t* act = argv[1];
    int dx = (argc > 3) ? _wtoi(argv[2]) : 80;
    int dy = (argc > 4) ? _wtoi(argv[3]) : 60;
    if (argc > 3 && argc <= 4) { dx = _wtoi(argv[2]); dy = 60; }

    if (wcscmp(act, L"info") == 0) {
        dump("岛台");
        return 0;
    }

    HWND h = findIsland();
    if (!h || !::IsWindowVisible(h)) {
        wprintf(L"岛台没开\n");
        return 1;
    }

    RECT r{};
    ::GetWindowRect(h, &r);
    dump("拖动前");

    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (wcscmp(act, L"resize") == 0) {
        // ⚠ 抓"右边中部"而不是"右下角"。
        //
        //   分层窗口里圆角外的像素 alpha=0，那里**不接收鼠标** ——
        //   拖右下角会直接穿透，WM_NCHITTEST 根本不会被调用。
        //   右边中部是实心像素，能正常命中。
        x1 = r.right - 5;
        y1 = r.top + (r.bottom - r.top) / 2;
        x2 = x1 + dx;
        y2 = y1;
    } else if (wcscmp(act, L"move") == 0) {
        // 顶部窄带（搜索框上方）作为拖动柄，x 取中间偏左避开关闭按钮
        x1 = r.left + (r.right - r.left) / 2;
        y1 = r.top + 3;
        x2 = x1 + dx;
        y2 = y1 + dy;
    } else {
        wprintf(L"未知动作\n");
        return 1;
    }

    wprintf(L"从 (%d,%d) 拖到 (%d,%d)\n", x1, y1, x2, y2);

    ::SetCursorPos(x1, y1);
    ::Sleep(250);
    ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    ::Sleep(200);
    for (int i = 1; i <= 12; ++i) {
        ::SetCursorPos(x1 + (x2 - x1) * i / 12, y1 + (y2 - y1) * i / 12);
        ::Sleep(45);
    }
    ::Sleep(200);
    ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    ::Sleep(800);

    dump("拖动后");
    return 0;
}