// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// icon_probe.cpp — 图标提取对照测试（不进主程序）
// 目的：搞清 SHGetFileInfoW / IShellItemImageFactory / SHGetImageList
//      在本机对不同路径写法分别返回什么，并把图标存成 PNG 肉眼看。
//
// 构建：g++ -std=c++17 -O1 -DUNICODE -D_UNICODE icon_probe.cpp -o icon_probe.exe
//        -lgdiplus -lole32 -lshell32 -luuid
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma GCC diagnostic ignored "-Wcast-function-type"

static int saveIconPng(HICON icon, int size, const char* outPath) {
    using namespace Gdiplus;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    memset(bits, 0, (size_t)size * size * 4);

    DrawIconEx(mem, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);

    // DIB 是 BGRA，转成 GDI+ Bitmap 保存
    Bitmap bmp(size, size, size * 4, PixelFormat32bppARGB, (BYTE*)bits);
    CLSID pngClsid;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &pngClsid);
    std::wstring w(outPath, outPath + strlen(outPath));
    Status st = bmp.Save(w.c_str(), &pngClsid, nullptr);

    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
    ReleaseDC(nullptr, screen);
    return st == Ok ? 0 : 1;
}

static int saveHBitmapPng(HBITMAP hbmp, const char* outPath) {
    using namespace Gdiplus;
    Bitmap bmp(hbmp, nullptr);
    CLSID pngClsid;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &pngClsid);
    std::wstring w(outPath, outPath + strlen(outPath));
    return bmp.Save(w.c_str(), &pngClsid, nullptr) == Ok ? 0 : 1;
}

int main(int argc, char** argv) {
    using namespace Gdiplus;
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::vector<std::wstring> paths;
    for (int i = 1; i < argc; ++i) {
        // 参数按 UTF-8 传进来
        int n = MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, &w[0], n);
        paths.push_back(w);
    }

    int idx = 0;
    for (const auto& p : paths) {
        ++idx;
        std::wstring norm = p;
        for (auto& c : norm)
            if (c == L'/') c = L'\\';

        printf("=== [%d] %ls\n", idx, p.c_str());

        // A. SHGetFileInfoW 原始写法（正斜杠）
        SHFILEINFOW sfi = {};
        DWORD_PTR r = SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi),
                                     SHGFI_ICON | SHGFI_LARGEICON | SHGFI_TYPENAME |
                                         SHGFI_DISPLAYNAME);
        printf("  A SHGFI(原样)   ret=%llu type=%ls disp=%ls hIcon=%p\n",
               (unsigned long long)r, sfi.szTypeName, sfi.szDisplayName, (void*)sfi.hIcon);
        if (sfi.hIcon) {
            char buf[128];
            snprintf(buf, sizeof(buf), "tools/_icon_%d_A.png", idx);
            saveIconPng(sfi.hIcon, 64, buf);
            DestroyIcon(sfi.hIcon);
        }

        // B. SHGetFileInfoW 规范化后的路径
        SHFILEINFOW sfi2 = {};
        r = SHGetFileInfoW(norm.c_str(), 0, &sfi2, sizeof(sfi2),
                           SHGFI_ICON | SHGFI_LARGEICON | SHGFI_TYPENAME | SHGFI_DISPLAYNAME);
        printf("  B SHGFI(反斜杠) ret=%llu type=%ls disp=%ls hIcon=%p\n", (unsigned long long)r,
               sfi2.szTypeName, sfi2.szDisplayName, (void*)sfi2.hIcon);
        if (sfi2.hIcon) {
            char buf[128];
            snprintf(buf, sizeof(buf), "tools/_icon_%d_B.png", idx);
            saveIconPng(sfi2.hIcon, 64, buf);
            DestroyIcon(sfi2.hIcon);
        }

        // C. IShellItemImageFactory（大图 + 带 alpha）
        IShellItem* item = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(norm.c_str(), nullptr, IID_IShellItem,
                                                 (void**)&item);
        if (SUCCEEDED(hr) && item) {
            IShellItemImageFactory* fac = nullptr;
            hr = item->QueryInterface(IID_IShellItemImageFactory, (void**)&fac);
            if (SUCCEEDED(hr) && fac) {
                SIZE sz = {96, 96};
                HBITMAP hb = nullptr;
                hr = fac->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb);
                if (SUCCEEDED(hr) && hb) {
                    BITMAP bm = {};
                    GetObject(hb, sizeof(bm), &bm);
                    printf("  C IShellItemImageFactory ok %ldx%ld bpp=%u\n", bm.bmWidth,
                           bm.bmHeight, bm.bmBitsPixel);
                    char buf[128];
                    snprintf(buf, sizeof(buf), "tools/_icon_%d_C.png", idx);
                    saveHBitmapPng(hb, buf);
                    DeleteObject(hb);
                } else {
                    printf("  C IShellItemImageFactory GetImage 失败 hr=0x%08lX\n", (unsigned long)hr);
                }
                fac->Release();
            }
            item->Release();
        } else {
            printf("  C SHCreateItemFromParsingName 失败 hr=0x%08lX\n", (unsigned long)hr);
        }

        // D. IImageList JUMBO（256）
        SHFILEINFOW sfi3 = {};
        r = SHGetFileInfoW(norm.c_str(), 0, &sfi3, sizeof(sfi3), SHGFI_SYSICONINDEX);
        printf("  D SHGFI(SYSICONINDEX) ret=%llu iIcon=%d\n", (unsigned long long)r, sfi3.iIcon);
    }

    CoUninitialize();
    GdiplusShutdown(token);
    return 0;
}
