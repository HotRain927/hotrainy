#pragma once

namespace hr {

// 搜索模块的接线：搜索窗失去焦点自动收起时，把要关掉的对话框句柄记下来。
// 由 app/main.cpp 创建搜索窗之后调用一次。
void searchWire();

}  // namespace hr
