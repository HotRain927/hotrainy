// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// tools/tb_lifecycle.cpp — 任务栏透明生命周期自测
//
// 覆盖 hotrainy 集成的透明功能的所有关键路径，每条都有明确断言：
//   1. 开启 → 系统主题切亮色 + 快照记录原值
//   2. 幂等 → 重复开启不覆盖原值
//   3. 崩溃残留 → 重启后仍能读到正确原值（不被当前值污染）
//   4. 关闭 → 系统主题还原 + 快照清理
//   5. 残留清理 → 配置为关但盘上有快照时，启动对账要还原
//
// 不依赖 GUI，直接调 hr::tb 接口。退出码 0 = 全部通过。
#include "hr/common.h"
#include "hr/tb/theme.h"

#include <cstdio>
#include <string>

using namespace hr;

static int g_fail = 0;
static int g_pass = 0;

static void check(const char* name, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
        printf("  [PASS] %s%s%s\n", name, detail.empty() ? "" : "  ", detail.c_str());
    } else {
        ++g_fail;
        printf("  [FAIL] %s%s%s\n", name, detail.empty() ? "" : "  ", detail.c_str());
    }
}

static DWORD curTheme() {
    tb::SystemTheme t = tb::readSystemTheme();
    return t.valid ? t.systemUsesLightTheme : 999;
}

static void setTheme(DWORD v) {
    tb::setSystemLightTheme(v != 0);
    ::Sleep(600);
}

int main() {
    logInit();
    printf("=== taskbar transparency lifecycle test ===\n\n");

    // 起点：深色，清掉快照
    tb::clearThemeSnapshot();
    setTheme(0);
    printf("start: theme=%lu\n\n", (unsigned long)curTheme());

    // ── 1. 开启 ──
    printf("[1] enable\n");
    bool on = tb::enableTransparency();
    ::Sleep(500);
    check("enableTransparency returns true", on);
    check("system theme switched to light", curTheme() == 1,
          "theme=" + std::to_string(curTheme()));
    tb::SystemTheme snap;
    bool has = tb::loadThemeSnapshot(&snap);
    check("snapshot written", has);
    check("snapshot holds original dark", has && snap.systemUsesLightTheme == 0,
          has ? ("saved=" + std::to_string(snap.systemUsesLightTheme)) : "");

    // ── 2. 幂等：重复开启不改原值 ──
    printf("\n[2] idempotent enable\n");
    tb::enableTransparency();
    ::Sleep(400);
    has = tb::loadThemeSnapshot(&snap);
    check("original value preserved on re-enable",
          has && snap.systemUsesLightTheme == 0,
          has ? ("saved=" + std::to_string(snap.systemUsesLightTheme)) : "");

    // ── 3. 模拟崩溃残留：进程重启后，主题停在亮色而快照是深色 ──
    printf("\n[3] crash residue (theme=light, snapshot=dark)\n");
    // 不删快照，直接重新"对账" —— 这是重启后的第一条路径
    tb::reconcileOnStartup(true);   // wantTransparent=true → 保持
    ::Sleep(400);
    check("theme stays light when wanted", curTheme() == 1);

    // ── 4. 关闭：还原 ──
    printf("\n[4] disable\n");
    bool off = tb::disableTransparency();
    ::Sleep(500);
    check("disableTransparency returns true", off);
    check("theme restored to dark", curTheme() == 0,
          "theme=" + std::to_string(curTheme()));
    check("snapshot cleared", !tb::loadThemeSnapshot(&snap));

    // ── 5. 残留清理：配置为关，但盘上有快照且主题是亮色 ──
    printf("\n[5] startup reconcile (config=off, stale snapshot present)\n");
    setTheme(0);
    tb::saveThemeSnapshot(tb::SystemTheme{0, 0, true});   // 原值=深色
    setTheme(1);                                          // 系统被留在亮色
    tb::reconcileOnStartup(false);                        // 配置说不要透明
    ::Sleep(500);
    check("stale residue reverted to dark", curTheme() == 0,
          "theme=" + std::to_string(curTheme()));
    check("stale snapshot removed", !tb::loadThemeSnapshot(&snap));

    // ── 6. 状态文本 ──
    printf("\n[6] status text\n");
    std::wstring st = tb::statusText();
    check("statusText non-empty", !st.empty());

    printf("\n=== result: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}