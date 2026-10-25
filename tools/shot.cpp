// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// tools/shot.cpp — 把屏幕某块区域存成 PNG，用来肉眼核对材质效果
// 用法: shot.exe out.png x y w h
#include <objidl.h>
#include <windows.h>

#include <gdiplus.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int getEncoderClsid(const wchar_t* mime, CLSID* out) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *out = codecs[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 6) {
        wprintf(L"usage: shot.exe out.png x y w h\n");
        return 1;
    }
    int x = _wtoi(argv[2]), y = _wtoi(argv[3]);
    int w = _wtoi(argv[4]), h = _wtoi(argv[5]);

    SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)-4);

    ULONG_PTR gdi = 0;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdi, &gsi, nullptr);

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = ::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
    ::SelectObject(mem, old);

    bool ok = false;
    {
        Gdiplus::Bitmap gb(bmp, nullptr);
        CLSID clsid;
        if (getEncoderClsid(L"image/png", &clsid) >= 0) {
            ok = gb.Save(argv[1], &clsid, nullptr) == Gdiplus::Ok;
        }
    }
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    Gdiplus::GdiplusShutdown(gdi);

    wprintf(L"%dx%d @(%d,%d) -> %s\n", w, h, x, y, ok ? L"OK" : L"FAIL");
    return ok ? 0 : 2;
}
