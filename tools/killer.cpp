// tools/killer.cpp — 给指定 PID 的 hotrainy 实例投 WM_CLOSE（优雅退出）
// 必须按 PID 精确匹配：本机可能同时跑着另一个 hotrainy 实例，
// 只按窗口标题找会误关别人的进程。
// 用法: killer.exe <pid>   按 PID 精确关
//       killer.exe 0       关掉找到的第一个实例（调试用；确认本机只有一个时才行）
#include <windows.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: killer.exe <pid|0>\n");
        return 1;
    }
    DWORD target = (DWORD)atoi(argv[1]);

    // 消息窗口挂在 HWND_MESSAGE 下，EnumWindows 看不到，只能这么找
    HWND h = nullptr;
    while ((h = ::FindWindowExW(HWND_MESSAGE, h, L"HotrainyMsg", nullptr)) != nullptr) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(h, &pid);
        if (target == 0 || pid == target) {
            ::PostMessageW(h, WM_CLOSE, 0, 0);
            printf("WM_CLOSE -> pid %lu (hwnd %p)\n", (unsigned long)pid, (void*)h);
            return 0;
        }
    }
    printf("pid %lu not found\n", (unsigned long)target);
    return 2;
}
