// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// tools/island_btn_probe.cpp — 查岛台相关的位置 / 测点击
//
// 用法：
//   island_btn_probe.exe pos       查岛台按钮当前的屏幕位置
//   island_btn_probe.exe tap       取一次「按钮被点击」的标志
//   island_btn_probe.exe startpos  查开始按钮原来占的那个坑（面板/按钮该放哪）
#include "hr/tb/xaml/channel.h"

#include <windows.h>

#include <cstdio>

using namespace hr::tb::xaml;

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!channelInit() || !channelReady()) {
        wprintf(L"通道未就绪 —— 先启动 hotrainy\n");
        return 1;
    }

    const wchar_t* act = (argc > 1) ? argv[1] : L"pos";

    if (wcscmp(act, L"pos") == 0) {
        ChannelResult r = channelSend(ChannelCmd::QueryIslandButtonRect, 0, 3000);
        wprintf(L"ok=%d\n", (int)r.ok);
        if (r.ok) {
            const unsigned packed = (unsigned)r.hr;
            wprintf(L"打包值 0x%08X -> 宽=%u 高=%u\n",
                    packed, packed >> 16, packed & 0xFFFF);
        }
        wprintf(L"（坐标在 tap.log 里，搜 '岛台按钮屏幕位置'）\n");

    } else if (wcscmp(act, L"tap") == 0) {
        ChannelResult r = channelSend(ChannelCmd::ConsumeIslandTap, 0, 2000);
        wprintf(L"ok=%d  取到点击标志=%d\n", (int)r.ok, r.ok ? (int)r.hr : -1);

    } else if (wcscmp(act, L"startpos") == 0) {
        // 这是方案 B 的关键：把开始按钮原来占的位置读出来，
        // 岛台按钮就摆在那儿 —— 位置是从开始按钮实测的，
        // 所以视觉上和图标组天然对齐。
        ChannelResult r = channelSend(ChannelCmd::QueryStartSlot, 0, 3000);
        wprintf(L"ok=%d  有效=%d\n", (int)r.ok, r.ok ? (int)r.hr : -1);
        wprintf(L"（坐标在 tap.log 里，搜 '开始按钮坑位'）\n");

    } else {
        wprintf(L"用法: island_btn_probe.exe <pos|tap|startpos>\n");
    }

    channelShutdown();
    return 0;
}