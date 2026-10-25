// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/island/taskbar.h — 任务栏透明（只做这一件事）
//
// 打开 = 任务栏背景完全透明（透出壁纸，图标保持清晰）
// 关掉 = 还原成系统默认外观
//
// 实现原理见 src/tb/xaml/ 下的注释：
//   任务栏背景是 explorer 进程内 XAML 树里名为 "BackgroundFill" 的
//   Rectangle。把它整层隐藏，背景就透出壁纸，而图标在别的节点上，
//   完全不受影响。
//
// 这条路必须注入 explorer —— 从外部用 DWM / accent / blur / backdrop
// 全部无效（已穷举实测），因为那层暗底是 explorer 自己用 XAML 画的。
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace hr {

class TaskbarService {
public:
    TaskbarService();
    ~TaskbarService();

    TaskbarService(const TaskbarService&) = delete;
    TaskbarService& operator=(const TaskbarService&) = delete;

    void Start();
    // 停止时会把任务栏还原成默认外观，不留副作用
    void Stop();
    bool running() const;

    // 主开关。true = 任务栏背景透明，false = 还原默认。
    void SetTransparent(bool on);
    bool transparent() const;

    // 兼容旧接口：新机制下 alpha 不参与（透明是二值的），
    // 调用会被忽略并记日志。
    void SetAlpha(int alpha);
    int alpha() const;

    // 当前效果下的图标锐度相对基准的比值（>1 表示比默认更清晰）
    float iconSharpnessRatio() const;

    // 重新应用当前状态（例如 explorer 重启之后）
    void Refresh();

    // 把任务栏强制还原成默认外观。
    //
    // 用于"上次进程被强杀、任务栏还留着透明"的场景：
    // 强杀时 Stop() 没机会执行，改动会留在 explorer 内存里。
    // 这个动作会在启动时被调用一次，确保从干净状态开始。
    void ForceReset();

    // 托盘文字颜色策略：L"auto" / L"dark" / L"light"。
    //
    // 任务栏透明后背景变成壁纸，系统默认的白色文字在浅色壁纸上
    // 会看不清。这个设置决定怎么处理：
    //   auto  —— 按背景亮度自动选（默认）
    //   dark  —— 强制深色
    //   light —— 强制浅色
    void SetTextColor(const std::wstring& mode);
    std::wstring textColor() const;

    // 立刻按当前策略应用一次文字颜色。
    // 壁纸换了、或者用户刚切换策略时调用。
    bool ApplyTextColor();

    // 界面展示：一句话状态
    std::wstring StatusText() const;

    // 兼容性提示，无冲突时返回空
    std::vector<std::wstring> CompatNotes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 进程内唯一的服务
TaskbarService& taskbarService();

}  // namespace hr