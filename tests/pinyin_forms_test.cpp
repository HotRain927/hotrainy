// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// Pinyin forms self test (ASCII-only source; CJK written as \uXXXX escapes).
//
// Verifies that Chinese file names produce both a full-pinyin string and an
// initials string, which is what the search matcher now scores against.
#include <string>

#include "hr/search/pinyin.h"

#include <cstdio>

static int g_fails = 0;

static void check(const wchar_t* text, const char* wantFull, const char* wantIni,
                  const char* wantAlt = nullptr) {
    hr::PyForms f;
    hr::pinyinForms(text, f);
    bool ok = (f.full == wantFull) && (f.initials == wantIni);
    if (wantAlt) ok = ok && (f.fullAlt == wantAlt);
    if (!ok) ++g_fails;
    std::printf("%s full=\"%s\" (want \"%s\") ini=\"%s\" (want \"%s\") alt=\"%s\"%s\n",
                ok ? "ok  " : "FAIL", f.full.c_str(), wantFull, f.initials.c_str(), wantIni,
                f.fullAlt.c_str(), wantAlt ? "" : " (alt not checked)");
}

int main() {
    // 工作报表.xlsx / 工作报表
    check(L"\u5DE5\u4F5C\u62A5\u8868.xlsx", "gongzuobaobiaoxlsx", "gzbbxlsx");
    check(L"\u5DE5\u4F5C\u62A5\u8868", "gongzuobaobiao", "gzbb");
    // 新建文件夹
    check(L"\u65B0\u5EFA\u6587\u4EF6\u5939", "xinjianwenjianjia", "xjwjj");
    // 银行 (polyphonic: hang should show up in fullAlt)
    check(L"\u94F6\u884C", "yinxing", "yx", "yinhang");
    // pure ASCII name -> no pinyin forms at all
    check(L"readme.txt", "", "");
    // mixed: "abc" + 报告
    check(L"abc\u62A5\u544A", "abcbaogao", "abcbg");

    std::printf("pinyin forms: fails=%d\n", g_fails);
    return g_fails ? 1 : 0;
}