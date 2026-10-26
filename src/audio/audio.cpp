// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/audio.cpp — 音频波动条（逐行对照 ui/audio_bar.py + core/audio_capture.py）
#include "hr/audio/audio.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "hr/shared/common.h"
#include "hr/glass/glass.h"

// MinGW 的 endpointvolume.h 只 forward-declare 了 IAudioMeterInformation，
// 没有给完整定义，这里按官方接口补齐（GetPeakValue 在 vtable 第一位）。
//
// ⚠ 只有 MinGW 需要这个补丁。MSVC 的 Windows SDK 已经在
//   endpointvolume.h 里完整定义了同名接口，再补一次会报
//   C2011（struct 类型重定义）。所以用编译器宏隔开。
#ifndef _MSC_VER
MIDL_INTERFACE("C02216F6-8C67-4B5B-9D00-D008E73E0064")
IAudioMeterInformation : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* pnChannelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 u32ChannelCount,
                                                            float* afPeakValues) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* pdwHardwareSupportMask) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IAudioMeterInformation, 0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7,
                0x3E, 0x00, 0x64)
#endif
#endif  // !_MSC_VER

namespace hr {
namespace {

const int kBarCount = 36;
const wchar_t* kClass = L"HotrainyAudioBar";

bool g_classReady = false;
HINSTANCE g_hinst = nullptr;
HWND g_hwnd = nullptr;
Surface g_frame;

std::atomic<bool> g_captureRunning{false};
std::thread g_captureThread;

std::mutex g_lvlMtx;
float g_levels[kBarCount] = {};
float g_rms = 0.0f;

// 峰值保持（对照 AudioBar._peak_hold / _peak_timers）
float g_peak[kBarCount] = {};
int g_peakTimer[kBarCount] = {};
int g_silentFrames = 0;

int g_w = 0, g_h = 0;

// ── 采集线程（对照 core/audio_capture.py 的 _meter_loop） ──

void captureLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::mt19937 rng((unsigned)(GetTickCount64() ^ (uintptr_t)&rng));
    std::uniform_real_distribution<float> jitter(-0.35f, 0.35f);

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioMeterInformation* meter = nullptr;
    IAudioEndpointVolume* vol = nullptr;

    do {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), (void**)&en);
        if (FAILED(hr) || !en) break;
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr) || !dev) break;
        dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter);
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol);
    } while (false);

    logf("audio: 采集 %s", meter ? "OK" : "失败（无音频设备）");

    // 每 30 秒重新绑定默认设备（插拔耳机/切设备）
    auto rebindAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (g_captureRunning.load()) {
        float peak = 0.0f;
        if (meter) {
            float p = 0.0f;
            if (SUCCEEDED(meter->GetPeakValue(&p))) peak = p;
        }
        if (vol) {
            float m = 1.0f;
            if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&m))) peak *= m;
        }

        // 生成参差不齐的频段模拟（对照 _meter_loop 的 base + jitter）
        float levels[kBarCount];
        for (int i = 0; i < kBarCount; ++i) {
            float base;
            if (i < kBarCount * 0.3f) {
                base = 1.0f;
            } else if (i < kBarCount * 0.65f) {
                base = 1.0f - ((i - kBarCount * 0.3f) / (float)kBarCount) * 0.6f;
            } else {
                base = 0.4f - ((i - kBarCount * 0.65f) / (float)kBarCount) * 0.35f;
            }
            float s = std::max(0.0f, base + jitter(rng));
            levels[i] = std::min(std::max(peak * s, 0.0f), 1.0f);
        }

        {
            std::lock_guard<std::mutex> lk(g_lvlMtx);
            for (int i = 0; i < kBarCount; ++i) g_levels[i] = levels[i];
            g_rms = peak;
        }

        // 30 秒重绑
        if (std::chrono::steady_clock::now() >= rebindAt) {
            rebindAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            if (en && dev) {
                IMMDevice* dev2 = nullptr;
                if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev2)) && dev2) {
                    IAudioMeterInformation* m2 = nullptr;
                    if (SUCCEEDED(dev2->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL,
                                                 nullptr, (void**)&m2)) &&
                        m2) {
                        if (meter) meter->Release();
                        meter = m2;
                    }
                    IAudioEndpointVolume* v2 = nullptr;
                    if (SUCCEEDED(dev2->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                                 nullptr, (void**)&v2)) &&
                        v2) {
                        if (vol) vol->Release();
                        vol = v2;
                    }
                    dev->Release();
                    dev = dev2;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    if (vol) vol->Release();
    if (meter) meter->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    CoUninitialize();
}

// ── 绘制（对照 AudioBar._draw_frame） ──

    // 配置传进来的尺寸/方向（0 = 用原来的屏幕比例）
    static int g_cfgW = 0;
    static int g_cfgH = 0;
    static int g_posX = -1;
    static int g_posY = -1;
    static void (*g_onMoved)(int, int) = nullptr;

void drawFrame() {
    if (!g_hwnd || g_w <= 0 || g_h <= 0) return;
    if (g_frame.width() != g_w || g_frame.height() != g_h) g_frame.create(g_w, g_h);
    g_frame.clear();   // 全透明底

    float levels[kBarCount];
    float rms;
    {
        std::lock_guard<std::mutex> lk(g_lvlMtx);
        for (int i = 0; i < kBarCount; ++i) levels[i] = g_levels[i];
        rms = g_rms;
    }

    if (rms < 0.02f) {
        g_silentFrames++;
    } else {
        g_silentFrames = 0;
    }
    const bool silent = g_silentFrames > 10;

    // ★ 横向声波丝带（照用户给的视频效果重写，旧版竖条已删除）：
    //   · 以水平中线**上下对称**
    //   · 由很多根细的半透明丝线叠出来（重叠处自然显亮，像流动的发丝）
    //   · 响的地方鼓、静的地方收成一根细线；两端渐尖
    const double cy = g_h * 0.5;
    const int margin = 6;
    const double usable = (double)std::max(1, g_w - margin * 2);
    const double maxAmp = std::max(2.0, g_h * 0.5 - 3.0);   // 最大半振幅
    const double t = (double)::GetTickCount64() / 1000.0;   // 时间相位 -> 游动

    // 36 段电平 -> 逐像素包络（线性插值 + 两端渐尖）
    auto envAt = [&](double u) {
        const double f = u * (kBarCount - 1);
        const int i0 = (int)f;
        const int i1 = std::min(i0 + 1, kBarCount - 1);
        const double fr = f - i0;
        const double v = levels[i0] * (1.0 - fr) + levels[i1] * fr;
        const double taper = std::sin(3.14159265358979 * u);
        return v * taper * taper;      // 两端收成细线
    };

    if (!silent) {
        const int kStrands = 14;
        for (int s = 0; s < kStrands; ++s) {
            const double k = (s + 1) / (double)kStrands;      // 由内到外
            const double ph = t * (0.6 + 0.35 * k) + s * 1.7; // 每根自己的相位
            const uint32_t alpha = (uint32_t)(34 + 62 * k);
            const uint32_t col = (alpha << 24) | 0x6E8CFFu;   // 蓝紫
            int px = 0, pyT = 0, pyB = 0;
            for (int x = 0; x <= (int)usable; ++x) {
                const double u = x / usable;
                const double a = maxAmp * envAt(u);
                // 丝线自身的起伏：两根正弦叠加，形成"发丝"的漂动
                const double wob = std::sin(u * 9.0 + ph) * 0.26 +
                                   std::sin(u * 23.0 + ph * 1.7) * 0.12;
                double off = a * (k + wob * (1.0 - k) * 0.9);
                if (off < 0.0) off = 0.0;
                const int X = margin + x;
                const int YT = (int)std::lround(cy - off);
                const int YB = (int)std::lround(cy + off);
                if (x > 0) {
                    g_frame.drawLine(px, pyT, X, YT, col, 1.0f);
                    g_frame.drawLine(px, pyB, X, YB, col, 1.0f);
                }
                px = X; pyT = YT; pyB = YB;
            }
        }
    }

    // 中轴：稍亮的一条细线（静音时它就是那根"细线"）
    g_frame.drawLine(margin, (int)cy, margin + (int)usable, (int)cy,
                     silent ? 0x2E6E8CFFu : 0x506E8CFFu, 1.0f);

    HDC screen = GetDC(nullptr);
    if (!screen) return;
    HBITMAP bmp = g_frame.createPremultipliedBitmap(screen);
    if (bmp) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        // ★ pptDst 是**屏幕坐标**，而且 ULW 会按它移动窗口 ——
        //   原来写死 (0,0)，于是每画一帧就把音频条拽到屏幕左上角
        //   （日志说 @y=776，实际窗口在 (0,0)，用户就"看不见"它）。
        //   用窗口当前真实位置：既不会再被拽走，以后自由拖动也不会被拉回。
        RECT wrc{};
        ::GetWindowRect(g_hwnd, &wrc);
        POINT dst = {wrc.left, wrc.top};
        SIZE sz = {g_w, g_h};
        POINT src = {0, 0};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK audioProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTCAPTION;   // drag the bar to move it
        case WM_EXITSIZEMOVE: {
            RECT r{}; ::GetWindowRect(g_hwnd, &r);
            g_posX = r.left; g_posY = r.top;
            if (g_onMoved) g_onMoved(r.left, r.top);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wp == 1) drawFrame();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool audioBarRunning() { return g_hwnd != nullptr && IsWindowVisible(g_hwnd); }

void initAudioBar(HINSTANCE hInst) {
    if (g_classReady) return;
    g_hinst = hInst;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = audioProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_classReady = true;
}


// 运行中原地应用新几何：只改窗口尺寸 + 画布尺寸，**不销毁重建**
// （销毁重建会让音频条闪一下消失，用户看到的就是"开了却没出现"）。
static void applyAudioGeometryNow() {
    if (!g_hwnd) return;
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    int barH = (g_cfgH > 0) ? g_cfgH : (int)(sh * 2 / 3);
    if (barH > sh) barH = sh;                 // 夹到屏幕内，否则底部画到屏幕外
    int barW = (g_cfgW > 0) ? g_cfgW : std::max((int)(sw * 3 / 52), 32);
    if (barW > sw) barW = sw;
    int barY = (g_posY >= 0) ? g_posY : (sh - barH) / 2;   // remembered position
    if (barY < 0) barY = 0;
    if (barY + barH > sh) barY = std::max(0, sh - barH);   // 整窗留在屏幕上
    g_w = barW;
    g_h = barH;
    ::SetWindowPos(g_hwnd, nullptr, 0, barY, barW, barH,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(g_hwnd, nullptr, FALSE);
    logf("audio: 原地应用几何 %dx%d @y=%d", barW, barH, barY);
}

void setAudioBarMovedHook(void (*cb)(int x, int y)) { g_onMoved = cb; }
void setAudioBarPos(int x, int y) { g_posX = x; g_posY = y; }

void setAudioBarGeometry(int w, int h) {
    const bool changed = (w != g_cfgW || h != g_cfgH);
    g_cfgW = w;
    g_cfgH = h;
    logf("audio: 几何设置 %dx%d", w, h);
    if (changed && g_hwnd) applyAudioGeometryNow();
}

void startAudioBar() {
    if (g_hwnd) {
        // ⚠ 不能只 ShowWindow：关掉时只是 SW_HIDE，
        //   分层窗口（UpdateLayeredWindow）的表层内容不会自己回来 ——
        //   表现就是“关一次再开就看不见了”。
        //   销毁重建，等同第一次打开那条完整路径。
        destroyAudioBar();
    }
    if (!g_classReady) return;

    int sh = GetSystemMetrics(SM_CYSCREEN);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    // 尺寸：优先用配置传进来的（像素），没传就用原来的屏幕比例
    int barH = (g_cfgH > 0) ? g_cfgH : (int)(sh * 2 / 3);
    if (barH > sh) barH = sh;                 // 夹到屏幕内，否则底部画到屏幕外
    int barW = (g_cfgW > 0) ? g_cfgW : std::max((int)(sw * 3 / 52), 32);
    if (barW > sw) barW = sw;
    // 位置：按用户选的 A——改尺寸/切方向时**位置不动**（左边贴边、纵向居中）
    int barY = (g_posY >= 0) ? g_posY : (sh - barH) / 2;   // remembered position
    if (barY < 0) barY = 0;
    if (barY + barH > sh) barY = std::max(0, sh - barH);   // 整窗留在屏幕上
    g_w = barW;
    g_h = barH;

    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             kClass, L"", WS_POPUP, 0, barY, barW, barH, nullptr, nullptr,
                             g_hinst, nullptr);
    if (!g_hwnd) {
        logf("audio: 创建窗口失败 %lu", (unsigned long)GetLastError());
        return;
    }

    for (int i = 0; i < kBarCount; ++i) {
        g_levels[i] = 0.0f;
        g_peak[i] = 0.0f;
        g_peakTimer[i] = 0;
    }
    g_silentFrames = 0;

    // 采集线程
    g_captureRunning = true;
    g_captureThread = std::thread(captureLoop);

    SetTimer(g_hwnd, 1, 30, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    logf("audio: 波动条开 %dx%d @(0,%d)", barW, barH, barY);
}

void stopAudioBar() {
    if (g_captureRunning.exchange(false) && g_captureThread.joinable()) {
        g_captureThread.join();
    }
    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_HIDE);
    }
    logf("audio: 波动条关");
}

void destroyAudioBar() {
    stopAudioBar();
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
}

}  // namespace hr
