# Copyright (c) 2026 HotRain927
# Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# This file is part of hotrainy: noncommercial use only; see LICENSE.

# CMakeLists.txt — hotrainy 构建配置
#
# ⚠ 必须用 MSVC（不是 MinGW）：
#    GlassTAP.dll 依赖 C++/WinRT（winrt::implements、winrt::com_ptr 等），
#    这些在 MinGW/GCC 下无法编译 —— 它们依赖 MSVC ABI 与 __declspec(uuid)。
#    hotrainy 主体本身 MinGW 也能编，但为了统一工具链，整体用 MSVC。
cmake_minimum_required(VERSION 3.20)
# 需要 C 和 CXX：
#   C   —— GlassBoot.dll 引导层必须用纯 C 编译
#   CXX —— GlassTAP.dll 功能层与主程序用 C++/WinRT
project(hotrainy LANGUAGES C CXX RC)

# ═══════════════════════════════════════════════════════════════════════
# 调试信息开关
#
# ⚠ 为什么 Release 也要 PDB：
#   这次一个崩溃卡了很久 —— cdb 只能给出 hotrainy+0x348a2 这种裸地址，
#   没有符号表就看不出是哪个函数、哪一行。
#   为了几 MB 的磁盘空间牺牲排查能力不值得。
#
#   PDB 只在本地保留，发布时排除掉（用 使用说明.txt 里的打包脚本）。
# ═══════════════════════════════════════════════════════════════════════
set(HR_DEBUG_INFO ON CACHE BOOL "生成 PDB 调试信息（Release 也开）")

function(hr_enable_debug_info target)
    if(HR_DEBUG_INFO)
        target_compile_options(${target} PRIVATE /Zi)
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()
endfunction()


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 公共编译选项 ─────────────────────────────────────────────────────
add_compile_definitions(
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)
if(MSVC)
    add_compile_options(/W3 /utf-8 /EHsc /permissive- /Zc:__cplusplus)
    add_compile_options($<$<CONFIG:Release>:/O2>)
endif()

# ── C++/WinRT ────────────────────────────────────────────────────────
# 优先用 NuGet 包里的头文件；没有就用 Windows SDK 自带的。
find_path(CPPWINRT_INCLUDE_DIR
    NAMES winrt/base.h
    PATHS
        "${CMAKE_SOURCE_DIR}/packages/Microsoft.Windows.CppWinRT.*/include"
        "$ENV{USERPROFILE}/.nuget/packages/microsoft.windows.cppwinrt"
    PATH_SUFFIXES include
)
if(NOT CPPWINRT_INCLUDE_DIR)
    # SDK 自带（10.0.22621+ 有 cppwinrt/winrt）
    find_path(CPPWINRT_INCLUDE_DIR
        NAMES winrt/base.h
        PATHS "$ENV{WindowsSdkDir}Include" 
        PATH_SUFFIXES cppwinrt
    )
endif()
message(STATUS "C++/WinRT include: ${CPPWINRT_INCLUDE_DIR}")

# ── 1. GlassBoot.dll（纯 C 引导层）───────────────────────────────────
#
# ⚠ 这个 DLL 必须是纯 C，且不能链接 C++ 运行时。
#
# 原因（实测确定）：InitializeXamlDiagnosticsEx 要求在
# "进程完全没有 COM/WinRT 初始化痕迹"的状态下调用。
# C++/WinRT 在 DLL 加载时会跑静态初始化（含套间初始化），
# 一旦发生，该 API 必然返回 0x80070490 (E_NOTFOUND)。
#
# 对照证据：
#   纯 C 的 DLL，同参数同线程  -> 0x00000000  成功
#   用 C++/WinRT 的 DLL        -> 0x80070490  失败
#
# 所以引导职责单独成层，抢在所有 C++ 代码之前执行。
add_library(GlassBoot SHARED
    src/tb/xaml/glassboot.c
    src/tb/xaml/crashguard.c
)
hr_enable_debug_info(GlassBoot)
target_include_directories(GlassBoot PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)
set_source_files_properties(
    src/tb/xaml/glassboot.c
    src/tb/xaml/crashguard.c
    PROPERTIES LANGUAGE C
)
set_target_properties(GlassBoot PROPERTIES
    OUTPUT_NAME "GlassBoot"
    PREFIX ""
    LINK_FLAGS "/DEF:\"${CMAKE_SOURCE_DIR}/src/tb/xaml/GlassBoot.def\""
)
if(MSVC)
    # 静态 CRT + 不下沉任何 C++ 运行时
    set_property(TARGET GlassBoot PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_link_libraries(GlassBoot PRIVATE user32 kernel32)
endif()


# ── 2. GlassTAP.dll（C++/WinRT 功能层）───────────────────────────────
file(GLOB TAP_SOURCES "${CMAKE_SOURCE_DIR}/src/tb/xaml/*.cpp")
# injector 属于主程序侧；bootstrap 是旧的单 DLL 方案，已被 GlassBoot 取代
list(REMOVE_ITEM TAP_SOURCES "${CMAKE_SOURCE_DIR}/src/tb/xaml/injector.cpp")
list(REMOVE_ITEM TAP_SOURCES "${CMAKE_SOURCE_DIR}/src/tb/xaml/bootstrap.cpp")

add_library(GlassTAP SHARED ${TAP_SOURCES})
hr_enable_debug_info(GlassTAP)
target_include_directories(GlassTAP PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/tb/xaml"
)
if(CPPWINRT_INCLUDE_DIR)
    target_include_directories(GlassTAP PRIVATE "${CPPWINRT_INCLUDE_DIR}")
endif()

# ⚠ 必须静态链接 CRT。
#
# 这个 DLL 要注入 explorer.exe。动态链接 CRT 会引入
# MSVCP140.dll / VCRUNTIME140.dll 依赖 —— explorer 进程里不一定有，
# 或者版本不匹配，导致 DllMain 或后续调用静默失败。
#
# 对照实测：TranslucentTB 的 ExplorerTAP.dll 依赖表极简，
# 只有 RPCRT4/USER32/KERNEL32 + winrt-* 系列，没有 MSVC 运行时。
if(MSVC)
    set_property(TARGET GlassTAP PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(GlassTAP PRIVATE $<$<CONFIG:Release>:/MT>)
    target_link_options(GlassTAP PRIVATE $<$<CONFIG:Release>:/NODEFAULTLIB:MSVCRT>)
endif()

# Windows Runtime 支持：/ZW 已过时，用 C++/WinRT 需要这些库
target_link_libraries(GlassTAP PRIVATE
    windowsapp          # Windows Runtime
    runtimeobject       # WinRT 运行时对象
    ole32 oleaut32 uuid
    user32
)
target_compile_definitions(GlassTAP PRIVATE
    _WINRT_DLL
)

set_target_properties(GlassTAP PROPERTIES
    OUTPUT_NAME "GlassTAP"
    PREFIX ""
    # ⚠ 必须用 .def 导出 DllGetClassObject：
    #   MSVC 默认不导出任何符号，没有 .def 时系统找不到类工厂，
    #   InitializeXamlDiagnosticsEx 会返回 0x80070490 (E_NOTFOUND)。
    LINK_FLAGS "/DEF:\"${CMAKE_SOURCE_DIR}/src/tb/xaml/GlassTAP.def\""
)

# ── 2. hotrainy.exe（主程序）─────────────────────────────────────────
# ★ 按模块目录收集源文件（重构后每个模块一个子目录）。
#   注意用逐目录列举而不是 GLOB_RECURSE，避免把 src/tb/** 重复收进来。
file(GLOB HR_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
foreach(mod app tagbar island settings search text audio glass shared)
    file(GLOB _HR_MOD "${CMAKE_SOURCE_DIR}/src/${mod}/*.cpp")
    list(APPEND HR_SOURCES ${_HR_MOD})
endforeach()
file(GLOB HR_TB_SOURCES "${CMAKE_SOURCE_DIR}/src/tb/*.cpp")
file(GLOB HR_XAML_SOURCES "${CMAKE_SOURCE_DIR}/src/tb/xaml/*.cpp")
# ★ Win10 适配：accents 亚克力任务栏（阶段 2），单独一个子目录
file(GLOB HR_ACCENT_SOURCES "${CMAKE_SOURCE_DIR}/src/tb/accent/*.cpp")
list(APPEND HR_SOURCES ${HR_TB_SOURCES} ${HR_XAML_SOURCES} ${HR_ACCENT_SOURCES})
# json.cpp 来自 tbg 子工程
list(APPEND HR_SOURCES "${CMAKE_SOURCE_DIR}/third_party/tbg/src/json.cpp")

add_executable(hotrainy WIN32 ${HR_SOURCES}
    res/hotrainy.rc)
hr_enable_debug_info(hotrainy)
target_include_directories(hotrainy PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third_party/tbg/include"
)
if(CPPWINRT_INCLUDE_DIR)
    target_include_directories(hotrainy PRIVATE "${CPPWINRT_INCLUDE_DIR}")
endif()

# ⚠ 入口点必须是 main，不是 WinMain。
#
# 主程序用的是标准 int main()（原 MinGW 构建靠 -municode 与默认启动代码
# 配成 main），而 CMake 的 WIN32 子系统默认期望 WinMain。
# 不指定的话链接报 LNK2019 "无法解析的外部符号 WinMain"。
if(MSVC)
    target_link_options(hotrainy PRIVATE "/ENTRY:mainCRTStartup")

    # ⚠ 静态链接 C 运行时（/MT）。
    #
    # 默认是 /MD，会依赖 MSVCP140.dll / VCRUNTIME140.dll。
    # 那两个文件只有装了 VC++ 可再发行组件的机器才有 ——
    # 用户拿到 exe 直接双击会报"找不到 MSVCP140.dll"。
    #
    # 改成静态链接后整个包零运行时依赖，拷贝即用。
    # GlassBoot / GlassTAP 已经是 /MT，这里保持一致。
    #
    # 代价是 exe 大几百 KB。对这种工具来说完全值得。
    set_property(TARGET hotrainy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(hotrainy PRIVATE $<$<CONFIG:Release>:/MT>)
    target_link_options(hotrainy PRIVATE $<$<CONFIG:Release>:/NODEFAULTLIB:MSVCRT>)
endif()

target_link_libraries(hotrainy PRIVATE
    gdiplus
    user32 gdi32 shell32 advapi32 ole32 oleaut32 uuid
    comdlg32 shlwapi dwmapi comctl32 shcore
    imm32          # ImmSetCompositionWindow —— 把输入法组合窗口挪到搜索框外
    wtsapi32
    windowsapp runtimeobject
    msimg32
)

# TAP DLL 输出到和主程序同一个目录，注入器按相对路径找它
set_target_properties(hotrainy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build"
)
set_target_properties(GlassTAP PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build"
)


# ── 6. 单元测试（回归安全网，不打进发布包）──
add_subdirectory(tests)
