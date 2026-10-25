// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// cap.cpp — 高频连拍屏幕区域，把"内容变化"的帧存成 BMP，用来抓瞬间频闪
// 用法: cap.exe <outdir> <x> <y> <w> <h> <frames> <interval_ms> "<frame:x,y;frame:x,y>"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

struct Move { int frame, x, y; bool post; };

static FILE* g_log = nullptr;
static void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 9) {
        out("usage: cap.exe outdir x y w h frames interval script\n");
        return 1;
    }
    const char* outdir = argv[1];
    int x = atoi(argv[2]), y = atoi(argv[3]), w = atoi(argv[4]), h = atoi(argv[5]);
    int frames = atoi(argv[6]), interval = atoi(argv[7]);
    std::vector<Move> moves;
    {
        char* p = argv[8];
        while (*p) {
            int f, mx, my;
            if (sscanf(p, "%d:m,%d,%d", &f, &mx, &my) == 3)
                moves.push_back({f, mx, my, true});
            else if (sscanf(p, "%d:%d,%d", &f, &mx, &my) == 3)
                moves.push_back({f, mx, my, false});
            char* semi = strchr(p, ';');
            if (!semi) break;
            p = semi + 1;
        }
    }

    char logp[1024];
    snprintf(logp, sizeof(logp), "%s/log.txt", outdir);
    g_log = fopen(logp, "w");

    timeBeginPeriod(1);
    SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    std::vector<unsigned char> px((size_t)w * h * 4);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    unsigned long long last = 0;
    int saved = 0;
    LARGE_INTEGER freq, t0, tn;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    for (int i = 0; i < frames; ++i) {
        for (auto& m : moves) {
            if (m.frame == i) {
                if (m.post) {
                    HWND bar = FindWindowW(L"HotrainyBar", nullptr);
                    if (bar) {
                        PostMessageW(bar, WM_MOUSEMOVE, 0, MAKELPARAM(m.x, m.y));
                        out("  >>> frame %d: post WM_MOUSEMOVE(%d,%d) -> bar\n", i, m.x, m.y);
                    } else {
                        out("  >>> frame %d: bar window NOT FOUND\n", i);
                    }
                } else {
                    SetCursorPos(m.x, m.y);
                    out("  >>> frame %d: cursor -> (%d,%d)\n", i, m.x, m.y);
                }
            }
        }
        BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
        GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);

        unsigned long long hsh = 1469598103934665603ull;
        long long sum = 0;
        for (size_t k = 0; k < px.size(); k += 4) {
            hsh = (hsh ^ px[k]) * 1099511628211ull;
            sum += px[k] + px[k + 1] + px[k + 2];
        }
        POINT cp = {};
        GetCursorPos(&cp);
        QueryPerformanceCounter(&tn);
        double ms = (tn.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        if (hsh != last) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/f%03d.bmp", outdir, i);
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER ih = bi.bmiHeader;
            ih.biHeight = h;  // 文件里保持正高度，读的时候再翻
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + (DWORD)px.size();
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                fwrite(px.data(), 1, px.size(), f);
                fclose(f);
            }
            out("frame %3d  t=%7.1fms  mean=%6.2f  cur=(%d,%d)  CHANGED -> %s\n", i, ms,
                (double)sum / (px.size() / 4.0) / 3.0, cp.x, cp.y, path);
            last = hsh;
            ++saved;
        }
        if (interval > 0) Sleep(interval);
    }
    out("done, %d distinct frames saved\n", saved);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    timeEndPeriod(1);
    return 0;
}
