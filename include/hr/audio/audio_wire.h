// hr/audio/audio_wire.h — 音频条的接线与设置键
#pragma once

#include <string>

namespace hr {

// 把音频条的回调接到模块上（main.cpp 初始化时调一次）。
void audioWire();

// 音频条相关的设置键（audio_bar / audio_bar_w / audio_bar_h / audio_bar_dir）。
bool audio_applySetting(const std::string& key, bool& handled);

}  // namespace hr
