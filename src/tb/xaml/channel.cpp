// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/tb/xaml/channel.cpp — 主程序与 explorer 内 DLL 的控制通道
//
// ═══════════════════════════════════════════════════════════════════════
// 【为什么不用 COM 跨进程】
//
// 最初用自定义 COM 接口（IGlassAppearanceService）让主程序远程控制 DLL。
// 进程内一切正常，但**跨进程调用失败**：
//
//     QI IGlassAppearanceService -> 0x80004002 (E_NOINTERFACE)
//
// 原因是自定义 COM 接口跨进程需要**代理/桩（proxy/stub）**做封送。
// 系统只对 IDispatch、IUnknown 和标准接口内置封送支持。
// 要让自定义接口可用，必须：
//   · 写 .idl
//   · 用 midl.exe 生成 _i.c / _p.c
//   · 注册 PS CLSID 到注册表
//   · 链接 rpcrt4.lib
// 对于"下达三条指令"这个需求，这个代价完全不值。
//
// 【本方案】
//
// 共享内存 + 命名事件，完全绕开 COM：
//
//     主程序                           explorer 内的 DLL
//       │                                    │
//       ├─ 写共享内存的 request 字段 ─────────┤
//       ├─ SetEvent(请求事件) ───────────────┤
//       │                              GetMessage 循环里轮询
//       │                                     │
//       │                              读请求、执行、写 response
//       │◄──────── SetEvent(应答事件) ────────┤
//       ├─ 读 response ───────────────────────┤
//
// 优点：
//   · 不依赖任何封送机制，不受接口/ABI 变化影响
//   · 两边代码都只有几十行
//   · 天然支持"主程序未运行时 DLL 也能工作"
//
// 命名对象用固定前缀 + 进程无关的名字，双方都能打开。
// ═══════════════════════════════════════════════════════════════════════
#include "hr/tb/xaml/channel.h"

#include <atomic>
#include <mutex>

// ⚠ 不要 include "hr/shared/common.h" 也不要调 logf。
//
// 这个文件同时被编译进 GlassTAP.dll（功能层）。
// 功能层不该依赖主程序的日志模块 —— 那会拖进一堆无关依赖，
// 而且主程序的日志路径逻辑在 explorer 环境里不一定可用。
//
// 需要记录时用 taplog 那套（DLL 自己的日志）。
#define CHANNEL_LOG(...)  ((void)0)

namespace hr {
namespace tb {
namespace xaml {

namespace {

// 共享内存与事件的名字。主程序与 DLL 必须完全一致。
const wchar_t* kShmName = L"Local\\GlassTAP_Channel_v1";
const wchar_t* kReqEventName = L"Local\\GlassTAP_Request_v1";
const wchar_t* kAckEventName = L"Local\\GlassTAP_Ack_v1";

HANDLE g_shm = nullptr;
ChannelShared* g_view = nullptr;
HANDLE g_reqEvent = nullptr;
HANDLE g_ackEvent = nullptr;
std::mutex g_mu;

// 命令序号，用来匹配请求与应答
std::atomic<unsigned> g_nextSeq{1};

}  // namespace

// ── 初始化 ───────────────────────────────────────────────────────────

bool channelInit() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_view) return true;

    // 共享内存：CreateFileMapping 谁先调用谁创建，另一方 OpenFileMapping
    g_shm = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                 PAGE_READWRITE, 0, sizeof(ChannelShared), kShmName);
    if (!g_shm) {
        CHANNEL_LOG("channel: 创建共享内存失败 err=%lu", ::GetLastError());
        return false;
    }

    // ⚠⚠ GetLastError 必须在 CreateFileMapping **紧接着**就读。
    //
    //   踩过的坑（代码审计发现）：原来是在 MapViewOfFile 之后才查，
    //   而 MapViewOfFile 成功时会**清零** GetLastError ——
    //   于是"已存在"这个判断几乎永远为假，
    //   我们就会把对端已经写好的共享状态 ZeroMemory 掉。
    //   表现是主程序与 DLL 之间的指令随机超时、状态错乱。
    //
    //   正确做法：CreateFileMapping 之后立刻取，存起来后面用。
    const DWORD createErr = ::GetLastError();
    const bool alreadyExisted = (createErr == ERROR_ALREADY_EXISTS);

    g_view = static_cast<ChannelShared*>(
        ::MapViewOfFile(g_shm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ChannelShared)));
    if (!g_view) {
        CHANNEL_LOG("channel: 映射共享内存失败 err=%lu", ::GetLastError());
        ::CloseHandle(g_shm);
        g_shm = nullptr;
        return false;
    }

    // 只有**第一次创建**时才清零，避免把对方写入的数据擦掉
    if (!alreadyExisted) {
        ::ZeroMemory(g_view, sizeof(ChannelShared));
        g_view->magic = ChannelShared::kMagic;
    }

    g_reqEvent = ::CreateEventW(nullptr, FALSE, FALSE, kReqEventName);   // 自动重置
    g_ackEvent = ::CreateEventW(nullptr, FALSE, FALSE, kAckEventName);

    if (!g_reqEvent || !g_ackEvent) {
        CHANNEL_LOG("channel: 创建事件失败 err=%lu", ::GetLastError());
        channelShutdown();
        return false;
    }

    CHANNEL_LOG("channel: 通道就绪 shm=%p", (void*)g_view);
    return true;
}

void channelShutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_view) { ::UnmapViewOfFile(g_view); g_view = nullptr; }
    if (g_shm) { ::CloseHandle(g_shm); g_shm = nullptr; }
    if (g_reqEvent) { ::CloseHandle(g_reqEvent); g_reqEvent = nullptr; }
    if (g_ackEvent) { ::CloseHandle(g_ackEvent); g_ackEvent = nullptr; }
}

// ── 主程序侧：发命令 ─────────────────────────────────────────────────

ChannelResult channelSend(ChannelCmd cmd, unsigned arg, unsigned timeoutMs) {
    ChannelResult r{ false, 0 };

    if (!channelInit()) return r;
    if (!g_view || g_view->magic != ChannelShared::kMagic) {
        CHANNEL_LOG("channel: 共享内存未就绪");
        return r;
    }

    const unsigned seq = g_nextSeq.fetch_add(1);

    // 写请求（先写参数，最后写序号的"有效性"标志）
    g_view->cmd = (unsigned)cmd;
    g_view->arg = arg;
    g_view->result = 0xFFFFFFFF;      // 哨兵：表示"尚未处理"
    g_view->seq = seq;
    ::InterlockedExchange(&g_view->ready, 1);

    // 通知 DLL
    ::SetEvent(g_reqEvent);

    // 等应答。DLL 侧在消息循环里轮询（默认 120ms 一次）。
    //
    // 超时值由调用方给：
    //   · 已经确认注入成功 —— 对面一定有人听，短超时即可
    //   · 只是"怀疑"有残留 DLL 在听（上一个实例被强杀）——
    //     不确定对面是否存在，需要给够时间
    DWORD wr = ::WaitForSingleObject(g_ackEvent, timeoutMs ? timeoutMs : 5000);
    if (wr != WAIT_OBJECT_0) {
        CHANNEL_LOG("channel: 命令 %u 等待应答超时", (unsigned)cmd);
        ::InterlockedExchange(&g_view->ready, 0);
        return r;
    }

    // 校验序号，避免读到过期应答
    if (g_view->seq != seq) {
        CHANNEL_LOG("channel: 应答序号不匹配（期望 %u 实得 %u）", seq, g_view->seq);
        return r;
    }

    r.ok = true;
    r.hr = g_view->result;
    CHANNEL_LOG("channel: 命令 %u 完成 hr=0x%08X", (unsigned)cmd, r.hr);
    return r;
}

// ── DLL 侧：轮询并处理命令 ───────────────────────────────────────────

bool channelPoll(ChannelCmd* outCmd, unsigned* outArg) {
    if (!channelInit()) return false;
    if (!g_view || g_view->magic != ChannelShared::kMagic) return false;

    // ready 为 0 表示没有待处理命令
    if (::InterlockedCompareExchange(&g_view->ready, 0, 1) != 1) return false;

    if (outCmd) *outCmd = (ChannelCmd)g_view->cmd;
    if (outArg) *outArg = g_view->arg;
    return true;
}

void channelReply(HRESULT hr) {
    if (!g_view) return;
    g_view->result = (unsigned)hr;
    ::SetEvent(g_ackEvent);
}

bool channelReady() {
    return g_view != nullptr && g_view->magic == ChannelShared::kMagic;
}

// 供调用方直接读写共享内存里那些
// HRESULT 放不下的字段（比如矩形坐标）。
ChannelShared* channelShared() {
    return g_view;   // 未初始化时就是 nullptr
}

}  // namespace xaml
}  // namespace tb
}  // namespace hr
