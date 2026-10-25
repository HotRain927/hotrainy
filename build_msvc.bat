@echo off
REM build_msvc.bat — 用 MSVC 构建 hotrainy + GlassTAP.dll
REM
REM 为什么必须用 MSVC：GlassTAP.dll 依赖 C++/WinRT，
REM   MinGW/GCC 无法编译 winrt::implements 等模板（MSVC ABI 依赖）。

setlocal

set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo [build] 找不到 vcvars64.bat
    exit /b 1
)

call %VCVARS% >nul 2>&1
if errorlevel 1 (
    echo [build] vcvars64 初始化失败
    exit /b 1
)

cd /d "%~dp0"
if not exist build mkdir build
cd build

echo [build] 配置 CMake...
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release .. 2>&1
if errorlevel 1 (
    echo [build] CMake 配置失败，尝试 NMake...
    cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ..
    if errorlevel 1 exit /b 1
)

echo.
echo [build] 编译...
cmake --build . --config Release 2>&1
if errorlevel 1 exit /b 1

echo.
echo [build] 产物:
dir /b *.exe *.dll 2>nul

endlocal