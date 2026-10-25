// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/tb/xaml/channel.h — 主程序与 explorer 内 DLL 的控制通道
//
// 共享内存 + 命名事件，不使用 COM 封送。
// 详细说明见 channel.cpp 顶部。
#pragma once

#include <windows.h>

namespace hr {
namespace tb {
namespace xaml {

// 命令码
enum class ChannelCmd : unsigned {
    None = 0,
    ApplyTransparent = 1,   // arg = 0，把背景与边框整层隐藏
    RestoreDefault = 2,     // 把 Opacity 写回 1
    QueryState = 3,         // 返回已识别的任务栏数量
    SetTextDark = 4,        // 把托盘文字强制改成深色
    SetTextLight = 5,       // 把托盘文字强制改成浅色
    AutoTextColor = 6,      // 按背景亮度自动决定深浅
    SetIslandTakeover = 7,  // arg = 1 开启岛台接管（隐藏任务栏的开始与搜索）
    ProbeInjectElement = 8, // arg = 1 插入岛台按钮；0 移除
    ConsumeIslandTap = 9,   // 取走"岛台按钮被点击"的标志（取完清零）
    QueryIslandButtonRect = 10, // 查岛台按钮在屏幕上的位置
    QueryStartSlot = 11,    // 查开始按钮原来占的那个坑（屏幕坐标）
    ConsumeLayoutChanged = 12,
    RestoreTransparencyOnly = 13, // 只还原透明化（背景+边框），不碰岛台按钮 // 取走"任务栏布局变了"的标志（图标增减时置位）
};

// 共享内存布局。
//
// 双方按同一份结构读写，所以它必须保持简单、只用 POD 类型 ——
// 不能有指针、不能有 C++ 对象，否则跨进程无意义。
struct ChannelShared {
    static const unsigned kMagic = 0x47544150;   // 'GTAP'

    unsigned magic;      // 固定值，用来判断这块内存是否已被初始化
    volatile LONG ready; // 1 = 有一条待处理命令
    unsigned seq;        // 命令序号，用于匹配请求与应答
    unsigned cmd;        // ChannelCmd
    unsigned arg;        // 命令参数
    unsigned result;     // 应答：HRESULT

    // ── 坐标回传 ──
    //
    // HRESULT 放不下一个矩形，日志又不适合程序读。
    // 所以专门开四个字段：DLL 算完写这里，主程序直接读。
    // 用在 QueryStartSlot（开始按钮的坑位）这类查询上。
    int rectLeft;
    int rectTop;
    int rectRight;
    int rectBottom;
};

struct ChannelResult {
    bool ok;        // 通道层面是否成功（收到了应答）
    HRESULT hr;     // DLL 侧执行结果
};

// 建立通道（主程序与 DLL 都会调用，幂等）
bool channelInit();

// 关闭通道。主程序退出前调用。
void channelShutdown();

// 通道是否可用
bool channelReady();

// ── 主程序侧 ─────────────────────────────────────────────────────────

// 发一条命令并等待应答。超时返回 ok=false。
//
// timeoutMs 给 0 表示用默认值（5 秒）。
// 已经确认注入成功时可以给短一点（比如 2000），
// 因为对面一定有人监听；只是"怀疑"有残留 DLL 时反而要给够时间。
ChannelResult channelSend(ChannelCmd cmd, unsigned arg = 0, unsigned timeoutMs = 0);

// ── DLL 侧 ───────────────────────────────────────────────────────────

// 取出一条待处理命令。没有则返回 false。
//
// DLL 需要在 explorer 的消息循环里周期性调用它。
// 我们已经在 XAML 线程上建了一个消息窗口（visualtree.cpp），
// 在那里加一个定时器即可。
bool channelPoll(ChannelCmd* outCmd, unsigned* outArg);

// 回复命令执行结果
void channelReply(HRESULT hr);

// 取得共享内存指针（只读地址，不拿所有权）。
// 用来传 HRESULT 放不下的数据，比如一个矩形。
// 未初始化时返回 nullptr。
ChannelShared* channelShared();

}  // namespace xaml
}  // namespace tb
}  // namespace hr