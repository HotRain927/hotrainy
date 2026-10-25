// tools/tb_test.cpp — 任务栏透明冒烟测试（控制台）
// 开 → 打印状态与实测色差 → 关 → 打印状态，最后恢复系统默认。
// 用法: tb_test.exe [每步停留毫秒，默认 1600]
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "hr/common.h"
#include "hr/taskbar.h"

using namespace hr;

namespace {

void show(const char* stage) {
    std::wstring st = taskbarService().StatusText();
    printf("[%-10s] %s\n", stage, toUtf8(st).c_str());
    fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    int hold = (argc > 1) ? atoi(argv[1]) : 1600;
    if (hold < 300) hold = 300;

    logInit();
    // 必须声明 DPI 感知：否则进程是 DPI-unaware，GetWindowRect 拿到的是被
    // 虚拟化过的坐标，抓屏会抓到任务栏以外的地方（实测 delta 恒为 0.00）。
    enablePerMonitorDpi();
    dpiScale();

    taskbarService().Start();

    taskbarService().SetTransparent(true);
    Sleep(hold);
    show("on");

    // 模拟"直接退出"：不显式关，直接 Stop()。Stop 必须自己把任务栏还原，
    // 否则用户退出程序后任务栏会一直是半透明的。
    taskbarService().Stop();
    Sleep(400);

    HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
    LONG_PTR ex = tb ? GetWindowLongPtrW(tb, GWL_EXSTYLE) : 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    if (tb && (ex & WS_EX_LAYERED)) GetLayeredWindowAttributes(tb, nullptr, &alpha, &flags);
    printf("[stop      ] layered=%d alpha=%d  -> %s\n", (ex & WS_EX_LAYERED) ? 1 : 0, (int)alpha,
           (ex & WS_EX_LAYERED) ? "没恢复！" : "已恢复系统默认");
    printf("done\n");
    return 0;
}
