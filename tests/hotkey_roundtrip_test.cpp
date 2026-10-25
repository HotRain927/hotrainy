// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// Hotkey round-trip self test.
//
// The settings recorder writes a hotkey with formatHotkey(mods, vk); the app
// reads it back with parseHotkey() when registering. If those two disagree,
// parsing fails and the settings UI shows "already in use" (the reported bug).
// This walks every virtual key 1..0xFF and prints any that do not round-trip.
#include <string>
#include <windows.h>

#include "hr/shared/hotkey.h"

#include <cstdio>

int main() {
    int good = 0, bad = 0;
    for (UINT vk = 1; vk <= 0xFF; ++vk) {
        const std::string spec = hr::formatHotkey(MOD_CONTROL | MOD_ALT, vk);
        UINT mods = 0, back = 0;
        const bool ok = hr::parseHotkey(spec, mods, back);
        if (!ok || back != vk || mods != (MOD_CONTROL | MOD_ALT)) {
            if (bad < 25) {
                std::printf("FAIL vk=0x%02X spec=\"%s\" parsed=%d back=0x%02X mods=0x%X\n",
                            (unsigned)vk, spec.c_str(), (int)ok, (unsigned)back, (unsigned)mods);
            }
            ++bad;
        } else {
            ++good;
        }
    }
    std::printf("round-trip: good=%d bad=%d\n", good, bad);
    return bad ? 1 : 0;
}