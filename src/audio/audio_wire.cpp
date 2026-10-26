// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/audio/audio_wire.cpp — 音频条的接线 + 它那几个设置键。
#include "hr/audio/audio_wire.h"

#include <string>

#include "hr/app/app_context.h"
#include "hr/audio/audio.h"
#include "hr/shared/config.h"

namespace hr {

void audioWire() {
    // 拖动音频条 -> 记位置并落盘
    setAudioBarMovedHook([](int x, int y) {
        cfg().audioBarX = x;
        cfg().audioBarY = y;
        saveConfig(cfg());
    });
}

// 音频条相关的设置键：开/关、横向/纵向长度、方向。
bool audio_applySetting(const std::string& key, bool& handled) {
    handled = true;
    if (key == "audio_bar") {
        if (cfg().audioBar) {
            startAudioBar();
        } else {
            stopAudioBar();
        }
    } else if (key == "audio_bar_w" || key == "audio_bar_h") {
        // ★ 音频条尺寸/方向：只把新几何交给它原地改，绝不 stop/start
        setAudioBarGeometry(cfg().audioBarW, cfg().audioBarH);
        setAudioBarPos(cfg().audioBarX, cfg().audioBarY);
    } else {
        handled = false;
    }
    return true;
}

}  // namespace hr
