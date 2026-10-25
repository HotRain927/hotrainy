// tools/tb_state.cpp — 打印任务栏窗口当前的透明状态（诊断用）
// 看 WS_EX_LAYERED 是否还挂着、alpha 是多少，用来判断"退出后有没有恢复"。
#include <windows.h>

#include <cstdio>

int main() {
    HWND tb = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tb) {
        printf("no Shell_TrayWnd\n");
        return 1;
    }
    LONG_PTR ex = ::GetWindowLongPtrW(tb, GWL_EXSTYLE);
    bool layered = (ex & WS_EX_LAYERED) != 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    COLORREF key = 0;
    if (layered) {
        if (!::GetLayeredWindowAttributes(tb, &key, &alpha, &flags)) {
            printf("GetLayeredWindowAttributes failed %lu\n", (unsigned long)::GetLastError());
            alpha = 0;
        }
    }
    printf("hwnd=%p exstyle=0x%llX layered=%d alpha=%d flags=0x%lX\n", (void*)tb,
           (unsigned long long)ex, (int)layered, (int)alpha, (unsigned long)flags);
    return 0;
}
