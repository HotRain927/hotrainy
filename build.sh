#!/bin/sh
# build.sh — 构建 hotrainy 的 C++ 版（mingw-w64 g++）
set -e

# 必须用 posix 线程模型的 g++（搜索索引用了 std::thread / std::mutex）。
# /d/Tools/mingww 那份是 win32 线程模型，编不过，不要用。
GXX="${GXX:-/d/Tools/Qt/Tools/mingw1310_64/bin/g++.exe}"
cd "$(dirname "$0")"
mkdir -p build

CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
  -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DUNICODE -D_UNICODE \
  -Iinclude -I../tbg/include"

LIBS="-luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -luuid -lgdiplus -lcomdlg32 \
  -lshlwapi -ldwmapi \
  -static -static-libgcc -static-libstdc++"

echo "[build] 编译 hotrainy…"
"$GXX" $CXXFLAGS -mwindows \
  src/*.cpp \
  ../tbg/src/json.cpp \
  -o build/hotrainy.exe $LIBS

echo "[build] 完成 -> build/hotrainy.exe"
ls -la build/hotrainy.exe
