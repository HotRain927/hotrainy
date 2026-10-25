// hr/icons.cpp
#include "hr/shared/icons.h"

#include <algorithm>
#include <cstring>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "hr/shared/common.h"

namespace hr {

namespace {

struct Key {
    std::wstring path;
    int px;
    bool operator==(const Key& o) const { return px == o.px && path == o.path; }
};

struct KeyHash {
    size_t operator()(const Key& k) const {
        size_t h = std::hash<std::wstring>()(k.path);
        return h ^ ((size_t)k.px * 2654435761u);
    }
};

std::unordered_map<Key, HBITMAP, KeyHash> g_cache;

}  // namespace

std::wstring shellPath(const std::wstring& in) {
    std::wstring s = in;
    for (auto& c : s) {
        if (c == L'/') c = L'\\';
    }
    return s;
}

// 对齐 Python icon_service.py:212 `_resolve_lnk`：把 .lnk 解析成目标路径，
// 否则对 .lnk 取图标经常拿不到目标程序的那张图。
std::wstring resolveLnk(const std::wstring& lnkPath) {
    std::wstring result;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link)))
        return result;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        if (SUCCEEDED(pf->Load(lnkPath.c_str(), STGM_READ))) {
            wchar_t buf[MAX_PATH] = {};
            if (SUCCEEDED(link->GetPath(buf, MAX_PATH, nullptr, SLGP_UNCPRIORITY)) && buf[0])
                result = buf;
        }
        pf->Release();
    }
    link->Release();
    return result;
}

HBITMAP iconToBitmap(HICON icon, int px) {
    if (!icon || px <= 0) return nullptr;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    memset(bits, 0, (size_t)px * px * 4);
    DrawIconEx(mem, 0, 0, icon, px, px, 0, nullptr, DI_NORMAL);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    // 老式图标没有 alpha 通道，画上去 alpha 全 0 → 贴上去就是全透明
    const uint8_t* p = (const uint8_t*)bits;
    bool anyAlpha = false;
    for (int i = 0; i < px * px; ++i) {
        if (p[i * 4 + 3] != 0) {
            anyAlpha = true;
            break;
        }
    }
    if (!anyAlpha) {
        uint8_t* q = (uint8_t*)bits;
        for (int i = 0; i < px * px; ++i) q[i * 4 + 3] = 255;
    }
    return dib;
}

namespace {

HBITMAP loadUncached(const std::wstring& rawPath, int px) {
    std::wstring path = shellPath(rawPath);
    if (path.empty()) return nullptr;

    // icon_service.py:196-199：.lnk 先解析成目标 .exe 再取图标
    std::wstring actual = path;
    if (path.size() >= 4 && _wcsicmp(path.c_str() + path.size() - 4, L".lnk") == 0) {
        std::wstring target = resolveLnk(path);
        if (!target.empty() && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
            actual = target;
    }

    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(actual.c_str(), nullptr, IID_IShellItem,
                                              (void**)&item)) &&
        item) {
        IShellItemImageFactory* fac = nullptr;
        if (SUCCEEDED(item->QueryInterface(IID_IShellItemImageFactory, (void**)&fac)) && fac) {
            // 要一份比实际显示更大的原图（96 起），缩下来比直接要 30x30 清楚得多
            SIZE sz = {std::max(96, px * 2), std::max(96, px * 2)};
            HBITMAP hb = nullptr;
            HRESULT hr = fac->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb);
            fac->Release();
            item->Release();
            if (SUCCEEDED(hr) && hb) return hb;
        } else {
            item->Release();
        }
    }

    SHFILEINFOW sfi = {};
    if (!SHGetFileInfoW(actual.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) ||
        !sfi.hIcon) {
        return nullptr;
    }
    HBITMAP bmp = iconToBitmap(sfi.hIcon, px);
    DestroyIcon(sfi.hIcon);
    return bmp;
}

}  // namespace

HBITMAP iconFor(const std::wstring& path, int px) {
    Key k{path, px};
    auto it = g_cache.find(k);
    if (it != g_cache.end()) return it->second;
    HBITMAP bmp = loadUncached(path, px);
    g_cache[k] = bmp;
    return bmp;
}

void clearIconCache() {
    for (auto& kv : g_cache) {
        if (kv.second) DeleteObject(kv.second);
    }
    g_cache.clear();
}

void pruneIconCache(size_t keepAtMost) {
    while (g_cache.size() > keepAtMost) {
        auto it = g_cache.begin();
        if (it->second) DeleteObject(it->second);
        g_cache.erase(it);
    }
}

}  // namespace hr
