// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/audio.h — 音频波动条（对照 ui/audio_bar.py + core/audio_capture.py）
//
// 屏幕最左侧纵向频谱条：高度 2/3 屏、纵向居中、宽度 = 屏宽*3/52（最小 32）。
// 通过 IAudioMeterInformation::GetPeakValue 读系统输出峰值，乘以主音量系数，
// 36 根柱子带峰值保持，静音时静止显示深灰短柱。
#pragma once

#include <windows.h>
#include <string>

namespace hr {

// 音频条尺寸/方向（由主程序从配置传入；像素单位）
// 自由拖动：松手回调（主程序用它把位置写进配置）
void setAudioBarMovedHook(void (*cb)(int x, int y));
// 启动时按记忆位置摆放（-1 = 用默认位置）
void setAudioBarPos(int x, int y);
void setAudioBarGeometry(int w, int h, const std::string& dir);

void initAudioBar(HINSTANCE hInst);
void startAudioBar();
void stopAudioBar();
void destroyAudioBar();
bool audioBarRunning();

}  // namespace hr
