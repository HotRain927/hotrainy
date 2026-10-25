// tools/inject_probe.cpp — 验证能否往 explorer 的 XAML 树里插入元素
//
// 背景：岛台要"长在任务栏上"，就得往任务栏的 XAML 树里 AddChild。
// 这件事没人试过，先做最小验证：
//
//   1. CreateInstance 造一个 Rectangle
//   2. 设它的尺寸和颜色
//   3. AddChild 挂到 TaskbarFrame 下
//   4. 截图看有没有多出一块颜色
//
// 命令行：
//   inject_probe.exe                   —— 插入测试矩形
//   inject_probe.exe remove            —— 移除并还原
//
// 这个工具通过共享内存通道跟 DLL 说话，实际动作在 DLL 侧做
// （改 XAML 必须在 XAML 线程上）。
#include "hr/tb/xaml/channel.h"

#include <windows.h>

#include <cstdio>

using namespace hr::tb::xaml;

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const bool remove = (argc > 1 && wcscmp(argv[1], L"remove") == 0);

    if (!channelInit() || !channelReady()) {
        wprintf(L"通道未就绪 —— 先启动 hotrainy\n");
        return 1;
    }

    wprintf(L"===== inject_probe =====\n\n");

    // 先确认对面有 DLL 在听
    ChannelResult probe = channelSend(ChannelCmd::QueryState, 0, 1500);
    if (!probe.ok) {
        wprintf(L"explorer 里没有 DLL 在监听（确认 hotrainy 已启动并注入）\n");
        return 1;
    }
    wprintf(L"对面已就绪，识别到 %d 个任务栏\n\n", (int)probe.hr);

    // 插一个测试矩形
    wprintf(L">>> %s测试矩形\n", remove ? L"移除" : L"插入");
    ChannelResult r = channelSend(ChannelCmd::ProbeInjectElement,
                                  remove ? 0u : 1u, 8000);
    wprintf(L"    ok=%d hr=0x%08X\n", (int)r.ok, (unsigned)r.hr);

    if (!r.ok) {
        wprintf(L"\n指令没有回应。\n");
        return 1;
    }

    if (SUCCEEDED(r.hr)) {
        wprintf(L"\n✅ 操作成功\n");
    } else {
        wprintf(L"\n❌ 失败 0x%08X\n", (unsigned)r.hr);
        wprintf(L"   0x80004005 = E_FAIL（XAML 拒绝了这次操作）\n");
        wprintf(L"   0x80070057 = E_INVALIDARG（参数或属性名不对）\n");
        wprintf(L"   0x80004002 = E_NOINTERFACE（接口不支持这个操作）\n");
    }

    channelShutdown();
    return 0;
}