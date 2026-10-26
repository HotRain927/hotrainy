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

        // ★ 频段模拟（第二版）：真 WASAPI 只给一个总峰值，原来的"斜坡 + jitter"
        //   让 36 段几乎一样高 —— 画出来是平滑月牙，没有波峰波谷（用户反馈）。
        //   改成**随时间演化的多正弦场**：三组不同频率/相位的行波叠加，峰谷
        //   明显且缓慢流动；两端略收，更像声波包络；整体乘真实总峰值，于是
        //   有声音才起伏、静音时收成一根细线。
        const double nowS = (double)::GetTickCount64() / 1000.0;
        const double kPi = 3.14159265358979;
        float levels[kBarCount];
        for (int i = 0; i < kBarCount; ++i) {
            const double u = (double)i / (double)(kBarCount - 1);
            // ★ 沿线起伏：四组行波叠加（频率依次升高）—— 沿 x 出 3~5 个明显
            //   的峰谷，而且整体缓慢流动。这里**不再**乘拱形，否则拱会盖掉起伏。
            // ★ 幅度更明显（用户要求）
            // ★ 频率提高（用户："让波动的频率更明显、更波动不齐"）
            double f = 0.46
                     + 0.34 * std::sin(u * 13.0 - nowS * 1.10)
                     + 0.26 * std::sin(u * 23.0 + nowS * 0.75 + 1.3)
                     + 0.16 * std::sin(u * 41.0 - nowS * 1.90 + 2.6)
                     + 0.10 * std::sin(u * 71.0 + nowS * 2.70 + 0.7);
            f = std::max(0.0, std::min(1.0, f));
            // ★ 中间更高、两端更低（圆顶更陡：两端 0.28、中间 1.0）
            const double dome = 0.28 + 0.72 * std::sin(kPi * u);
            // ★ 振幅调大（用户要求）：
            //   ① 响度补偿 sqrt(peak) —— 电平实测通常只有 0.3~0.6，开方后视觉
            //      上才满（视觉均衡器的常规做法，不是造假数据，只是映射曲线）；
            //   ② 基数与摆动都抬高：0.42+0.92f -> 0.55+1.05f。
            levels[i] = (float)std::min(1.0, std::sqrt((double)peak) * dome * (0.55 + 1.05 * f));
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
    static std::string g_tone = "black";     // 色调：black/white/blue/red/yellow（默认黑）

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
    // ★ 静音/音量为零时**不能整条消失**（用户反馈）：此时改用一条低而清晰的
    //   静止丝带（约窗口高的 3.5%，至少 5px），看得到但安静。
    const double maxAmp = silent ? std::max(2.5, g_h * 0.010)
                                 : std::max(2.0, g_h * 0.5 - 3.0);
    const double t = (double)::GetTickCount64() / 1000.0;   // 时间相位 -> 游动

    // 36 段电平 -> 逐像素包络。
    // ★ 用 Catmull-Rom 平滑插值（原来是线性，24px 一段的折角肉眼可见 ——
    //   用户反馈"不够连贯"），再叠两端渐尖。
    auto sample = [&](int i) {
        if (i < 0) i = 0;
        if (i >= kBarCount) i = kBarCount - 1;
        return (double)levels[i];
    };
    auto envAt = [&](double u) {
        const double f = u * (kBarCount - 1);
        const int i1 = (int)f;
        const double fr = f - i1;
        const double p0 = sample(i1 - 1), p1 = sample(i1);
        const double p2 = sample(i1 + 1), p3 = sample(i1 + 2);
        // Catmull-Rom
        const double v = 0.5 * ((2.0 * p1) +
                                (-p0 + p2) * fr +
                                (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * fr * fr +
                                (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * fr * fr * fr);
        // ★ 只把最两端收成细线：原来是 sin^2（等于又叠了一层拱，把沿线起伏
        //   一起压平了），改成 sqrt(sin) —— 中间绝大部分宽度不被压。
        const double taper = std::pow(std::max(0.0, std::sin(3.14159265358979 * u)), 0.85);   // 边缘更细
        // ★ 包络层再加一道**逐像素**细密波纹（用户："更加波动不齐"）：
        //   频段插值本身很顺，只有靠它才能出参差不齐的细节。乘性叠加，
        //   响的地方波纹大、静的地方自动收敛。
        const double ripple = 1.0 + 0.15 * std::sin(u * 86.0 + t * 1.7)
                                  + 0.08 * std::sin(u * 163.0 - t * 2.3 + 0.9);
        return std::max(0.0, v) * taper * ripple;
    };

    {
        // ★ 三层画法，从外到内：外光晕(4.5) -> 内光晕(2.2) -> 细芯(1.0)，
        //   alpha 都压低，叠起来是"发光"而不是"描边"（用户要更柔和）。
        //   每根丝线的色相在 青蓝 -> 蓝 -> 紫 之间取，外圈偏紫、内圈偏青，
        //   再沿 x 加一点色相漂移 —— 色调更丰富。
        // 静音 3 条平直细线；有声 11 根（用户指定），alpha 与颜色按同一条
        // 线性梯度从内到外均匀变淡。
        // 静音 3 条；有声 14 根（用户指定）
        const int kStrands = silent ? 3 : 14;
        const int kStep = 1;                    // 横向采样步长（1px -> 更顺）
        // ★ 几何还原成第一版那套细线条（用户在"变粗"之后要求改回来）；
        //   变淡方向保留"内亮外淡"，并把跨度拉大 —— 最外圈 alpha 归 0，
        //   于是看起来是一层层往外化掉。
        // ⚠ 宽度是**逐层**的，同一层里所有丝线宽度完全一致 ——
        //   "越向外越淡"只靠 alpha 与颜色（饱和度/亮度），**绝不靠变细**
        //   （用户明确否掉"越向外线越细"）。
        // ★ 每根线都发柔光（用户要求）：三层叠加 ——
        //   宽而极淡的外光晕 + 中光晕 + 细亮芯。光晕 alpha 只有 0x08/0x14，
        //   所以线看着仍然是细的，只是周围有一圈柔光(bloom)。
        //   宽度仍是**逐层统一**的：同一层所有丝线一样宽，变淡只靠 alpha/颜色。
        // ★ 不要光晕（用户：不要光晕了，显得太粗了）：只留一根细亮芯。
        //   越向外越淡仍然靠 alpha 与颜色（饱和度/亮度）的线性梯度实现。
        // ★ 光晕压到极限（用户：或者你把光晕压倒极限）：
        //   宽而**几乎不可见** —— 6px 只有 alpha 0x04(1.6%)、2.4px 只有 0x0A(4%)。
        //   宽 + 极低 alpha 观感上是一层光而不是一条粗线，
        //   所以既保留了一点柔光，又不会显得粗。
        // ★ 完全不要光晕（用户："完全不要光晕吧还是"）：只有一根细亮芯。
        //   "越向外越淡"靠 alpha 与颜色（饱和度/亮度）的线性梯度实现。
        struct Pass { float w; uint32_t a0; uint32_t a1; };
        const Pass passes[1] = {
            {0.7f, 0xF0u, 0x08u},               // 仅亮芯：内 0xF0 -> 外 0x08
        };
        for (int pass = 0; pass < 1; ++pass) {
            for (int s = 0; s < kStrands; ++s) {
                const double k = (s + 1) / (double)kStrands;   // 由内到外
                const double ph = t * (0.7 + 0.5 * k) + s * 2.3;
                const uint32_t alpha = (uint32_t)(passes[pass].a0 +
                                                  (passes[pass].a1 - passes[pass].a0) * k);
                int lx = 0, lyT = 0, lyB = 0;
                for (int x = 0; x <= (int)usable; x += kStep) {
                    const double u = x / usable;
                    // 静音时不用（几乎为 0 的）电平包络，改用固定形状的低丝带
                    const double a = maxAmp * (silent ? 1.0 : envAt(u));
                    // ★ 只留两组**低频**抖动（原来还有 17x/41x，那是纯毛刺，
                    //   让线条互相穿插抖成一团，用户反馈"不连贯、不清晰"）。
                    //   低频保留流动感，线条仍是干净连贯的曲线。
                    // ★ 抖动（让丝线互相交错）；静音那三条要平直，所以归零
                    const double wob = silent ? 0.0
                        : (std::sin(u * 5.0 + ph) * 0.30 +
                           std::sin(u * 11.0 + ph * 1.6) * 0.13);
                    double off = a * (k * 0.85 + wob * 0.55);
                    if (off < 0.0) off = 0.0;
                    // ★ 色调（用户可选 黑/白/蓝/红/黄）：每个色调由**两种十分
                    //   相近的色相**掺杂 —— 偶数根用主色相、奇数根用副色相，
                    //   再沿 x 极轻微漂移，于是同一条丝带里有细腻的双色层次，
                    //   而不是一根死色。
                    double hueA, hueB, sat, val;
                    if (g_tone == "black")       { hueA = 0.66; hueB = 0.60; sat = 0.12; val = 0.16; }
                    else if (g_tone == "white")  { hueA = 0.60; hueB = 0.66; sat = 0.07; val = 1.00; }
                    else if (g_tone == "red")    { hueA = 0.99; hueB = 0.945; sat = 0.86; val = 1.00; }
                    else if (g_tone == "yellow") { hueA = 0.145; hueB = 0.105; sat = 0.90; val = 1.00; }
                    else                         { hueA = 0.58; hueB = 0.635; sat = 0.82; val = 1.00; }
                    // ★ 颜色本身也往外淡：外圈的饱和度与亮度都降下来
                    //   （淡是"一层层退色"，不只看 alpha）。
                    sat *= (1.0 - 0.60 * k);
                    val *= (1.0 - 0.22 * k);
                    double hue = ((s % 2 == 0) ? hueA : hueB)
                               + 0.02 * std::sin(u * 5.0 + ph * 0.6);
                    hue -= std::floor(hue);
                    const double hh = hue * 6.0;
                    const int isec = (int)hh;
                    const double ff = hh - isec;
                    const double q = val * (1.0 - sat * ff);
                    const double t2 = val * (1.0 - sat * (1.0 - ff));
                    double rr = val, gg = val, bb = val;
                    switch (isec % 6) {
                        case 0: rr = val; gg = t2;  bb = val * (1.0 - sat); break;
                        case 1: rr = q;   gg = val; bb = val * (1.0 - sat); break;
                        case 2: rr = val * (1.0 - sat); gg = val; bb = t2; break;
                        case 3: rr = val * (1.0 - sat); gg = q;   bb = val; break;
                        case 4: rr = t2;  gg = val * (1.0 - sat); bb = val; break;
                        default: rr = val; gg = val * (1.0 - sat); bb = q; break;
                    }
                    const uint32_t col = (alpha << 24) |
                                         ((uint32_t)std::lround(rr * 255.0) << 16) |
                                         ((uint32_t)std::lround(gg * 255.0) << 8) |
                                         (uint32_t)std::lround(bb * 255.0);
                    const int X = margin + x;
                    // ★ 上下不对称（用户：不要上下对称，上面比下面浮动大）：
                    //   上半个丝带用完整振幅，下半个压到 25%。
                    const int YT = (int)std::lround(cy - off * 1.00);
                    const int YB = (int)std::lround(cy + off * 0.25);
                    if (lx > 0 || x > 0) {
                        // 静音那三条再细一档
                        const float lw = passes[pass].w * (silent ? 0.32f : 1.0f);
                        g_frame.drawLine(lx, lyT, X, YT, col, lw);
                        g_frame.drawLine(lx, lyB, X, YB, col, lw);
                    }
                    lx = X; lyT = YT; lyB = YB;
                }
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
    // ★ 默认位置：靠屏幕下方（留 1/12 屏高的边距）。原来是在垂直方向居中。
    int barY = (g_posY >= 0) ? g_posY : (sh - barH) - sh / 12;   // remembered position
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

// 色调：只影响绘制，立即重画一帧
void setAudioTone(const std::string& tone) {
    if (tone.empty() || tone == g_tone) return;
    g_tone = tone;
    logf("audio: 色调 = %s", g_tone.c_str());
    drawFrame();
}
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
    // ★ 默认位置：靠屏幕下方（留 1/12 屏高的边距）。原来是在垂直方向居中。
    int barY = (g_posY >= 0) ? g_posY : (sh - barH) - sh / 12;   // remembered position
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
