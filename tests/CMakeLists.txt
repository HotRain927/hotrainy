# ── 单元测试 ────────────────────────────────────────────────────────────────
#
# 这两个都是**回归测试** —— 每一个都抓到过一个真 bug：
#   · hotkey_roundtrip：formatHotkey 少写了一个 '+'，
#     于是"录音器写出的字符串"解析器读不懂，改快捷键永远提示"已被占用"。
#     修复前 good=0 bad=255，修复后 good=255 bad=0。
#   · pinyin_forms：拼音表的分隔符写成 "\x00"，
#     被 C 的贪婪 \x 转义吃掉后续字节 → 查表整片错位（读出来是乱码）。
#
# 跑法（构建后直接执行，退出码 0 = 全过）：
#     build\hr_test_hotkey.exe
#     build\hr_test_pinyin.exe
#
# 它们不打进发布包，只在这里当安全网 —— 尤其是做结构重构的时候。

add_executable(hr_test_hotkey
    hotkey_roundtrip_test.cpp
    "${CMAKE_SOURCE_DIR}/src/shared/hotkey.cpp"
)
target_include_directories(hr_test_hotkey PRIVATE "${CMAKE_SOURCE_DIR}/include")

add_executable(hr_test_pinyin
    pinyin_forms_test.cpp
    "${CMAKE_SOURCE_DIR}/src/search/pinyin.cpp"
    "${CMAKE_SOURCE_DIR}/src/search/pinyin_data.cpp"
)
target_include_directories(hr_test_pinyin PRIVATE "${CMAKE_SOURCE_DIR}/include")

# 与主程序一样静态链接 C 运行时（/MT）：免得测试机缺 MSVCP140.dll
foreach(t hr_test_hotkey hr_test_pinyin)
    set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(${t} PRIVATE $<$<CONFIG:Release>:/MT>)
    set_target_properties(${t} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
endforeach()

enable_testing()
add_test(NAME hotkey_roundtrip COMMAND hr_test_hotkey)
add_test(NAME pinyin_forms COMMAND hr_test_pinyin)