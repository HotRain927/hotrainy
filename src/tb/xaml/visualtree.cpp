// hr/tb/xaml/visualtree.cpp — XAML 视觉树监听：定位并替换任务栏背景
//
// 实现 IVisualTreeServiceCallback3，接收 explorer 的 XAML 树变更通知。
//
// 关键发现（来自实测与 XAML 结构分析）：
//   任务栏的 XAML 树形如：
//     DesktopWindowXamlSource
//       └─ RootGrid
//            └─ Taskbar.TaskbarFrame
//                 ├─ Rectangle  Name="BackgroundFill"    ← 背景！就是它
//                 └─ Rectangle  Name="BackgroundStroke"  ← 边框
//
//   只要拿到 BackgroundFill 那个 Rectangle 的 Fill 属性并替换，
//   任务栏背景就变了 —— 图标是同一棵树上别的兄弟节点，完全不受影响。
//   这正是"背景透明而图标保持清晰"能实现的原理。
#include "hr/tb/xaml/interfaces.h"

#include "hr/tb/xaml/taplog.h"
#include "hr/tb/xaml/channel.h"
#include "hr/text/island_theme.h"   // 岛台配色常量（单一来源，轻量头）

#include <xamlOM.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <atomic>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace hr {
namespace tb {
namespace xaml {

namespace wux  = winrt::Windows::UI::Xaml;
namespace wuxh = winrt::Windows::UI::Xaml::Hosting;
namespace wuxm = winrt::Windows::UI::Xaml::Media;

namespace {

// 每个任务栏记录：背景元素 + 它的原始画刷（还原用）+ 边框元素
struct TaskbarEntry {
    HWND window = nullptr;
    wux::Shapes::Rectangle background{nullptr};
    wuxm::Brush originalFill{nullptr};
    // RegisterPropertyChangedCallback 返回 int64_t，不是 winrt::event_token
    // （实测：用 event_token 会报 C2679「没有找到接受 int64_t 的运算符」）
    int64_t fillToken = 0;
    wux::Shapes::Rectangle border{nullptr};
    wuxm::Brush originalBorderFill{nullptr};
    int64_t borderToken = 0;

    // ── 岛台接管模式要隐藏的任务栏元素 ──
    //
    // 开启岛台后，任务栏上的「开始」和「搜索」入口由岛台接管，
    // 原按钮要藏起来，否则入口重复。
    //
    // 只记录元素本身，不记画刷 —— 这些元素的隐藏与还原都用 Opacity，
    // 和背景层同一条路径（Shape.Fill 写不进去，Opacity 可以）。
    std::vector<wux::UIElement> islandHidden;

    // ── 开始按钮所在的容器 ──
    //
    // ⚠ 这是岛台按钮该放的地方，不是一个"随便找到的 Grid"。
    //
    //   之前踩的坑：插按钮时遍历所有元素、找第一个能转成 Grid 的节点，
    //   结果那个 Grid 跟开始按钮根本没关系 ——
    //   按钮插进去之后位置飘在任务栏最左边，和图标组隔着老远。
    //
    //   正确做法：用回调里的 relation.Parent 记下开始按钮的**父容器**，
    //   把岛台按钮插到同一个容器、同一个下标位置，顶替它。
    //   这样它就"长在"开始按钮原来的坑里，和图标组连成一体。
    wux::Controls::Panel islandHostPanel{nullptr};
    unsigned int islandHostIndex = 0;
    bool islandHostValid = false;

    // 开始按钮在容器里的布局坐标。
    //
    // ⚠ 光有下标不够。
    //   岛台按钮插进去之后，实测坐标是 (-10000,-10016) ——
    //   那是 XAML 的“未参与布局”哨兵值，意味着容器
    //   用附加属性（Grid.Row/Column 之类）定位子元素，
    //   而我们插进去的按钮什么都没设，于是被丢到屏幕外。
    //
    //   修法：把开始按钮的布局属性读出来，
    //   设成一样的。这样它就落在开始按钮原来的格子里。
    int islandStartCol = 0;
    int islandStartRow = 0;

    // 开始按钮原来在屏幕上的位置和尺寸（XAML 树内坐标，单位 DIP）。
    //
    // ⚠ 必须在把它 Collapsed 之前读。
    //   隐藏之后 ActualWidth 变 0，坐标也不再更新。
    double islandStartX = 0;
    double islandStartY = 0;
    double islandStartW = 0;
    double islandStartH = 0;
    bool islandStartPosValid = false;

    // 开始按钮的句柄，用来持续跟踪它的位置。
    //
    // 图标数量变化时任务栏会重新排版，居中组的起点跟着变 ——
    // 所以不能只读一次。留着句柄，定时重新读。
    InstanceHandle islandStartHandle = 0;

    // 开始按钮是不是 ContentControl（能换内容）。
    // 如果是，就把岛台图标放进它的 Content —— 那是"真嵌入"的最简路径。
    bool islandStartIsContentControl = false;

    // 是否已经成功把岛台图标嵌进开始按钮的 Content。
    // 嵌入成功就不需要那个独立窗口了。
    bool islandEmbedded = false;
};

std::mutex g_mu;
// key 是 TaskbarFrame 的 InstanceHandle
std::map<InstanceHandle, TaskbarEntry> g_taskbars;
// 已创建但还没匹配到 TaskbarFrame 的 XAML 源
std::set<InstanceHandle> g_pendingSources;

IXamlDiagnostics* g_diag = nullptr;

// 我们自己的 XAML 线程消息窗口（HWND_MESSAGE）。
//
// 定时器要挂它上面：挂 Shell_TrayWnd 没用，
// 那个窗口的消息不经过我们的 WndProc，定时器永远不会触发。
HWND g_xamlThreadWnd = nullptr;

// 我们自己的 XAML 线程消息窗口。定时器要挂在它上面 ——
// 挂在 Shell_TrayWnd 上没用，那个窗口的消息不经过我们的 WndProc。

// 从 InstanceHandle 还原成 WinRT 对象
template <typename T>
T fromHandle(InstanceHandle h) {
    if (!g_diag || !h) return nullptr;
    ::IInspectable* insp = nullptr;
    if (FAILED(g_diag->GetIInspectableFromHandle(h, &insp)) || !insp) return nullptr;
    // 接管引用
    winrt::com_ptr<::IInspectable> holder;
    holder.attach(insp);
    return holder.try_as<T>();
}

// 沿 XAML 树向上找指定名字的 FrameworkElement
wux::FrameworkElement findParent(const wchar_t* name, wux::FrameworkElement element) {
    int guard = 0;
    while (element && guard++ < 64) {
        auto parent = wuxm::VisualTreeHelper::GetParent(element).try_as<wux::FrameworkElement>();
        if (!parent) return nullptr;
        if (parent.Name() == name) return parent;
        element = parent;
    }
    return nullptr;
}

// 把 ABGR 转成 WinRT Color
winrt::Windows::UI::Color fromAbgr(UINT abgr) {
    winrt::Windows::UI::Color c;
    c.A = (uint8_t)((abgr >> 24) & 0xFF);
    c.B = (uint8_t)((abgr >> 16) & 0xFF);
    c.G = (uint8_t)((abgr >> 8) & 0xFF);
    c.R = (uint8_t)(abgr & 0xFF);
    return c;
}

// 让任务栏背景变透明。
//
// ═══════════════════════════════════════════════════════════════════
// 【为什么这样做，而不是改 Fill】
//
// 最直觉的做法是把 BackgroundFill 的 Fill 换成透明画刷。实测不行：
//
//   element.Fill(transparentBrush)                       -> 0x80004005
//   SetProperty(bg, 自造画刷句柄, Fill索引)              -> 0x80004005
//   SetProperty(bg, CreateInstance造的句柄, Fill索引)    -> 0x80004005
//   ClearProperty(bg, Fill索引)                          -> 0x80004005
//
// 同一个索引 GetProperty 却是成功的，说明索引没错 ——
// 是 XAML 诊断的写路径不接受对 Shape.Fill 这类依赖属性的写入。
//
// 【实际有效的做法】把背景元素的 Opacity 设为 0。
//
//   GetPropertyIndex(UIElement.Opacity) -> idx=453
//   CreateInstance(Double, "0")         -> handle
//   SetProperty(bg, handle, 453)        -> S_OK      ← 生效
//
// Opacity 是普通依赖属性，可以被诊断服务写入。
// 背景元素整体透明后，那层挡住壁纸的矩形就消失了，
// 而图标在视觉树的兄弟节点上，完全不受影响 ——
// 这正是"背景全透、图标清晰"想要的效果。
//
// 【两个必须遵守的细节】
//   1. 属性名必须写全："Windows.UI.Xaml.UIElement.Opacity"。
//      裸写 "Opacity" 会返回 0x80070057 (E_INVALIDARG)。
//   2. 值对象必须由诊断服务 CreateInstance 造，不能自己 new 一个
//      WinRT 对象再取句柄 —— 系统只认自己创建的句柄。
// ═══════════════════════════════════════════════════════════════════
HRESULT setElementOpacityZero(InstanceHandle target) {
    if (!g_diag) return (HRESULT)E_FAIL;

    // 服务接口：优先 v3，退到 v2
    winrt::com_ptr<IUnknown> unk;
    HRESULT hrQi = g_diag->QueryInterface(__uuidof(IVisualTreeService3), unk.put_void());
    if (FAILED(hrQi) || !unk) {
        hrQi = g_diag->QueryInterface(__uuidof(IVisualTreeService2), unk.put_void());
    }
    if (FAILED(hrQi) || !unk) {
        tapLogf("xaml: 取 VisualTreeService 失败 0x%08X", (unsigned)hrQi);
        return hrQi;
    }

    auto vts  = unk.try_as<IVisualTreeService>();
    auto vts2 = unk.try_as<IVisualTreeService2>();
    if (!vts || !vts2) {
        tapLogf("xaml: 服务接口转换失败");
        return (HRESULT)E_FAIL;
    }

    // 属性索引（完整属性名）
    unsigned int opIndex = 0;
    HRESULT hr = vts2->GetPropertyIndex(
        target, L"Windows.UI.Xaml.UIElement.Opacity", &opIndex);
    tapLogf("xaml: GetPropertyIndex(Opacity) -> 0x%08X idx=%u", (unsigned)hr, opIndex);
    if (FAILED(hr)) return hr;

    // 由诊断服务创建 double 值 0
    BSTR typeName = ::SysAllocString(L"Windows.Foundation.Double");
    BSTR valueStr = ::SysAllocString(L"0");
    InstanceHandle zero = 0;
    hr = vts->CreateInstance(typeName, valueStr, &zero);
    ::SysFreeString(typeName);
    ::SysFreeString(valueStr);
    if (FAILED(hr) || !zero) {
        tapLogf("xaml: CreateInstance(Double,0) 失败 0x%08X", (unsigned)hr);
        return hr;
    }

    hr = vts->SetProperty(target, zero, opIndex);
    tapLogf("xaml: SetProperty(Opacity=0) -> 0x%08X", (unsigned)hr);
    return hr;
}

// 把一个元素从布局里摘掉（Visibility = Collapsed）。
//
// ═══════════════════════════════════════════════════════════════════
// 【为什么必须用 Visibility 而不是 Opacity】
//
//   Opacity = 0 只是"画成透明的"：
//     · 元素仍占布局空间
//     · 仍参与命中测试 —— **点它还能点开**
//   实测确认：隐藏任务栏开始/搜索后，原来的位置依然能点出开始菜单和搜索框。
//
//   Visibility = Collapsed 才是真正的"取消固定"：
//     · 不占布局空间，后面的元素会顶上来
//     · 不参与命中测试 —— 点不到了
//
// 【枚举值怎么传】
//
//   Opacity 是 Double，CreateInstance("Windows.Foundation.Double", "0") 就行。
//   Visibility 是**枚举**，CreateInstance 需要的是它的底层数值 ——
//   Windows.UI.Xaml.Visibility 是 int：0=Visible, 1=Collapsed。
//
//   用 "Windows.UI.Xaml.Visibility" 作为类型名 + 数值字符串 "1"。
//   实测这样能造出值对象并写入成功。
// ═══════════════════════════════════════════════════════════════════
HRESULT setElementCollapsed(InstanceHandle target) {
    if (!g_diag) return (HRESULT)E_FAIL;

    winrt::com_ptr<IUnknown> unk;
    HRESULT hrQi = g_diag->QueryInterface(__uuidof(IVisualTreeService3), unk.put_void());
    if (FAILED(hrQi) || !unk) {
        hrQi = g_diag->QueryInterface(__uuidof(IVisualTreeService2), unk.put_void());
    }
    if (FAILED(hrQi) || !unk) return hrQi;

    auto vts  = unk.try_as<IVisualTreeService>();
    auto vts2 = unk.try_as<IVisualTreeService2>();
    if (!vts || !vts2) return (HRESULT)E_FAIL;

    unsigned int visIndex = 0;
    HRESULT hr = vts2->GetPropertyIndex(
        target, L"Windows.UI.Xaml.UIElement.Visibility", &visIndex);
    tapLogf("xaml: GetPropertyIndex(Visibility) -> 0x%08X idx=%u", (unsigned)hr, visIndex);
    if (FAILED(hr)) return hr;

    // 枚举值：Collapsed = 1
    BSTR typeName = ::SysAllocString(L"Windows.UI.Xaml.Visibility");
    BSTR valueStr = ::SysAllocString(L"1");
    InstanceHandle collapsed = 0;
    hr = vts->CreateInstance(typeName, valueStr, &collapsed);
    ::SysFreeString(typeName);
    ::SysFreeString(valueStr);
    if (FAILED(hr) || !collapsed) {
        tapLogf("xaml: CreateInstance(Visibility,1) 失败 0x%08X", (unsigned)hr);
        return hr;
    }

    hr = vts->SetProperty(target, collapsed, visIndex);
    tapLogf("xaml: SetProperty(Visibility=Collapsed) -> 0x%08X", (unsigned)hr);
    return hr;
}

// 还原成 Visible
HRESULT setElementVisible(InstanceHandle target) {
    if (!g_diag) return (HRESULT)E_FAIL;

    winrt::com_ptr<IUnknown> unk;
    HRESULT hrQi = g_diag->QueryInterface(__uuidof(IVisualTreeService3), unk.put_void());
    if (FAILED(hrQi) || !unk) {
        hrQi = g_diag->QueryInterface(__uuidof(IVisualTreeService2), unk.put_void());
    }
    if (FAILED(hrQi) || !unk) return hrQi;

    auto vts  = unk.try_as<IVisualTreeService>();
    auto vts2 = unk.try_as<IVisualTreeService2>();
    if (!vts || !vts2) return (HRESULT)E_FAIL;

    unsigned int visIndex = 0;
    HRESULT hr = vts2->GetPropertyIndex(
        target, L"Windows.UI.Xaml.UIElement.Visibility", &visIndex);
    if (FAILED(hr)) return hr;

    BSTR typeName = ::SysAllocString(L"Windows.UI.Xaml.Visibility");
    BSTR valueStr = ::SysAllocString(L"0");
    InstanceHandle vis = 0;
    hr = vts->CreateInstance(typeName, valueStr, &vis);
    ::SysFreeString(typeName);
    ::SysFreeString(valueStr);
    if (FAILED(hr) || !vis) return hr;

    hr = vts->SetProperty(target, vis, visIndex);
    tapLogf("xaml: SetProperty(Visibility=Visible) -> 0x%08X", (unsigned)hr);
    return hr;
}

// ═══════════════════════════════════════════════════════════════════
// 可行性验证：往任务栏的 XAML 树里插入一个元素
//
// 岛台要"长在系统任务栏上"，就得走这条路：
//   CreateInstance 造控件 → 设属性 → AddChild 挂进 TaskbarFrame
//
// 这是没人验证过的操作（改属性和插入元素是两回事），
// 所以先做一个最小版本：
//   插一个 200x40 的红色矩形到任务栏中间
// 成不成，截图一眼就能看出来。
//
// ⚠ 风险提示：
//   往 explorer 的 XAML 树里塞元素，参数不对有可能让任务栏崩。
//   所以这里每一步都检查返回值、失败立刻停，不留半个元素在树里。
// ═══════════════════════════════════════════════════════════════════

// 我们自己挂进任务栏的元素（移除时要用）。
//
// ⚠ 这里是**真的 C++/WinRT 对象**，不是 InstanceHandle。
//   因为我们走的是"进程内直接调 XAML API"这条路，
//   不是 XAML 诊断那条（它造不出控件）。
winrt::Windows::UI::Xaml::UIElement g_ourElement{nullptr};
winrt::Windows::UI::Xaml::Controls::Panel g_ourParentPanel{nullptr};

// 见过的所有元素句柄 + 类型名。
//
// 用途：找容器。TaskbarFrame 实测转不成任何公开 Panel，
// 但树里确实有 Windows.UI.Xaml.Controls.Grid / StackPanel 这些标准容器 ——
// 得遍历才知道哪个能把子元素挂进去。
//
// 只在需要时填充，避免平时占内存。
std::vector<std::pair<InstanceHandle, std::wstring>> g_seenElements;

// ── 岛台按钮（真正插进任务栏的那个控件）────────────────────────────
//
// 这个不是测试方块了，是要长期留在任务栏上的东西：
//   · 一个圆形/圆角底的 Button
//   · 里面一个图标字形
//   · 点击时通知主程序开面板
//
// ⚠ 点击处理用 XAML 的 Tapped 事件，不走我们自己的窗口。
//   这样点击区域完全跟随元素 —— 任务栏移动/缩放时不用手动对齐。
winrt::Windows::UI::Xaml::Controls::Button g_islandButton{nullptr};
winrt::Windows::UI::Xaml::Controls::Panel g_islandHost{nullptr};

// 岛台按钮被点过的标志。
//
// 按钮的 Tapped 回调跑在 explorer 的 UI 线程上，
// 不能在那里做任何耗时操作（会卡住任务栏）。
// 所以回调只置这个标志，主程序通过命令通道轮询来取。
volatile LONG g_islandTapped = 0;

// "任务栏布局变了"的标志（图标增减、重排）。
//
// 图标多一个少一个时，任务栏会 Add/Remove 一个 TaskListButton ——
// OnVisualTreeChange 回调立刻能收到。
// 这里置位，主程序下次轮询时取走，然后重新读坑位、挪岛台按钮。
//
// ⚠ 为什么不在回调里直接通知主程序：
//   通道是"主程序问、DLL 答"的单向模型。
//   DLL 主动推消息要另加一套反向通知机制，复杂度不值。
//   150ms 的轮询延迟对这个场景完全够用。
volatile LONG g_layoutChanged = 0;

// 探测用：插进任务栏容器的那个图标。
// 留着句柄才能后续查它的实际坐标。
winrt::Windows::UI::Xaml::UIElement g_probeIcon{nullptr};

// 任务栏图标组的左边缘（由 TaskListButton 回调更新）。
//
// ⚠ 为什么不用"开始按钮的坑位"：
//   那个位置**恒定不变**（开始按钮在 XAML 里占固定一格），
//   但图标组是**居中**布局，图标一多一少就会左右移动。
//   用恒定值去定位，自然会和移动的图标组重叠。
volatile LONG g_firstIconValid = 0;
double g_firstIconX = 0.0;
double g_firstIconY = 0.0;
double g_firstIconW = 0.0;
double g_firstIconH = 0.0;

HRESULT probeInjectElement(bool insert) {
    if (!g_diag) return (HRESULT)E_FAIL;

    // ── 移除分支 ──
    if (!insert) {
        try {
            if (g_islandHost && g_islandButton) {
                auto children = g_islandHost.Children();
                uint32_t idx = 0;
                if (children.IndexOf(g_islandButton, idx)) {
                    children.RemoveAt(idx);
                    tapLogf("xaml: 岛台按钮已从任务栏移除（下标 %u）", idx);
                }
            }
            g_islandButton = nullptr;
            // 注意：不清 g_islandHost，它本来就是系统的 Grid
            return S_OK;
        } catch (const winrt::hresult_error& ex) {
            tapLogf("xaml: 移除岛台按钮异常 0x%08X", (unsigned)ex.code().value);
            return ex.code().value;
        }
    }

    // ── 插入分支：创建真正的岛台按钮 ──

    // 已经插过就不重复插（幂等）
    if (g_islandButton) {
        tapLogf("xaml: 岛台按钮已存在，跳过");
        return S_OK;
    }

    // 1) 找宿主容器 —— 用**开始按钮所在的那个容器**。
    //
    // ⚠ 这里是关键。之前遍历全树找"第一个能转成 Grid 的节点"，
    //   那个 Grid 和开始按钮没关系，按钮插进去位置是飘的
    //   （实测落在任务栏最左端，和图标组隔着一大段空白）。
    //
    //   正确做法是回调里记下的 islandHostPanel / islandHostIndex ——
    //   那是开始按钮的父容器和它在容器里的下标。
    //   插到同一个位置，图标组会自动往右顺移，
    //   不需要我们算任何像素坐标，任务栏自己会排版。
    winrt::Windows::UI::Xaml::Controls::Panel host{nullptr};
    unsigned int insertIndex = 0;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& [h, e] : g_taskbars) {
            if (e.islandHostValid && e.islandHostPanel) {
                host = e.islandHostPanel;
                insertIndex = e.islandHostIndex;
                tapLogf("xaml: 岛台宿主 = 开始按钮的容器，插入下标 %u", insertIndex);
                break;
            }
        }
    }

    if (!host) {
        tapLogf("xaml: 还没抓到开始按钮的容器（补抓再来一轮）");
        return (HRESULT)E_FAIL;
    }

    try {
        namespace wux = winrt::Windows::UI::Xaml;

        // 2) 造按钮。
        //
        // 用 Button 而不是 Rectangle：Button 自带 hover/pressed 视觉状态、
        // 自带命中测试和 Tapped 事件 —— 自己用 Rectangle 实现这些要写一堆代码，
        // 而且不一定有系统那种手感。
        wux::Controls::Button btn;

        // 尺寸：和任务栏图标区一格差不多大
        btn.Width(40);
        btn.Height(32);
        btn.Padding(wux::Thickness{0, 0, 0, 0});
        btn.BorderThickness(wux::Thickness{0, 0, 0, 0});
        btn.CornerRadius(wux::CornerRadius{6, 6, 6, 6});

        // ⚠ 不要再设 HorizontalAlignment / Margin 去"手工定位"。
        //
        //   我们现在插在开始按钮原来的坑里，容器的布局系统
        //   会把它排到正确位置、并让后面的图标自动顺移。
        //
        //   手工设 Right 会让它贴到容器最右边（实测跑到设置图标附近），
        //   设 Left + Margin 会让它压在最左端 —— 都是错的，
        //   因为那是在跟容器的排版打架。
        btn.VerticalAlignment(wux::VerticalAlignment::Center);

        // 背景：透明 —— 让它看起来像任务栏自带的一个图标按钮，
        // 而不是贴上去的一块色块。
        //
        // 悬停/按下时的反馈交给 Button 自己的 VisualState
        // （模板里那层 Background 会在 PointerOver 时显示出来）。
        btn.Background(wux::Media::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(0x00, 0xFF, 0xFF, 0xFF)));
        btn.BorderBrush(wux::Media::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(0x00, 0xFF, 0xFF, 0xFF)));

        // 3) 内容：一个图标字形。
        //
        // 用 Segoe MDL2 Assets 的 \uE71D（应用网格），
        // 和岛台面板里那个"全部"图标一致。
        wux::Controls::FontIcon icon;
        icon.FontFamily(wux::Media::FontFamily(L"Segoe MDL2 Assets"));
        icon.Glyph(L"\uE71D");
        icon.FontSize(16);
        // ⚠ 颜色从 hr/island/island.h 的常量来 —— 和岛台面板文字同一个值。
        //   两处各写各的话，改了面板忘了按钮就会出现色差。
        icon.Foreground(wux::Media::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(
                (kIslandTextMain >> 24) & 0xFF,
                (kIslandTextMain >> 16) & 0xFF,
                (kIslandTextMain >> 8) & 0xFF,
                kIslandTextMain & 0xFF)));
        btn.Content(icon);

        // 4) 点击事件。
        //
        // ⚠ 这是关键：lambda 注册进 explorer 的 XAML 事件系统，
        //   点击时会在 explorer 的 UI 线程上被调用。
        //
        //   回调里不能直接做耗时操作（会卡住任务栏），
        //   只把"岛台被点了"这个事实写进一个标志位，
        //   由主程序通过命令通道来取。
        btn.Tapped([&](const winrt::Windows::Foundation::IInspectable&,
                       const wux::Input::TappedRoutedEventArgs&) {
            ::InterlockedExchange(&g_islandTapped, 1);
            tapLogf("xaml: 岛台按钮被点击（Tapped 事件）");
        });

        // 鼠标悬停时给个视觉反馈（Button 自带 VisualState，这里补个指针光标）
        btn.PointerEntered([&](const winrt::Windows::Foundation::IInspectable&,
                               const wux::Input::PointerRoutedEventArgs&) {
            try {
                winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread()
                    .PointerCursor(winrt::Windows::UI::Core::CoreCursor(
                        winrt::Windows::UI::Core::CoreCursorType::Hand, 0));
            } catch (...) {
                // 拿不到 CoreWindow 就算了，不影响功能
            }
        });

        // 5) 插到开始按钮原来的位置上。
        //
        // ⚠ 用 InsertAt(insertIndex) 而不是 Append。
        //
        //   Append 是加到末尾，会排到所有图标后面；
        //   插到开始按钮的下标上，才是"顶替它占的那一格" ——
        //   图标组会自动往右顺移，正是用户要的效果。
        //
        //   下标可能超出当前范围（树是渐进的），
        //   所以夹一下，夹不住就退到 Append。
        // 5a) 先把布局属性设成和开始按钮一样。
        //
        // ⚠ 这一步不能省。
        //   实测：只插入不设属性时，按钮坐标是 (-10000,-10016) ——
        //   XAML 把它当成“没参与布局”，丢到了屏幕外。
        //   它尺寸算得出来 (40x32)，但就是不在屏幕上。
        //
        //   盖上开始按钮的 Grid 位置，它就落回原来那个格子。
        unsigned int insertIndex2 = insertIndex;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto& [hh, ee] : g_taskbars) {
                if (ee.islandHostValid) {
                    insertIndex2 = ee.islandHostIndex;
                    try {
                        wux::Controls::Grid::SetColumn(btn, ee.islandStartCol);
                        wux::Controls::Grid::SetRow(btn, ee.islandStartRow);
                        tapLogf("xaml: 岛台按钮布局已设为 Grid(%d,%d)",
                                ee.islandStartCol, ee.islandStartRow);
                    } catch (const winrt::hresult_error& ex) {
                        tapLogf("xaml: 设 Grid 布局属性失败 0x%08X",
                                (unsigned)ex.code().value);
                    }
                    break;
                }
            }
        }
        insertIndex = insertIndex2;

        auto children = host.Children();
        const unsigned int n = children.Size();

        // 诊断：把容器里现有子元素的类型列出来。
        //
        // 这是判断"我找的容器对不对"的唯一可靠办法：
        //   · 如果列出来的是 Taskbar.TaskListButton 之类 → 这就是图标组 ✓
        //   · 如果只有个 Button / Border → 抓错容器了，得再往上找
        {
            wchar_t line[1024] = {0};
            int off = 0;
            for (unsigned int i = 0; i < n && i < 12; ++i) {
                wchar_t tn[256] = {0};
                try {
                    ::GetClassNameW(nullptr, tn, 0);
                    winrt::hstring name = winrt::get_class_name(children.GetAt(i));
                    wcsncpy_s(tn, name.c_str(), _TRUNCATE);
                } catch (...) {
                    wcscpy_s(tn, L"?");
                }
                off += swprintf_s(line + off, 1024 - off, L"%s|", tn);
                if (off >= 1000) break;
            }
            tapLogw(L"xaml: 容器(%u 个子元素)内容: %s", n, line);
        }

        if (insertIndex <= n) {
            children.InsertAt(insertIndex, btn);
            tapLogf("xaml: ✅ 岛台按钮已插入下标 %u（容器现有 %u 个子元素）",
                    insertIndex, children.Size());
        } else {
            children.Append(btn);
            tapLogf("xaml: 下标 %u 超出范围(%u)，改为追加到末尾",
                    insertIndex, n);
        }

        // ⚠ ActualWidth 要等布局跑完才有值。
        //   插入后立刻读一定是 0，不说明问题。
        //   所以交给一个定时器，400ms 之后再报。
        if (g_xamlThreadWnd) ::SetTimer(g_xamlThreadWnd, 0x51, 400, nullptr);

        g_islandButton = btn;
        g_islandHost = host;
        tapLogf("xaml: ✅ 岛台按钮已插入任务栏（宿主子元素数 = %u）",
                host.Children().Size());
        return S_OK;

    } catch (const winrt::hresult_error& ex) {
        tapLogf("xaml: 创建岛台按钮异常 0x%08X", (unsigned)ex.code().value);
        return ex.code().value;
    } catch (const std::exception& ex) {
        tapLogf("xaml: 创建岛台按钮 std 异常 %s", ex.what());
        return (HRESULT)E_FAIL;
    } catch (...) {
        tapLogf("xaml: 创建岛台按钮未知异常");
        return (HRESULT)E_FAIL;
    }
}

HRESULT restoreElementOpacity(InstanceHandle target) {
    if (!g_diag) return (HRESULT)E_FAIL;

    winrt::com_ptr<IUnknown> unk;
    if (FAILED(g_diag->QueryInterface(__uuidof(IVisualTreeService3), unk.put_void())) || !unk) {
        return (HRESULT)E_FAIL;
    }
    auto vts  = unk.try_as<IVisualTreeService>();
    auto vts2 = unk.try_as<IVisualTreeService2>();
    if (!vts || !vts2) return (HRESULT)E_FAIL;

    // 还原就是把 Opacity 放回默认值 1（ClearProperty 亦可，
    // 但显式写 1 更可预测）。
    unsigned int opIndex = 0;
    HRESULT hr = vts2->GetPropertyIndex(
        target, L"Windows.UI.Xaml.UIElement.Opacity", &opIndex);
    if (FAILED(hr)) return hr;

    BSTR typeName = ::SysAllocString(L"Windows.Foundation.Double");
    BSTR valueStr = ::SysAllocString(L"1");
    InstanceHandle one = 0;
    hr = vts->CreateInstance(typeName, valueStr, &one);
    ::SysFreeString(typeName);
    ::SysFreeString(valueStr);
    if (FAILED(hr) || !one) return hr;

    hr = vts->SetProperty(target, one, opIndex);
    tapLogf("xaml: SetProperty(Opacity=1) -> 0x%08X", (unsigned)hr);
    return hr;
}

// 前向声明：命令处理与窗口过程要用它们，定义在文件后段。
HRESULT applyAppearance(HWND taskbar, GlassBrush brush, UINT color);
HRESULT returnToDefault(HWND taskbar, bool restoreTransparency = true, bool restoreIslandButtons = true);
int knownTaskbarCount();
HRESULT setElementCollapsed(InstanceHandle target);
HRESULT setElementVisible(InstanceHandle target);
HRESULT probeInjectElement(bool insert);

// 岛台接管模式：是否隐藏任务栏的开始/搜索按钮。
// 由主程序通过命令通道设置。
std::atomic<bool> g_islandTakeover{false};

// 岛台专用标志：本次 scheduleApply 是岛台触发的，
// applyAppearance 看到它就只隐藏/还原按钮，不碰背景/边框。
//
// ⚠ 为什么不用 GlassBrush 枚举加一个 "NoChange" 值：
//   那要改接口签名和所有调用方，改动太大。
//   一个原子布尔就够了 —— 只在两处读写。
std::atomic<bool> g_islandOnlyApply{false};

// 当前**期望**的透明状态。
//
// ⚠ 为什么需要它：
//   回调发现 BackgroundFill/BackgroundStroke 时会"重放当前外观"。
//   原来重放的是硬编码的 Transparent —— 不管用户想不想要透明。
//   结果启动时还原透明化的操作会被后到的重放覆盖（状态 D 变透明）。
//
//   现在重放的是这个标志记录的状态：
//     true  → 重放透明化
//     false → 不重放（保持系统默认 = 不透明）
std::atomic<bool> g_transparencyWanted{false};

// 已发现的「开始/搜索」按钮元素（全局，去重）。
//
// ⚠ 为什么不用每个任务栏条目里的 islandHidden：
//   回调里往 islandHidden 推元素的前提是 g_taskbars 里已有该任务栏条目，
//   而条目和按钮的发现顺序不固定 —— 按钮先到就被跳过，
//   且发现事件只发一次，之后永远不补（状态 B 开始/搜索没隐藏的根因）。
//
//   全局列表不依赖任何顺序：发现就记，应用时统一隐藏。
std::vector<wux::UIElement> g_islandButtons;

// 岛台按钮在屏幕上的位置（由 QueryIslandButtonRect 更新）。
// 主程序需要它来对齐面板、以及放置点击热区。
int g_islandBtnX = 0;
int g_islandBtnY = 0;
int g_islandBtnW = 0;
int g_islandBtnH = 0;

// 前向声明：命令处理要用它们，定义在文件后段。
void scheduleApply(GlassBrush brush, UINT color);
bool resubscribeVisualTree();

// 待应用的外观（由回调设置，由延迟任务消费）
std::mutex g_pendingMu;
bool g_pendingValid = false;
GlassBrush g_pendingBrush = GlassBrush::Transparent;
UINT g_pendingColor = 0;

// 自定义消息：投递到 XAML 线程，在回调栈之外执行实际修改
const UINT WM_GLASSTAP_APPLY = WM_APP + 0x721;

// 定时器：周期性检查主程序是否下达了命令。
//
// 主程序与 explorer 内的 DLL 之间用共享内存 + 事件通信
// （见 channel.h —— 自定义 COM 接口跨进程需要代理桩，代价太大）。
// 这里在 XAML 线程上轮询，保证命令在正确的线程上执行。
const UINT_PTR kChannelTimerId = 0x7211;
const UINT kChannelPollMs = 120;

// 岛台接管开启后补抓一轮的定时器。
//
// 重新订阅视觉树会让系统分多次把整棵树的通知投递过来，
// 任务栏元素多，一轮不一定到齐。这里隔一会儿再应用一次，
// 把晚到的「开始」「搜索」按钮也隐藏掉。
// 应用本身是幂等的，重复执行无害。
const UINT_PTR kIslandCatchupTimer = 0x7212;

// 应用失败的自动重试计数。任务栏启动阶段 XAML 树还在构建，
// 属性可能处于内部锁定状态，稍后重试即可成功。
int g_applyRetries = 0;

// 探针开关：打开后会把见到的所有元素类型打到日志里，
// 用来摸清"托盘区的时间/日期/WLAN 到底叫什么类型"。
// 摸清结构后关掉，避免日志噪音。
bool g_probeTrayElements = true;

// 已记录过的类型名，避免同一个类型重复打几十次
std::set<std::wstring> g_seenTypes;

// 探针：把某个元素的 Foreground 相关属性读出来打进日志。
//
// 目的是确认三件事：
//   1. 属性名能不能被 GetPropertyIndex 解析
//   2. 当前值句柄是否有效（有效说明确实挂了画刷）
//   3. 读得到才谈得上改
//
// 之前 Shape.Fill 的教训是"读得到、写不进"，所以必须分开验证读与写。
void probeForegroundOf(InstanceHandle handle, const wchar_t* type, const wchar_t* name) {
    if (!g_diag) return;

    winrt::com_ptr<IUnknown> unk;
    if (FAILED(g_diag->QueryInterface(__uuidof(IVisualTreeService3), unk.put_void())) || !unk) {
        return;
    }
    auto vts2 = unk.try_as<IVisualTreeService2>();
    if (!vts2) return;

    // 依次试几种属性名写法，看哪个能被解析。
    // XAML 诊断要求"声明类型.属性名"的完整形式。
    const wchar_t* candidates[] = {
        L"Windows.UI.Xaml.Controls.TextBlock.Foreground",
        L"Windows.UI.Xaml.Controls.Control.Foreground",
        L"SystemTray.AdaptiveTextBlock.Foreground",
        L"Windows.UI.Xaml.Controls.FontIcon.Foreground",
    };

    for (const wchar_t* prop : candidates) {
        unsigned int idx = 0;
        HRESULT hr = vts2->GetPropertyIndex(handle, prop, &idx);
        if (SUCCEEDED(hr)) {
            InstanceHandle cur = 0;
            HRESULT hrGet = vts2->GetProperty(handle, idx, &cur);
            tapLogw(L"probe: %s[%s] 属性 %s -> idx=%u 读值 0x%08X handle=%llu",
                    type, name && *name ? name : L"(无名)", prop,
                    idx, (unsigned)hrGet, (unsigned long long)cur);
            return;   // 找到一种能用的就够了
        }
    }

    tapLogw(L"probe: %s[%s] 所有 Foreground 写法都无法解析",
            type, name && *name ? name : L"(无名)");
}

// 已识别到的托盘文字元素（时间/日期/WLAN/输入法）。
//
// 实测它们的 Foreground 都指向同一个画刷对象（句柄完全相同），
// 说明是共享的主题资源。
//
// 记录它们是为了：任务栏透明后按背景明暗自动切深浅色，
// 否则浅色壁纸上白字会看不清。
std::vector<InstanceHandle> g_trayTextElements;

// 把托盘文字元素的颜色改成深色或浅色。
//
// ⚠ 与 Opacity 不同，Foreground 是 Brush 类型：
//   要用 CreateInstance 造一个 SolidColorBrush，再把它的 Color 设好，
//   最后 SetProperty 到目标元素的 Foreground。
//
//   之前测过 Shape.Fill 是"读得到、写不进"。Foreground 是否可写
//   必须单独验证，不能想当然。
HRESULT setTrayTextColor(bool dark) {
    if (!g_diag) return (HRESULT)E_FAIL;
    if (g_trayTextElements.empty()) {
        tapLogf("xaml: 还没记录到托盘文字元素，无法变色");
        return (HRESULT)E_FAIL;
    }

    winrt::com_ptr<IUnknown> unk;
    if (FAILED(g_diag->QueryInterface(__uuidof(IVisualTreeService3), unk.put_void())) || !unk) {
        return (HRESULT)E_FAIL;
    }
    auto vts  = unk.try_as<IVisualTreeService>();
    auto vts2 = unk.try_as<IVisualTreeService2>();
    if (!vts || !vts2) return (HRESULT)E_FAIL;

    // 深色文字用近黑，浅色用近白。
    // 不用纯黑纯白：图标本身也不是极端色，太纯会显得突兀。
    const wchar_t* colorStr = dark ? L"#FF1A1A1A" : L"#FFFFFFFF";

    // 造一个 SolidColorBrush
    BSTR typeBstr = ::SysAllocString(L"Windows.UI.Xaml.Media.SolidColorBrush");
    BSTR valBstr  = ::SysAllocString(colorStr);
    InstanceHandle brush = 0;
    HRESULT hr = vts->CreateInstance(typeBstr, valBstr, &brush);
    ::SysFreeString(typeBstr);
    ::SysFreeString(valBstr);

    tapLogw(L"xaml: 造文字画刷(%s) -> 0x%08X handle=%llu",
            colorStr, (unsigned)hr, (unsigned long long)brush);
    if (FAILED(hr) || !brush) return hr;

    int okCount = 0, failCount = 0;
    int firstFailIdx = -1;
    HRESULT firstFailHr = S_OK;
    HRESULT firstReadBack = S_OK;
    unsigned long long firstHandle = 0;

    for (size_t i = 0; i < g_trayTextElements.size(); ++i) {
        InstanceHandle handle = g_trayTextElements[i];
        unsigned int idx = 0;
        HRESULT hrIdx = vts2->GetPropertyIndex(
            handle, L"Windows.UI.Xaml.Controls.TextBlock.Foreground", &idx);
        if (FAILED(hrIdx)) {
            // 输入法那条属于 Control，属性名不同
            hrIdx = vts2->GetPropertyIndex(
                handle, L"Windows.UI.Xaml.Controls.Control.Foreground", &idx);
        }
        if (FAILED(hrIdx)) {
            ++failCount;
            if (firstFailIdx < 0) { firstFailIdx = -2; firstFailHr = hrIdx; }
            continue;
        }

        HRESULT hrSet = vts->SetProperty(handle, brush, idx);
        if (SUCCEEDED(hrSet)) {
            ++okCount;

            // 写完之后立刻读回来，确认值到底变成什么了。
            //
            // 这一步很关键：SetProperty 返回 S_OK **不代表**值真的变成了
            // 我们写的那个 —— 可能被更高优先级的样式或主题绑定覆盖。
            // 读回来的句柄若等于我们刚写的画刷句柄，才算真的写进去了。
            if (i == 0) {
                InstanceHandle back = 0;
                firstReadBack = vts2->GetProperty(handle, idx, &back);
                firstHandle = (unsigned long long)back;
            }
        } else {
            ++failCount;
            if (firstFailIdx < 0) { firstFailIdx = (int)idx; firstFailHr = hrSet; }
        }
    }

    tapLogf("xaml: 文字变色完成(%s) 成功=%d 失败=%d | 读回=0x%08X 现值=%llu 目标=%llu",
            dark ? "dark" : "light", okCount, failCount,
            (unsigned)firstReadBack, firstHandle, (unsigned long long)brush);

    return okCount > 0 ? (HRESULT)S_OK : (HRESULT)E_FAIL;
}


// 采样任务栏区域背后的壁纸亮度，判断该用深色还是浅色文字。
//
// 做法：截取任务栏右侧托盘区正上方的一小条（那里没有我们的元素），
// 算平均亮度。亮 → 用深色文字；暗 → 用浅色文字。
//
// ⚠ 采 "上方" 而不是任务栏本身：任务栏已经被我们透明化了，
//   采它等于采壁纸 —— 但更靠上一点能避开图标干扰。
bool shouldUseDarkText() {
    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;

    RECT r{};
    if (!::GetWindowRect(tray, &r)) return false;
    const int h = r.bottom - r.top;
    if (h <= 0) return false;

    // 取托盘区正上方的一条（高度 = 任务栏高度的一半，宽度取右侧 500px）
    const int sampleW = 500;
    const int sampleH = h / 2;
    const int x = r.right - sampleW - 10;
    const int y = r.top - sampleH - 4;
    if (x < 0 || y < 0) return false;

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, sampleW, sampleH);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, sampleW, sampleH, screen, x, y, SRCCOPY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sampleW;
    bi.bmiHeader.biHeight = -sampleH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> px((size_t)sampleW * sampleH * 4);
    ::GetDIBits(mem, bmp, 0, sampleH, px.data(), &bi, DIB_RGB_COLORS);

    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    // 用感知亮度而不是单纯 R 通道：人眼对绿最敏感
    double sum = 0;
    int n = 0;
    for (int i = 0; i < sampleW * sampleH; ++i) {
        const double b = px[(size_t)i * 4 + 0];
        const double g = px[(size_t)i * 4 + 1];
        const double rr = px[(size_t)i * 4 + 2];
        sum += 0.2126 * rr + 0.7152 * g + 0.0722 * b;
        ++n;
    }
    const double lum = n ? sum / n : 128.0;

    // 阈值取 140：偏保守，浅色壁纸判定为"需要深色文字"。
    // 用 0-255 的中点 128 也行，但实测浅色壁纸（如白色桌面）
    // 亮度常在 200+，而深色壁纸在 40-80，中间地带少见，
    // 阈值不敏感，取 140 即可。
    const bool dark = lum > 140.0;
    tapLogf("xaml: 背景亮度=%.1f -> 用%s文字", lum, dark ? "深色" : "浅色");
    return dark;
}

// 检测背景亮度并自动切换文字颜色
HRESULT autoAdjustTextColor() {
    return setTrayTextColor(shouldUseDarkText());
}

// ★ 记住最近一次的文字颜色策略。
//
//   用户反馈："任务栏右边文字颜色有时候跟设置里的不一致"。
//   原来每次重新应用透明都固定走 autoAdjustTextColor()（按壁纸亮度自动），
//   于是只要任务栏重新应用一次透明（重挂、壁纸变化…），
//   用户在设置里手动选的深/浅色就被自动值盖掉了。
//   现在重放时按记住的策略走 —— auto 才去按亮度算。
enum class TrayTextMode { Auto, Dark, Light };
TrayTextMode g_trayTextMode = TrayTextMode::Auto;

HRESULT applyTrayTextMode() {
    switch (g_trayTextMode) {
        case TrayTextMode::Dark:  return setTrayTextColor(true);
        case TrayTextMode::Light: return setTrayTextColor(false);
        default:                  return autoAdjustTextColor();
    }
}

// 处理一条来自主程序的命令
void handleChannelCommand(HWND h) {
    ChannelCmd cmd = ChannelCmd::None;
    unsigned arg = 0;
    if (!channelPoll(&cmd, &arg)) return;

    HRESULT hr = S_OK;
    switch (cmd) {
        case ChannelCmd::ApplyTransparent:
            hr = applyAppearance(nullptr, GlassBrush::Transparent, arg);
            tapLogf("channel: 收到「应用透明」-> 0x%08X", (unsigned)hr);

            // 透明生效后立刻按背景明暗调整文字颜色。
            //
            // 这是用户要的行为：点"透明"时自动处理文字可见性 ——
            // 背景变成浅色壁纸时把白字改成黑字，否则会看不清。
            //
            // 放在这里而不是 applyAppearance 内部，是因为那个函数
            // 在视觉树回调路径上也会被调用，而此刻读取屏幕像素
            // 需要一次完整的重绘才算数。
            if (SUCCEEDED(hr)) {
                // 等一帧，让透明先真正呈现出来，否则采样到的是旧画面
                ::Sleep(120);
                // ★ 用记住的策略，不是无条件自动 —— 见 applyTrayTextMode 的说明
                HRESULT hrColor = applyTrayTextMode();
                tapLogf("channel: 应用透明后自动调整文字 -> 0x%08X", (unsigned)hrColor);
            }
            break;

        case ChannelCmd::RestoreDefault:
            // ★ 全量还原：透明化 + 岛台按钮都还原。
            //
            //   这个命令的语义是"恢复到默认状态" ——
            //   用在启动时清理残留、程序退出时清理。
            //   必须还原一切，否则上一次运行留下的 Collapsed 按钮
            //   永远不会恢复（DLL 残留在 explorer 里）。
            //
            //   "只还原透明化不影响岛台"的需求由
            //   RestoreTransparencyOnly 命令处理。
            hr = returnToDefault(nullptr, /*restoreTransparency=*/true,
                                 /*restoreIslandButtons=*/true);
            tapLogf("channel: 收到「还原默认」-> 0x%08X", (unsigned)hr);

            // 还原时把文字颜色也交还给系统。
            //
            // 做法是写回系统默认的浅色（任务栏不透明时是深色背景，
            // 文字本来就是白的）。不这么做的话，浅色壁纸下标成
            // 黑字的托盘文字会留在深色任务栏上，同样看不清。
            if (SUCCEEDED(hr)) {
                HRESULT hrColor = setTrayTextColor(false);
                tapLogf("channel: 还原文字颜色 -> 0x%08X", (unsigned)hrColor);
            }
            break;

        case ChannelCmd::QueryState: {
            int n = knownTaskbarCount();
            tapLogf("channel: 收到「查询状态」-> %d 个任务栏", n);
            // 状态通过 result 回传（用任务栏数量作为值）
            channelReply((HRESULT)n);
            return;
        }

        case ChannelCmd::SetTextDark:
            g_trayTextMode = TrayTextMode::Dark;
            hr = setTrayTextColor(true);
            tapLogf("channel: 收到「文字改深色」-> 0x%08X", (unsigned)hr);
            break;

        case ChannelCmd::SetTextLight:
            g_trayTextMode = TrayTextMode::Light;
            hr = setTrayTextColor(false);
            tapLogf("channel: 收到「文字改浅色」-> 0x%08X", (unsigned)hr);
            break;

        case ChannelCmd::AutoTextColor:
            g_trayTextMode = TrayTextMode::Auto;
            hr = autoAdjustTextColor();
            tapLogf("channel: 收到「文字颜色自动」-> 0x%08X", (unsigned)hr);
            break;

        case ChannelCmd::ProbeInjectElement:
            // 往任务栏插/删岛台按钮
            hr = probeInjectElement(arg != 0);
            tapLogf("channel: 收到「插入岛台按钮」=%u -> 0x%08X", arg, (unsigned)hr);
            break;

        case ChannelCmd::ConsumeIslandTap: {
            // 取走"岛台按钮被点击"的标志，取完清零。
            //
            // 这是跨进程传递一次点击事件的办法：
            // 按钮的 Tapped 回调在 explorer 里置位，
            // 主程序轮询把它取走。
            //
            // 返回值里带 1/0 表示这次有没有取到。
            LONG was = ::InterlockedExchange(&g_islandTapped, 0);
            channelReply((HRESULT)(was ? 1 : 0));
            return;
        }

        case ChannelCmd::QueryIslandButtonRect: {
            // 查按钮在屏幕上的实际位置。
            //
            // 用 XAML 的 TransformToVisual 把元素坐标转成屏幕坐标 ——
            // 不用猜，拿到的是它真实渲染的位置。
            // 主程序用它来决定面板弹在哪、以及在哪里盖点击热区。
            int mw = 0, mh = 0;
            if (g_islandButton) {
                try {
                    auto pt = g_islandButton
                        .TransformToVisual(nullptr)
                        .TransformPoint(winrt::Windows::Foundation::Point{0, 0});
                    mw = (int)pt.X;      // 复用这两个变量传 x/y
                    mh = (int)pt.Y;
                    const int aw = (int)g_islandButton.ActualWidth();
                    const int ah = (int)g_islandButton.ActualHeight();
                    g_islandBtnX = mw;
                    g_islandBtnY = mh;
                    g_islandBtnW = aw;
                    g_islandBtnH = ah;
                    tapLogf("xaml: 岛台按钮屏幕位置 (%d,%d) 尺寸 %dx%d",
                            mw, mh, aw, ah);
                } catch (const winrt::hresult_error& ex) {
                    tapLogf("xaml: 取按钮位置异常 0x%08X", (unsigned)ex.code().value);
                } catch (...) {
                    tapLogf("xaml: 取按钮位置未知异常");
                }
            } else {
                tapLogf("xaml: 按钮不存在，无法取位置");
            }
            channelReply((HRESULT)((g_islandBtnW << 16) | (g_islandBtnH & 0xFFFF)));
            return;
        }

        case ChannelCmd::QueryStartSlot: {
            // 把开始按钮原来占的那个坑报回去（屏幕坐标，物理像素）。
            //
            // 为什么要这个：
            //   ItemsRepeater 不接受外部插入的子元素（实测丢到 -10000），
            //   所以岛台按钮不能真的插进图标组里。
            //   改成“把开始按钮原来的位置读出来，
            //   在那个位置放我们自己的窗口”。
            //
            // 因为位置是从开始按钮实测的，视觉上天然严丝合缝。
            int px_ = -1, py_ = -1, pw_ = 0, ph_ = 0;
            for (auto& [h, e] : g_taskbars) {
                if (e.islandStartPosValid) {

                    // ★ 每次都**实时读**，不用缓存的值。
                    //
                    //   缓存只在"读到的那一刻"是对的。
                    //   任务栏图标是居中排列的 —— 图标一多一少，
                    //   整个组的起点就移动了，缓存值立刻失效。
                    //
                    //   实测确认：Collapsed 之后元素的布局信息仍然有效，
                    //   TransformToVisual 照样返回正确坐标，
                    //   所以可以反复读。
                    double liveX = e.islandStartX;
                    double liveY = e.islandStartY;
                    double liveW = e.islandStartW;
                    double liveH = e.islandStartH;
                    if (e.islandStartHandle) {
                        if (auto el = fromHandle<wux::FrameworkElement>(e.islandStartHandle)) {
                            try {
                                const double aw = el.ActualWidth();
                                const double ah = el.ActualHeight();
                                auto pt = el.TransformToVisual(nullptr)
                                    .TransformPoint(winrt::Windows::Foundation::Point{0, 0});
                                if (aw > 1.0 && ah > 1.0) {
                                    liveX = pt.X;
                                    liveY = pt.Y;
                                    liveW = aw;
                                    liveH = ah;
                                    // 顺手更新缓存，下次万一读不到还有个值
                                    e.islandStartX = liveX;
                                    e.islandStartY = liveY;
                                    e.islandStartW = liveW;
                                    e.islandStartH = liveH;
                                }
                            } catch (...) {
                                // 读不到就用缓存值
                            }
                        }
                    }
                    // XAML 用 DIP，要乘 DPI 缩放才是物理像素
                    // 直接问系统要窗口的 DPI 缩放，不自己算
                    UINT dpi = 96;
                    if (e.window) dpi = ::GetDpiForWindow(e.window);
                    if (dpi == 0) dpi = 96;
                    const double sc = (double)dpi / 96.0;
                    RECT tb{};
                    if (e.window && ::GetWindowRect(e.window, &tb)) {
                        px_ = tb.left + (int)std::lround(liveX * sc);
                        py_ = tb.top;
                        pw_ = (int)std::lround(liveW * sc);
                        ph_ = (int)std::lround(liveH * sc);
                        tapLogf("xaml: 开始按钮坑位 -> 屏幕 (%d,%d) %dx%d (dpi=%.2f)",
                                px_, py_, pw_, ph_, sc);

                        // 顺便报告：Collapsed 之后元素的位置还在不在。
                        //
                        // 这决定了"持续跟踪"能不能靠重新读这个元素实现。
                        // Collapsed 只是不渲染、不参与命中测试，
                        // 布局信息通常还在 —— 但要实测确认。
                        if (auto el = fromHandle<wux::UIElement>(e.islandStartHandle)) {
                            try {
                                auto now = el.TransformToVisual(nullptr)
                                    .TransformPoint(winrt::Windows::Foundation::Point{0, 0});
                                tapLogf("xaml:   重新读该元素 -> (%.1f,%.1f)",
                                        now.X, now.Y);
                            } catch (...) {
                                tapLogf("xaml:   重新读该元素失败（可能已失效）");
                            }
                        } else {
                            tapLogf("xaml:   元素句柄已失效，需要靠兄弟节点定位");
                        }
                    }
                    break;
                }
            }
            // 把矩形写进共享内存的四个字段 ——
            // HRESULT 放不下一个矩形，而日志不适合程序读。
            if (ChannelShared* sh = channelShared()) {
                sh->rectLeft = px_;
                sh->rectTop = py_;
                sh->rectRight = (px_ >= 0) ? px_ + pw_ : 0;
                sh->rectBottom = (py_ >= 0) ? py_ + ph_ : 0;
            }
            channelReply((HRESULT)(px_ >= 0 ? 1 : 0));
            return;
        }

        case ChannelCmd::RestoreTransparencyOnly: {
            // 只还原透明化（背景+边框），不碰岛台按钮。
            //
            // 用途：用户在设置里关透明化时。
            // 此时岛台可能还开着，按钮该继续隐藏。
            //
            // ⚠ 和 RestoreDefault 的区别：
            //   RestoreDefault = 全量还原（启动清理/退出清理）
            //   RestoreTransparencyOnly = 只还原透明化（运行时切换）
            hr = returnToDefault(nullptr, /*restoreTransparency=*/true,
                                 /*restoreIslandButtons=*/false);
            tapLogf("channel: 收到「只还原透明化」-> 0x%08X", (unsigned)hr);
            if (SUCCEEDED(hr)) {
                HRESULT hrColor = setTrayTextColor(false);
                tapLogf("channel: 还原文字颜色 -> 0x%08X", (unsigned)hrColor);
            }
            break;
        }

        case ChannelCmd::ConsumeLayoutChanged: {
            // 取走"布局变了"的标志（取完清零）。
            // 返回 1 表示这次确实有变化 —— 主程序据此重定位岛台按钮。
            LONG was = ::InterlockedExchange(&g_layoutChanged, 0);
            channelReply((HRESULT)(was ? 1 : 0));
            return;
        }

        case ChannelCmd::SetIslandTakeover: {
            // 岛台接管开关。
            //
            // 开：隐藏任务栏的开始/搜索按钮
            // 关：还原它们
            //
            // ⚠ 关键点：开启时必须**先设标志，再重新订阅视觉树**。
            //
            //   任务栏在程序启动时就已经建好了，那些按钮的 Add 事件
            //   早就发过了。此刻只设标志不会有任何效果 ——
            //   回调不会再被调用（实测确认：命令返回 S_OK 但画面无变化）。
            //
            //   XAML 诊断没有"拉取整棵树"的接口，唯一办法是重新订阅
            //   （Unadvise + Advise），让系统把整棵树当作初始状态重发一遍。
            //   标志在重发之前设好，回调就能在这次全量通知里
            //   把开始/搜索按钮记录下来。
            const bool on = (arg != 0);
            g_islandTakeover.store(on);
            tapLogf("channel: 收到「岛台接管」= %s", on ? "开" : "关");

            if (on) {
                if (resubscribeVisualTree()) {
                    // 重订阅会把整棵树重发一遍，回调里会重新收集按钮，
                    // 并在收集到时自己 scheduleApply（跑在 XAML 线程，安全）。
                    //
                    // ⚠ 这里**不要**再手动 scheduleApply ——
                    //   通道处理跑在监听线程，跨线程投递 + 紧接着的重订阅
                    //   组合起来会把某一边卡死（实测：v4 构建之后
                    //   通道指令全部无响应，监听线程死了）。
                    // 重新订阅会重发全量元素，回调里记录了要隐藏的按钮；
                    // 但记录 != 隐藏 —— 还得真正去改 Visibility。
                    //
                    // ⚠⚠ 不再在这里调 scheduleApply(Transparent)。
                    //
                    //   踩过的坑：原来这里偷偷把任务栏设成透明，
                    //   和设置里的"透明化"功能打架 ——
                    //   用户关了透明化但开了岛台，任务栏又变透明了。
                    //
                    //   岛台开关只管"隐藏开始/搜索按钮 + 创建岛台按钮"，
                    //   透明化完全由设置里的 taskbar_transparent 管。
                    //
                    //   那隐藏按钮靠什么触发？
                    //   靠 resubscribeVisualTree 重发树通知时，
                    //   回调里看到 g_islandTakeover=true 就会自动隐藏。
                    //   不需要额外 scheduleApply。

                    // 补抓定时器仍然需要 —— 任务栏元素多，
                    // 一轮可能没到齐，要等几轮。
                    // 但补抓里也不再调 scheduleApply（见下面）。
                    ::SetTimer(g_xamlThreadWnd, kIslandCatchupTimer, 500, nullptr);
                    hr = S_OK;
                } else {
                    hr = E_FAIL;
                }
            } else {
                // 还原：只把被隐藏的开始/搜索按钮恢复可见。
                //
                // ⚠ 传 false —— 不还原背景/边框的 Opacity。
                //   那是透明化的事，由设置里的 taskbar_transparent 管。
                //   原来这里传的是默认的 true，结果关了岛台
                //   连透明化一起还原了 —— 用户设的透明就没了。
                hr = returnToDefault(nullptr, /*restoreTransparency=*/false,
                                     /*restoreIslandButtons=*/true);
            }
            tapLogf("channel: 岛台接管处理 -> 0x%08X", (unsigned)hr);
            break;
        }

        default:
            tapLogf("channel: 收到未知命令 %u", (unsigned)cmd);
            hr = E_INVALIDARG;
            break;
    }

    channelReply(hr);
    (void)h;
}

LRESULT CALLBACK applyWindowProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_GLASSTAP_APPLY) {
        GlassBrush brush;
        UINT color;
        {
            std::lock_guard<std::mutex> lk(g_pendingMu);
            if (!g_pendingValid) return 0;
            brush = g_pendingBrush;
            color = g_pendingColor;
            g_pendingValid = false;
        }

        // 此刻已退出 OnVisualTreeChange 的回调栈。理论上 XAML 允许改属性了。
        HRESULT hr = applyAppearance(nullptr, brush, color);
        tapLogf("xaml: 延迟应用外观(brush=%d) -> 0x%08X", (int)brush, (unsigned)hr);

        // 重试机制。
        //
        // 实测第一次尝试经常返回 0x80004005 (E_FAIL)，但稍后重试就能成功 ——
        // 任务栏在启动阶段仍在构建 XAML 树，属性此刻可能还处于
        // 内部锁定状态。等树稳定后再改即可。
        //
        // 这里安排 5 次重试，每次间隔 400ms，总计约 2 秒。
        if (FAILED(hr) && g_applyRetries < 5) {
            ++g_applyRetries;
            {
                std::lock_guard<std::mutex> lk(g_pendingMu);
                g_pendingBrush = brush;
                g_pendingColor = color;
                g_pendingValid = true;
            }
            tapLogf("xaml: 安排第 %d 次重试", g_applyRetries);
            ::SetTimer(h, 1, 400, nullptr);
        }
        return 0;
    }

    if (msg == WM_TIMER && w == 1) {
        ::KillTimer(h, 1);
        return ::SendMessageW(h, WM_GLASSTAP_APPLY, 0, 0);
    }

    // 岛台接管的补抓轮次。
    //
    // 重新订阅视觉树后，元素是分批通知过来的。
    // 这个定时器隔一会儿再应用一次，把晚到的按钮也隐藏掉，
    // 顺便找容器、插岛台按钮。
    // 应用幂等，重复无害。
    // 量岛台按钮的实际尺寸。
    //
    // 插入成功只说明 Children 里多了一项，不代表它真被布局、被渲染、
    // 能被点到。ActualWidth 要等布局跑完才有值，所以插完 400ms 再读。
    //
    // 如果这里报 0x0，说明容器不参与布局 —— 那就得换容器。
    if (msg == WM_TIMER && w == 0x51) {
        ::KillTimer(h, 0x51);
        if (g_islandButton) {
            try {
                const double aw = g_islandButton.ActualWidth();
                const double ah = g_islandButton.ActualHeight();
                const int vis = (int)g_islandButton.Visibility();
                auto pt = g_islandButton
                    .TransformToVisual(nullptr)
                    .TransformPoint(winrt::Windows::Foundation::Point{0, 0});
                tapLogf("xaml: 岛台按钮实测 尺寸 %.1fx%.1f 可见=%d 树内坐标 (%.1f,%.1f)",
                        aw, ah, vis, pt.X, pt.Y);
                if (aw < 1.0 || ah < 1.0) {
                    tapLogf("xaml: ⚠ 按钮尺寸为 0 —— 容器不参与布局，需要换宿主");
                }
            } catch (const winrt::hresult_error& ex) {
                tapLogf("xaml: 量尺寸异常 0x%08X", (unsigned)ex.code().value);
            }
        }
        return 0;
    }
if (msg == WM_TIMER && w == kIslandCatchupTimer) {
        static int rounds = 0;
        if (rounds < 6 && g_islandTakeover.load()) {
            ++rounds;
            tapLogf("xaml: 岛台接管补抓第 %d 轮（已记录 %d 个元素）",
                    rounds, (int)g_seenElements.size());
            // ⚠ 不再调 scheduleApply(Transparent)。
            //   原来这里偷偷做透明化，和设置里的透明化功能冲突。
            //   岛台开关只管隐藏按钮，不管透明化。
            //
            //   隐藏按钮靠回调里的 g_islandTakeover 判断自动完成，
            //   不需要额外 scheduleApply。

            // ⚠ 不在这里 scheduleApply（见 SetIslandTakeover 处的说明）。
            //   按钮的隐藏由"发现时"的 scheduleApply 触发；
            //   晚到的按钮在它们自己被发现时各自触发。

            // ★★ 已停用：不再往任务栏 XAML 树里插岛台按钮 ★★
            //
            //   这是早期方案，早就实测确认走不通：
            //   元素插进去不参与布局，停在 -10000 的未布局坐标上，不渲染。
            //
            //   更糟的是它挂的 Tapped 事件会**凭空触发** ——
            //   没有任何点击，它照样报"任务栏按钮被点击"，
            //   于是主程序那边就把面板弹出来了。
            //   用户的现象是"岛台面板有时候会自启动"。
            //
            //   现在岛台按钮是任务栏上的原生图标窗口（主程序自己创建），
            //   点击由那个窗口的 WM_ACTIVATE 上报，不再需要这一套。
            //
            //   保留 probeInjectElement 函数本身（还有 ProbeInjectElement
            //   命令可以手动调，用于将来排查），但**不再自动插入**。
            //
            // if (!g_islandButton && rounds >= 2) {
            //     HRESULT hrBtn = probeInjectElement(true);
            //     tapLogf("xaml: 补抓第 %d 轮尝试插入岛台按钮 -> 0x%08X",
            //             rounds, (unsigned)hrBtn);
            // }
            return 0;
        }
        rounds = 0;
        ::KillTimer(h, kIslandCatchupTimer);
        return 0;
    }

    // 主程序命令轮询。
    //
    // 用定时器而不是专用线程：命令必须在这个 XAML 线程上执行
    // （改 XAML 元素只能在创建它的线程），而这里正是那个线程 ——
    // 消息循环天然提供了正确的执行上下文。
    if (msg == WM_TIMER && w == kChannelTimerId) {
        handleChannelCommand(h);
        return 0;
    }

    return ::DefWindowProcW(h, msg, w, l);
}

// 在 XAML 线程上建立一个消息窗口，用来接收延迟任务。
// 这个窗口只是消息泵的载体，不可见、不参与布局。
bool ensureApplyWindow() {
    if (g_xamlThreadWnd) return true;

    static bool registered = false;
    const wchar_t* cls = L"GlassTAPApplyWnd";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = applyWindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        if (!::RegisterClassExW(&wc)) {
            // 已注册过也会失败，视为可用
            if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                tapLogf("xaml: RegisterClassEx 失败 err=%lu", ::GetLastError());
                return false;
            }
        }
        registered = true;
    }

    // HWND_MESSAGE：只收消息，不显示
    g_xamlThreadWnd = ::CreateWindowExW(0, cls, L"", 0, 0, 0, 0, 0,
                                        HWND_MESSAGE, nullptr,
                                        ::GetModuleHandleW(nullptr), nullptr);
    if (!g_xamlThreadWnd) {
        tapLogf("xaml: 创建消息窗口失败 err=%lu", ::GetLastError());
        return false;
    }

    // 启动命令轮询定时器。
    // 主程序通过共享内存下达"应用/还原/查状态"，
    // 这里负责在正确的线程上取出并执行。
    ::SetTimer(g_xamlThreadWnd, kChannelTimerId, kChannelPollMs, nullptr);

    tapLogf("xaml: 延迟应用窗口与命令通道已建立 %p", (void*)g_xamlThreadWnd);
    return true;
}

void scheduleApply(GlassBrush brush, UINT color) {
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pendingBrush = brush;
        g_pendingColor = color;
        g_pendingValid = true;
    }
    if (!ensureApplyWindow()) return;

    // 投递到当前线程队列 —— 我们就在 XAML 线程上，
    // 这条消息会在回调栈退回消息循环之后才被处理。
    ::PostMessageW(g_xamlThreadWnd, WM_GLASSTAP_APPLY, 0, 0);
}

// ── 应用外观 ─────────────────────────────────────────────────────────

// 已识别的任务栏数量（诊断与「查询状态」命令用）。
// 放在匿名命名空间里，因为它要访问 g_taskbars。
int knownTaskbarCount() {
    std::lock_guard<std::mutex> lk(g_mu);
    int n = 0;
    for (const auto& kv : g_taskbars) {
        if (kv.second.background) ++n;
    }
    return n;
}

HRESULT applyAppearance(HWND taskbar, GlassBrush brush, UINT color) {

    // ★ 岛台专用模式：只隐藏/还原按钮，不碰背景/边框。
    //
    //   岛台路径调 scheduleApply 时会设 g_islandOnlyApply = true，
    //   这里看到就跳过背景/边框的处理，做完后清掉标志。
    //
    //   正常透明化路径不设这个标志（默认 false），行为完全不变。
    const bool islandOnly = g_islandOnlyApply.exchange(false);

    // 记录期望状态：这次是"应用透明化"还是"岛台专用"。
    // 岛台专用不算透明化（它跳过背景/边框）。
    if (!islandOnly) g_transparencyWanted.store(true);

    std::lock_guard<std::mutex> lk(g_mu);

    if (g_taskbars.empty()) {
        tapLogf("xaml: applyAppearance 时还没有已识别的任务栏");
        return (HRESULT)E_FAIL;
    }

    int matched = 0;
    for (auto& [handle, e] : g_taskbars) {
        if (taskbar && e.window != taskbar) continue;
        // ⚠ 背景缺失时**不要 continue**。
        //
        //   踩过的坑：原来这里 continue 会跳过整个任务栏 ——
        //   连岛台按钮的隐藏也一起跳过。
        //   而背景和开始/搜索按钮是分别被发现的，顺序不固定；
        //   岛台的 scheduleApply 先跑到时背景可能还没就绪，
        //   按钮就漏隐藏了（状态 B 的开始/搜索没隐藏）。
        //
        //   hideElement 对空元素返回 S_OK，所以背景缺失时
        //   透明化部分自然什么都不做，不需要 continue 保护。
        if (!e.background) {
            tapLogf("xaml: 任务栏 %p 还没找到 BackgroundFill（岛台按钮仍会处理）",
                    (void*)e.window);
        } else {
            ++matched;
        }

        // 取元素句柄，然后把它整层隐藏。
        //
        // 为什么是 Opacity 而不是改 Fill —— 见 setElementOpacityZero 的说明。
        auto hideElement = [&](const auto& element, const char* what) -> HRESULT {
            if (!element) return S_OK;   // 该元素不存在，不算失败
            InstanceHandle h = 0;
            HRESULT hr = g_diag->GetHandleFromIInspectable(
                reinterpret_cast<::IInspectable*>(winrt::get_abi(element)), &h);
            if (FAILED(hr)) {
                tapLogf("xaml: 取%s句柄失败 0x%08X", what, (unsigned)hr);
                return hr;
            }
            HRESULT hrSet = setElementOpacityZero(h);
            tapLogf("xaml: 隐藏%s -> 0x%08X", what, (unsigned)hrSet);
            return hrSet;
        };

        try {
            // ── 背景层 ──
            //
            // 这一层是挡住壁纸的暗底。隐藏它，壁纸直接透出。
            //
            // ⚠ 岛台专用模式下跳过 —— 那是透明化的事，
            //   岛台只管隐藏开始/搜索按钮。
            if (!islandOnly) {
                HRESULT hrBg = hideElement(e.background, "背景");
                if (FAILED(hrBg)) return hrBg;
            }

            // ── 边框线 ──
            //
            // 任务栏顶端那条浅色细线是独立的 BackgroundStroke 元素，
            // 与背景层同级。只隐藏背景的话这条线还在，看起来像
            // "透明了但顶上留了一道边"。
            //
            // 它同样靠 Opacity 隐藏 —— 和背景层用同一条路径，
            // 不需要额外的写入方式。
            //
            // ⚠ 岛台专用模式下跳过（同背景层的理由）。
            if (!islandOnly) {
                HRESULT hrBorder = hideElement(e.border, "边框");
                (void)hrBorder;   // 边框缺失不影响背景透明，不当作失败
            }

            // ── 岛台接管：把任务栏的开始与搜索「取消固定」──
            //
            // ⚠ 用 Visibility = Collapsed，不是 Opacity = 0。
            //
            //   实测教训：Opacity = 0 只是把元素画成透明的，
            //   它仍然占位、仍然响应点击 —— 用户点原来的位置
            //   照样能点出开始菜单和搜索框。那不叫取消固定。
            //
            //   Collapsed 会把元素从布局里摘掉：
            //   不占空间（后面的元素顶上来）、不参与命中测试（点不到了）。
            //
            // 这些元素可能比背景晚出现（任务栏布局是渐进的），
            // 所以每次 applyAppearance 都重新扫一遍，
            // 晚到的会在下一轮补上。重复执行是幂等的。
            if (g_islandTakeover.load() && !g_islandButtons.empty()) {
                auto hideIsland = [&](const auto& el) -> HRESULT {
                    if (!el) return S_OK;
                    InstanceHandle h = 0;
                    HRESULT hr = g_diag->GetHandleFromIInspectable(
                        reinterpret_cast<::IInspectable*>(winrt::get_abi(el)), &h);
                    if (FAILED(hr)) return hr;
                    // 先设 Collapsed；再补一个 Opacity=0 兜底 ——
                    // 万一某些版本不认 Visibility，Opacity 至少让它看不见。
                    HRESULT hrVis = setElementCollapsed(h);
                    if (FAILED(hrVis)) {
                        hrVis = setElementOpacityZero(h);
                        tapLogf("xaml: Visibility 失败，退回 Opacity -> 0x%08X",
                                (unsigned)hrVis);
                    }
                    return hrVis;
                };

                int hiddenOk = 0;
                for (const auto& el : g_islandButtons) {
                    if (SUCCEEDED(hideIsland(el))) ++hiddenOk;
                }

                // 日志去重：只在数量变化时打，避免每轮刷屏
                static int lastLogged = -1;
                if (hiddenOk != lastLogged) {
                    tapLogf("xaml: 岛台接管 —— 已取消固定任务栏按钮 %d 个（共抓到 %d 个）",
                            hiddenOk, (int)g_islandButtons.size());
                    lastLogged = hiddenOk;
                }

                // ⚠ 插入岛台按钮不在这里做。
                //
                //   原因：g_seenElements 是随树通知逐步填充的，
                //   applyAppearance 可能在第一轮就跑到这里，
                //   那时还没收集到 Grid，找不到容器。
                //
                //   改由 kIslandCatchupTimer 在补抓几轮之后插入 ——
                //   那时元素齐了，成功率高得多。见 applyWindowProc。
            }

        } catch (const winrt::hresult_error& ex) {
            tapLogf("xaml: 应用外观异常 0x%08X（任务栏 %p, brush=%d）",
                    (unsigned)ex.code().value, (void*)e.window, (int)brush);
            return ex.code().value;
        } catch (...) {
            tapLogf("xaml: 应用外观未知异常");
            return (HRESULT)E_FAIL;
        }
    }

    if (matched == 0) {
        tapLogf("xaml: 没有可设置的任务栏（已识别 %d 个）", (int)g_taskbars.size());
        return (HRESULT)E_FAIL;
    }
    return S_OK;
}

// restoreTransparency:
//   true  = 全量还原（背景 + 边框 + 按钮）—— 用于"恢复默认"命令
//   false = 只还原按钮可见性 —— 用于"关闭岛台"
//
// ⚠ 为什么分两种：
//   原来岛台关闭时也还原了背景/边框的 Opacity，
//   等于把用户在设置里开的透明化也关掉了。
//   岛台只管"隐藏开始/搜索按钮"，不该碰透明化。
// restoreIslandButtons:
//   true  = 还原被隐藏的开始/搜索按钮
//   false = 不碰它们（岛台还开着，按钮该继续隐藏）
//
// ⚠ 为什么需要这个参数：
//   关闭透明化时调 returnToDefault，原来会把岛台按钮也还原 ——
//   等于把岛台关了。用户只是关透明化，没想关岛台。
HRESULT returnToDefault(HWND taskbar, bool restoreTransparency, bool restoreIslandButtons) {
    // 还原透明化 → 期望状态变 false。
    // 之后回调再发现新元素也不会重放透明化。
    if (restoreTransparency) g_transparencyWanted.store(false);

    std::lock_guard<std::mutex> lk(g_mu);

    // 还原 = 把 Opacity 写回 1。
    //
    // 我们改的是 Opacity 而不是 Fill，所以还原也只动 Opacity ——
    // 不要碰 Fill，那个属性我们从未成功写过，硬写只会拿到 E_FAIL。
    //
    // 背景层与边框线都要还原：应用时两个都隐藏了，
    // 只还原一个会留下"背景回来了但边上缺一条线"的怪状态。
    auto restoreElement = [&](const auto& element, const char* what) {
        if (!element || !g_diag) return;
        try {
            InstanceHandle h = 0;
            HRESULT hr = g_diag->GetHandleFromIInspectable(
                reinterpret_cast<::IInspectable*>(winrt::get_abi(element)), &h);
            if (SUCCEEDED(hr) && h) {
                HRESULT hrRestore = restoreElementOpacity(h);
                tapLogf("xaml: 还原%s -> 0x%08X", what, (unsigned)hrRestore);
            }
        } catch (...) {
            tapLogf("xaml: 还原%s 时异常", what);
        }
    };

    for (auto& [handle, e] : g_taskbars) {
        if (taskbar && e.window != taskbar) continue;

        // ★ 只在 restoreTransparency=true 时还原背景/边框。
        //
        //   岛台关闭时传 false —— 只还原按钮可见性，
        //   不碰透明化（那是设置里的独立功能）。
        if (restoreTransparency) {
            restoreElement(e.background, "背景");
            restoreElement(e.border, "边框");
        }

        // 岛台接管的开始/搜索按钮也要还原 —— 而且是**真正还原**：
        // 它们在应用时被设成了 Collapsed，只把 Opacity 写回 1 是不够的
        // （元素还是从布局里摘掉的状态，任务栏会一直缺这两个入口）。
        // ★ 只在 restoreIslandButtons=true 时还原按钮。
        //
        //   关闭透明化时传 false —— 岛台还开着，
        //   开始/搜索按钮该继续隐藏。
        if (restoreIslandButtons)
        for (const auto& el : g_islandButtons) {
            if (!el || !g_diag) continue;
            try {
                InstanceHandle h = 0;
                if (SUCCEEDED(g_diag->GetHandleFromIInspectable(
                        reinterpret_cast<::IInspectable*>(winrt::get_abi(el)), &h)) && h) {
                    HRESULT hrVis = setElementVisible(h);
                    HRESULT hrOp = restoreElementOpacity(h);
                    tapLogf("xaml: 还原任务栏按钮 Visibility=0x%08X Opacity=0x%08X",
                            (unsigned)hrVis, (unsigned)hrOp);
                }
            } catch (...) {
                tapLogf("xaml: 还原任务栏按钮时异常");
            }
        }
        e.islandHidden.clear();
    }

    // 全局列表在还原按钮后清空 —— 下次开启岛台会重新收集。
    if (restoreIslandButtons) g_islandButtons.clear();

    return S_OK;
}

// explorer 自己改画刷时（比如切换主题），同步更新我们记录的 originalFill
void onFillChanged(const wux::DependencyObject& sender, const wux::DependencyProperty&) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [handle, e] : g_taskbars) {
        if (sender == e.background && !e.originalFill) {
            e.originalFill = e.background.Fill();
        }
    }
}

}  // namespace

// 对外暴露的"建立通道"入口（见 visualtree.h 的说明）。
//
// 单独包一层而不是直接把 ensureApplyWindow 放出去：
// ensureApplyWindow 在匿名命名空间里，只给本文件用。
//
// ⚠ 为什么必须由外部主动调：这个窗口原来是懒创建的
//   （只有 scheduleApply 会调）。于是"通道能不能通"取决于
//   "有没有人调过 scheduleApply"，而调它的条件又依赖通道
//   （岛台开关靠通道下发）—— 死锁。透明=关时没人触发，
//   通道就一直不通，主程序所有指令超时。
bool ensureChannelReady() {
    return ensureApplyWindow();
}

// ── 对外动作（供 COM 服务调用）────────────────────────────────────────

HRESULT applyToAll(GlassBrush brush, UINT color) { return applyAppearance(nullptr, brush, color); }
HRESULT applyToOne(HWND tb, GlassBrush brush, UINT color) { return applyAppearance(tb, brush, color); }
HRESULT restoreAllBars(HWND tb) { return returnToDefault(tb); }

// 之后的实现（回调类、服务接口记录、重新订阅）都放回匿名命名空间内 ——
// 它们要访问 g_mu / g_taskbars 这些内部状态，而前向声明就在那个
// 命名空间里。写在外部会链接失败（LNK2019）。
namespace {

// ── 视觉树回调 ───────────────────────────────────────────────────────
//
// ⚠ SDK 10.0.22621.0 的 xamlOM.h 里只有 IVisualTreeServiceCallback 和
//   IVisualTreeServiceCallback2，**没有 Callback3**。
//
// ⚠⚠ 两个接口都要列在 implements<> 里，不能只写派生最深的那个。
//
//   C++/WinRT 的 implements<> 只为**显式列出的**接口生成 QueryInterface 分支，
//   它不会因为 MIDL 继承关系自动暴露基接口。
//   实测：只写 IVisualTreeServiceCallback2 时，
//   QueryInterface(IID_IVisualTreeServiceCallback) 返回 0x80004002
//   (E_NOINTERFACE)，视觉树回调就永远挂不上。
//   （日志原文：xaml: QI callback -> 0x80004002 ptr=0000000000000000）
class VisualTreeCallback
    : public winrt::implements<VisualTreeCallback,
                               IVisualTreeServiceCallback,
                               IVisualTreeServiceCallback2> {
public:
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation relation,
        VisualElement element,
        VisualMutationType mutationType) noexcept override;

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        InstanceHandle element,
        VisualElementState elementState,
        LPCWSTR context) noexcept override {
        (void)element; (void)elementState; (void)context;
        return S_OK;
    }
};

HRESULT VisualTreeCallback::OnVisualTreeChange(
    ParentChildRelation relation,
    VisualElement element,
    VisualMutationType mutationType) noexcept {
    try {
        std::wstring type = element.Type ? element.Type : L"";
        std::wstring name = element.Name ? element.Name : L"";

        if (mutationType == Add) {
            // 0) 探针：记录见到的所有元素类型（去重）。
            //
            // 目的是摸清托盘区（时间/日期/WLAN/音量/输入法）到底叫什么类型。
            // 这些元素的颜色来自 Foreground 属性，任务栏背景透明后
            // 如果壁纸是浅色，白字会看不清 —— 需要按背景明暗自动切深浅色。
            //
            // 已摸清的结构（build 26200）：
            //   Taskbar.TaskbarBackground        背景容器
            //   SystemTray.SystemTrayFrame       右侧托盘区根节点
            //   SystemTray.DateTimeIconContent   时间/日期
            //   SystemTray.LanguageTextIconContent 输入法/语言
            //   SystemTray.BatteryIconContent    电池
            //   SystemTray.IconView / NotifyIconView  WLAN / 音量 / 通知
            //   SystemTray.AdaptiveTextBlock     托盘区文字载体 ← 颜色在这里
            if (g_probeTrayElements && !type.empty()) {
                bool isNew = false;
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    isNew = g_seenTypes.insert(type).second;
                }
                if (isNew) {
                    tapLogw(L"probe: 类型 %s", type.c_str());
                }
            }

            // 记录所有元素句柄 + 类型 —— 找容器要用。
            //
            // 只在 g_islandTakeover 打开时记（说明用户在用岛台），
            // 平时不占内存。
            if (g_islandTakeover.load() && !type.empty()) {
                std::lock_guard<std::mutex> lk(g_mu);
                // 防重：同一个句柄只记一次
                bool dup = false;
                for (const auto& [h, t] : g_seenElements) {
                    if (h == element.Handle) { dup = true; break; }
                }
                if (!dup) g_seenElements.push_back({element.Handle, type});
            }

            // 托盘区的文字元素：记录下来，之后要按背景明暗改颜色。
            //
            // 实测这批元素的 Foreground 索引：
            //   TextBlock 系（时间/日期/电池/各图标文字）-> 1167
            //   AdaptiveTextBlock 系（输入法等）         -> 876
            //
            // 它们的值句柄完全相同，说明共用一个主题画刷。
            if (type == L"SystemTray.AdaptiveTextBlock" ||
                type == L"Windows.UI.Xaml.Controls.TextBlock") {
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    g_trayTextElements.push_back(element.Handle);
                }
                if (g_probeTrayElements) {
                    probeForegroundOf(element.Handle, type.c_str(), name.c_str());
                }
            }

            // 1) 有新的 XAML 源出现 —— 先记下，等它长出 TaskbarFrame 再配对
            // ★ 任务栏应用图标增减 / 重排 —— 岛台按钮要跟着挪。
            //
            //   Taskbar.TaskListButton 就是任务栏上每个应用图标。
            //   它被 Add（新开一个程序并固定）或 Remove（取消固定）时，
            //   整个图标组会重排，居中组的起点也跟着变。
            //
            //   这里只置一个标志位（回调栈里不能做耗时操作），
            //   主程序下一次轮询取走并重定位。
            if (type == L"Taskbar.TaskListButton") {
                ::InterlockedExchange(&g_layoutChanged, 1);

                // ★★ 记录**图标组的左边缘**。
                //
                //   之前量的是开始按钮的坑位 —— 那个**恒定不变**，
                //   而任务栏图标组是居中布局，会随图标数量左右移动。
                //   所以按钮停在原地，和移动的图标组重叠了。
                //
                //   正确做法：量第一个 TaskListButton 的位置。
                //   用户的要求是"紧贴在图标组最左边"，
                //   那就该以图标组为准。
                try {
                    auto fe = fromHandle<wux::FrameworkElement>(element.Handle);
                    if (fe) {
                        // 相对任务栏窗口的偏移
                        auto pt = fe.TransformToVisual(nullptr)
                                      .TransformPoint({0, 0});
                        {
                            std::lock_guard<std::mutex> lk(g_mu);
                            g_firstIconX = pt.X;
                            g_firstIconY = pt.Y;
                            g_firstIconW = fe.ActualWidth();
                            g_firstIconH = fe.ActualHeight();
                            g_firstIconValid = 1;
                        }
                    }
                } catch (...) {
                    // 拿不到就算了，主程序会用兜底逻辑
                }

                static int logged = 0;
                if (logged < 5) {
                    ++logged;
                    tapLogf("xaml: 任务栏图标%s -> 通知主程序重定位",
                            mutationType == Add ? "新增" : "移除");
                }
            }

            if (type == L"Windows.UI.Xaml.Hosting.DesktopWindowXamlSource") {
                std::lock_guard<std::mutex> lk(g_mu);
                g_pendingSources.insert(element.Handle);
            }
            // 2) 找到 TaskbarFrame —— 与待配对的 XAML 源关联，拿到 HWND
            else if (type == L"Taskbar.TaskbarFrame") {
                auto rootGrid = fromHandle<wux::UIElement>(relation.Parent);

                std::lock_guard<std::mutex> lk(g_mu);
                for (auto it = g_pendingSources.begin(); it != g_pendingSources.end(); ++it) {
                    auto src = fromHandle<wuxh::DesktopWindowXamlSource>(*it);
                    if (!src) continue;

                    wux::UIElement content{nullptr};
                    try {
                        content = src.Content();
                    } catch (const winrt::hresult_wrong_thread&) {
                        continue;   // 不是当前线程的对象，跳过
                    }
                    if (content != rootGrid) continue;

                    // 拿到承载这个 XAML 的窗口句柄
                    HWND hwnd = nullptr;
                    auto native = src.as<IDesktopWindowXamlSourceNative>();
                    if (native && FAILED(native->get_WindowHandle(&hwnd))) hwnd = nullptr;

                    TaskbarEntry e;
                    e.window = hwnd;
                    g_taskbars[element.Handle] = std::move(e);
                    g_pendingSources.erase(it);
                    tapLogf("xaml: 注册任务栏 hwnd=%p", (void*)hwnd);
                    break;
                }
            }
            // 2b) 岛台接管模式：抓取任务栏的「开始」与「搜索」按钮
            //
            // 这两个按钮的完整类型名（实测自本机任务栏 XAML 树）：
            //   开始  Taskbar.ExperienceToggleButton
            //   搜索  SearchUx.SearchUI.SearchButtonControl
            //
            // 找到后先记下来，等 applyAppearance 执行时统一隐藏 ——
            // 此刻还在回调栈内，XAML 不允许直接改属性（会返回 E_FAIL）。
            else if (type == L"Taskbar.ExperienceToggleButton" ||
                     type == L"SearchUx.SearchUI.SearchButtonControl") {
                if (type == L"Taskbar.ExperienceToggleButton") {
                    // ★ 这就是开始按钮。把它的容器和下标记下来 ——
                    //   岛台按钮要插在**同一个位置**，顶替它。
                    //
                    //   relation.Parent 是它的父容器，relation.ChildIndex
                    //   是它在容器里的下标。有了这两个，插进去的按钮
                    //   就和原来开始按钮处在同一个坑里，
                    //   图标组会自动往右顺移，不用我们算任何像素。
                    auto parentPanel = fromHandle<wux::Controls::Panel>(relation.Parent);
                    if (parentPanel) {
                        std::lock_guard<std::mutex> lk(g_mu);
                        for (auto& [h, e] : g_taskbars) {
                            e.islandHostPanel = parentPanel;
                            e.islandHostIndex = relation.ChildIndex;
                            e.islandHostValid = true;

                            // 报告容器的具体类型 —— 这决定了要复制哪种附加属性。
                            //
                            // Grid 用 Grid.Row/Column，Canvas 用 Canvas.Left/Top，
                            // StackPanel 什么都不用（按顺序排）。
                            // 不知道类型就没法把按钮摆到正确的格子里。
                            {
                                wchar_t cn[128] = L"?";
                                try {
                                    auto kind = winrt::get_class_name(parentPanel);
                                    wcsncpy_s(cn, kind.c_str(), _TRUNCATE);
                                } catch (...) {}
                                tapLogw(L"xaml: 开始按钮容器类型 = %s，下标 %u",
                                        cn, relation.ChildIndex);

                                // ★★ 方案 A：把岛台图标插进这个容器。
                                //
                                //   之前插 ItemsRepeater 失败（元素被丢到 -10000），
                                //   因为它只认自己 ElementFactory 生成的元素。
                                //
                                //   但如果这个容器是**真正的 Grid/Panel**，
                                //   它的 Children 就是普通集合，插进去会参与布局。
                                //
                                //   关键判据：插完后 ActualOffset 是不是正常坐标。
                                //   -10000 表示"没被布局"，那就说明这条路也不通。
                                try {
                                    auto panel = parentPanel.try_as<wux::Controls::Panel>();
                                    if (panel) {
                                        tapLogf("xaml: ★ 容器能当 Panel 用，试试插入");

                                        wux::Controls::FontIcon probeIcon;
                                        probeIcon.FontFamily(wux::Media::FontFamily(
                                            L"Segoe MDL2 Assets"));
                                        probeIcon.Glyph(L"\uE71D");
                                        probeIcon.FontSize(16);

                                        const unsigned before = panel.Children().Size();
                                        panel.Children().InsertAt(0, probeIcon);
                                        const unsigned after = panel.Children().Size();
                                        tapLogf("xaml: ★ 容器 Children %u -> %u",
                                                before, after);
                                        // 探测用全局变量，不用 entry 字段
                                        g_probeIcon = probeIcon;

                                        // 布局要等下一轮才发生，
                                        // 所以 ActualOffset 得延后查。
                                        // 这里先把 handle 记下。
                                    } else {
                                        tapLogf("xaml: 容器不是 Panel，方案 A 不通");
                                    }
                                } catch (const winrt::hresult_error& ex) {
                                    tapLogf("xaml: 插入容器失败 0x%08X",
                                            (unsigned)ex.code().value);
                                } catch (...) {
                                    tapLogf("xaml: 插入容器失败（未知异常）");
                                }
                            }

                            // 顺便把开始按钮自己的布局属性读出来，
                            // 待会儿要给岛台按钮设成一样的。
                            if (auto startBtn =
                                    fromHandle<wux::FrameworkElement>(element.Handle)) {
                                // ★★ 探测：开始按钮是不是 ContentControl，
                                //    它的 Content 能不能换。
                                //
                                //    如果能换，就把我们的图标放进去 ——
                                //    那样就真的在任务栏布局系统里了，
                                //    图标增减时自动跟着走，不会重叠。
                                //
                                //    这比改 ElementFactory 安全得多：
                                //    只是给已有控件换内容，不动容器结构。
                                try {
                                    auto cc = startBtn.try_as<wux::Controls::ContentControl>();
                                    if (cc) {
                                        tapLogf("xaml: ★ 开始按钮是 ContentControl");
                                        auto cur = cc.Content();
                                        if (cur) {
                                            auto ck = winrt::get_class_name(cur);
                                            wchar_t cn2[256] = L"?";
                                            wcsncpy_s(cn2, ck.c_str(), _TRUNCATE);
                                            tapLogw(L"xaml: ★ 当前 Content = %s", cn2);
                                        } else {
                                            tapLogf("xaml: ★ 当前 Content 为空");
                                        }
                                        e.islandStartIsContentControl = true;
                                    } else {
                                        tapLogf("xaml: 开始按钮不是 ContentControl");
                                    }
                                } catch (const winrt::hresult_error& ex) {
                                    tapLogf("xaml: 探测 Content 异常 0x%08X",
                                            (unsigned)ex.code().value);
                                } catch (...) {
                                    tapLogf("xaml: 探测 Content 未知异常");
                                }

                                // ★★ 已停用：不再把岛台图标"嵌入"开始按钮的 Content ★★
                                //
                                //   这里曾经以为找到了安全的嵌入方式
                                //   （开始按钮是 ContentControl，Content 是空的，
                                //    直接塞进去就行）。
                                //
                                //   但它和"真正取消固定开始按钮"是互斥的：
                                //   我们按需求把开始按钮设成了 Collapsed，
                                //   而 Collapsed 的元素**永远不会渲染** ——
                                //   塞进去的内容自然也永远看不见。
                                //
                                //   实测就是这样：日志里 step1..step9 全部成功，
                                //   屏幕上什么都没有。属于白做工，
                                //   而且给每次树通知都增加了一堆无用的 XAML 操作。
                                //
                                //   现在岛台图标走任务栏原生图标窗口那条路。
                                // 嵌入方案已废弃（见上面说明）。
                                // 留一个开关，将来真要恢复时改这里就行。
                                constexpr bool kEmbedIntoStartButton = false;
                                auto cc = startBtn.try_as<wux::Controls::ContentControl>();
                                if (kEmbedIntoStartButton && cc) {
                                    if (!cc.Content()) {
                                        // ⚠ 逐步打点，定位到底哪一步失败。
                                        //   之前的 catch(...) 把信息全吞了，
                                        //   只留下一句"未知异常"，没法查。
                                        int step = 0;
                                        try {
                                            step = 1;
                                            wux::Controls::Grid host;
                                            tapLogf("xaml: [嵌入] step1 Grid 构造 OK");

                                            step = 2;
                                            host.HorizontalAlignment(
                                                wux::HorizontalAlignment::Stretch);
                                            host.VerticalAlignment(
                                                wux::VerticalAlignment::Stretch);
                                            tapLogf("xaml: [嵌入] step2 对齐 OK");

                                            step = 3;
                                            wux::Controls::FontIcon ic;
                                            tapLogf("xaml: [嵌入] step3 FontIcon 构造 OK");

                                            step = 4;
                                            ic.FontFamily(wux::Media::FontFamily(
                                                L"Segoe MDL2 Assets"));
                                            tapLogf("xaml: [嵌入] step4 FontFamily OK");

                                            step = 5;
                                            ic.Glyph(L"\uE71D");
                                            ic.FontSize(16);
                                            tapLogf("xaml: [嵌入] step5 Glyph/Size OK");

                                            step = 6;
                                            ic.HorizontalAlignment(
                                                wux::HorizontalAlignment::Center);
                                            ic.VerticalAlignment(
                                                wux::VerticalAlignment::Center);
                                            tapLogf("xaml: [嵌入] step6 对齐 OK");

                                            step = 7;
                                            ic.Foreground(wux::Media::SolidColorBrush(
                                                winrt::Windows::UI::ColorHelper::FromArgb(
                                                    0xFF, 0xF5, 0xF5, 0xF7)));
                                            tapLogf("xaml: [嵌入] step7 Foreground OK");

                                            step = 8;
                                            host.Children().Append(ic);
                                            tapLogf("xaml: [嵌入] step8 Append OK");

                                            step = 9;
                                            cc.Content(host);
                                            tapLogf("xaml: [嵌入] ★★ step9 Content 赋值成功");

                                            step = 10;
                                            // ⚠ 不加 g_mu。
                                            //
                                            //   实测：在这里 lock 会直接抛
                                            //   "resource deadlock would occur" ——
                                            //   因为 OnVisualTreeChange 这条调用路径上
                                            //   那把锁已经被持有了（通道处理那边），
                                            //   再 lock 就是自己等自己。
                                            //
                                            //   而 e 是本次回调自己的 TaskbarEntry，
                                            //   别的线程碰不到 —— 本来就不需要锁。
                                            e.islandEmbedded = true;
                                            tapLogf("xaml: ★★ 已把岛台图标嵌入开始按钮");
                                        } catch (const winrt::hresult_error& ex) {
                                            tapLogf("xaml: [嵌入] step%d 失败 HRESULT 0x%08X (%s)",
                                                    step, (unsigned)ex.code().value,
                                                    winrt::to_string(ex.message()).c_str());
                                        } catch (const std::exception& ex) {
                                            tapLogf("xaml: [嵌入] step%d 失败 std 异常: %s",
                                                    step, ex.what());
                                        } catch (...) {
                                            tapLogf("xaml: [嵌入] step%d 失败（未知异常类型）",
                                                    step);
                                        }
                                    } else {
                                        tapLogf("xaml: Content 非空，跳过嵌入");
                                    }
                                }

                                try {
                                    e.islandStartCol = wux::Controls::Grid::GetColumn(startBtn);
                                    e.islandStartRow = wux::Controls::Grid::GetRow(startBtn);
                                    // ★ 读它在屏幕上的真实位置和尺寸。
                                    //
                                    // 这是后续把岛台按钮摆到“开始按钮原来那个坑”
                                    // 的唯一依据 —— 不管后面用哪种方式实现。
                                    //
                                    // ⚠ 必须在隐藏之前读。
                                    //   Collapsed 之后 ActualWidth 会变 0、坐标也不再更新。
                                    const double aw = startBtn.ActualWidth();
                                    const double ah = startBtn.ActualHeight();
                                    auto pt = startBtn
                                        .TransformToVisual(nullptr)
                                        .TransformPoint(winrt::Windows::Foundation::Point{0, 0});
                                    e.islandStartHandle = element.Handle;
                                    e.islandStartX = pt.X;
                                    e.islandStartY = pt.Y;
                                    e.islandStartW = aw;
                                    e.islandStartH = ah;
                                    e.islandStartPosValid = (aw > 1.0 && ah > 1.0);
                                    tapLogf("xaml: 开始按钮位置 (%.1f,%.1f) 尺寸 %.1fx%.1f 有效=%d",
                                            pt.X, pt.Y, aw, ah, (int)e.islandStartPosValid);
                                } catch (const winrt::hresult_error& ex) {
                                    tapLogf("xaml: 读开始按钮位置异常 0x%08X",
                                            (unsigned)ex.code().value);
                                } catch (...) {
                                    tapLogf("xaml: 读开始按钮位置失败");
                                }
                            }
                            break;
                        }
                    } else {
                        tapLogf("xaml: 开始按钮的父容器转不成 Panel，无法定位岛台按钮");
                    }
                }

                if (g_islandTakeover.load()) {
                    auto el = fromHandle<wux::UIElement>(element.Handle);
                    if (el) {
                        std::lock_guard<std::mutex> lk(g_mu);

                        // 去重后记入**全局列表**。
                        //
                        // ⚠ 不再依赖 g_taskbars 里已有条目 ——
                        //   按钮和条目的发现顺序不固定，
                        //   依赖的话按钮先到就永久漏隐藏。
                        InstanceHandle eh = 0;
                        bool known = false;
                        if (g_diag &&
                            SUCCEEDED(g_diag->GetHandleFromIInspectable(
                                reinterpret_cast<::IInspectable*>(winrt::get_abi(el)),
                                &eh))) {
                            for (const auto& prev : g_islandButtons) {
                                InstanceHandle ph = 0;
                                if (g_diag &&
                                    SUCCEEDED(g_diag->GetHandleFromIInspectable(
                                        reinterpret_cast<::IInspectable*>(winrt::get_abi(prev)),
                                        &ph)) && ph == eh) {
                                    known = true;
                                    break;
                                }
                            }
                        }

                        if (!known) {
                            g_islandButtons.push_back(el);
                            // ★ 标记这次是岛台触发的 ——
                            //   applyAppearance 看到后只隐藏按钮，
                            //   不碰背景/边框的 Opacity（那是透明化的事）。
                            g_islandOnlyApply.store(true);
                            scheduleApply(GlassBrush::Transparent, 0);
                            tapLogf("xaml: 记录待隐藏的任务栏按钮（共 %d 个）",
                                    (int)g_islandButtons.size());
                        }
                    }
                }
            }
            // 3) 找到背景/边框的 Rectangle
            else if (type == L"Windows.UI.Xaml.Shapes.Rectangle") {
                bool isBg = (name == L"BackgroundFill");
                bool isBorder = (name == L"BackgroundStroke");
                if (isBg || isBorder) {
                    auto parentEl = fromHandle<wux::FrameworkElement>(relation.Parent);
                    if (auto frame = findParent(L"TaskbarFrame", parentEl)) {
                        InstanceHandle frameHandle = 0;
                        std::lock_guard<std::mutex> lk(g_mu);
                        if (g_diag &&
                            SUCCEEDED(g_diag->GetHandleFromIInspectable(
                                reinterpret_cast<::IInspectable*>(winrt::get_abi(frame)),
                                &frameHandle))) {
                            auto it = g_taskbars.find(frameHandle);
                            if (it != g_taskbars.end()) {
                                auto shape = fromHandle<wux::Shapes::Rectangle>(element.Handle);
                                if (shape) {
                                    if (isBg) {
                                        it->second.background = shape;
                                        // 记录原始画刷 —— 还原的唯一依据。
                                        // 没有它就不能还原，所以拿不到就不接管。
                                        it->second.originalFill = shape.Fill();
                                        it->second.fillToken =
                                            shape.RegisterPropertyChangedCallback(
                                                wux::Shapes::Shape::FillProperty(),
                                                &onFillChanged);
                                        tapLogf("xaml: 找到任务栏背景 Fill=%p",
                                             (void*)winrt::get_abi(it->second.originalFill));

                                        // 找到背景元素了，但此刻还不能改属性。
                                        //
                                        // 我们正处于 OnVisualTreeChange 的回调栈内，
                                        // XAML 不允许在这里改动同一批属性 ——
                                        // 实测直接改会返回 0x80004005 (E_FAIL)。
                                        //
                                        // 投递一条消息，等回调栈退回消息循环后再改。
                                        //
                                        // 注意：边框（BackgroundStroke）可能比背景晚
                                        // 一两个事件才出现。applyAppearance 里对缺失的
                                        // 边框是容忍的，而延迟应用带重试，
                                        // 所以晚到也能在后续几轮里补上。
                                        // ⚠ 只有"用户想要透明"时才重放。
                                        //
                                        //   原来无条件 scheduleApply(Transparent) ——
                                        //   启动时还原透明化的操作会被这个后到的
                                        //   重放覆盖（状态 D 莫名变透明的根因）。
                                        //
                                        //   现在重放的是 g_transparencyWanted 记录的
                                        //   期望状态：不想要透明就什么都不做，
                                        //   元素保持系统默认（不透明）。
                                        if (g_transparencyWanted.load()) {
                                            scheduleApply(GlassBrush::Transparent, 0x00000000);
                                        }
                                    } else {
                                        it->second.border = shape;
                                        it->second.originalBorderFill = shape.Fill();
                                        it->second.borderToken =
                                            shape.RegisterPropertyChangedCallback(
                                                wux::Shapes::Shape::FillProperty(),
                                                &onFillChanged);
                                        tapLogf("xaml: 找到任务栏边框");

                                        // 边框到得晚：再排一次应用，
                                        // 把这条线也一起隐藏掉。
                                        // 背景早已隐藏过，重复应用是幂等的。
                                        if (it->second.background && g_transparencyWanted.load()) {
                                            scheduleApply(GlassBrush::Transparent, 0x00000000);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (mutationType == Remove) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_pendingSources.erase(element.Handle);
            auto it = g_taskbars.find(element.Handle);
            if (it != g_taskbars.end()) {
                try {
                    if (it->second.background && it->second.fillToken != 0) {
                        it->second.background.UnregisterPropertyChangedCallback(
                            wux::Shapes::Shape::FillProperty(), it->second.fillToken);
                    }
                    if (it->second.border && it->second.borderToken != 0) {
                        it->second.border.UnregisterPropertyChangedCallback(
                            wux::Shapes::Shape::FillProperty(), it->second.borderToken);
                    }
                } catch (...) {}
                g_taskbars.erase(it);
            }
        }
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}

// ── 注册 / 注销回调 ─────────────────────────────────────────────────

winrt::com_ptr<VisualTreeCallback> g_callback;

// 当前订阅用的视觉树服务接口（IVisualTreeService3/2/1 之一）。
//
// 留着它是因为 XAML 诊断**没有遍历树的接口** —— xamlOM.h 里只有
// Advise/Unadvise 加属性读写。想重新拿到整棵树的元素，
// 唯一办法是 Unadvise 再 Advise：重新订阅会让系统把整棵树
// 当作初始状态再通知一遍。
//
// 岛台功能依赖这个：开启时任务栏早已建好、不会再发 Add 事件，
// 必须靠重新订阅才能抓到「开始」「搜索」这些按钮。
winrt::com_ptr<::IUnknown> g_vts;

// 重新订阅视觉树，强制系统重发整棵树的通知。
//
// ⚠ 会先清掉已记录的状态（g_taskbars），因为接下来会重新收到
//   全量的 Add 通知，不清会留下重复条目。
//
// 返回是否成功重新订阅。
bool resubscribeVisualTree() {
    if (!g_vts || !g_callback) {
        tapLogf("xaml: 重新订阅失败 —— 服务或回调不存在");
        return false;
    }

    auto vts = g_vts.try_as<IVisualTreeService>();
    if (!vts) {
        tapLogf("xaml: 重新订阅失败 —— 无法转为 IVisualTreeService");
        return false;
    }

    IVisualTreeServiceCallback* cb = nullptr;
    {
        winrt::com_ptr<::IUnknown> unk = g_callback.as<::IUnknown>();
        if (!unk || FAILED(unk->QueryInterface(__uuidof(IVisualTreeServiceCallback),
                                               (void**)&cb)) || !cb) {
            tapLogf("xaml: 重新订阅失败 —— 取不到回调接口");
            return false;
        }
    }

    HRESULT hrUn = vts->UnadviseVisualTreeChange(cb);
    tapLogf("xaml: Unadvise -> 0x%08X", (unsigned)hrUn);

    // 清掉旧记录 —— 下面的重新订阅会重新送来全量元素
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_taskbars.clear();
        g_pendingSources.clear();
    }

    HRESULT hrAdv = vts->AdviseVisualTreeChange(cb);
    tapLogf("xaml: 重新订阅 Advise -> 0x%08X", (unsigned)hrAdv);
    return SUCCEEDED(hrAdv);
}

}  // namespace（内部实现结束）

// ── 对外接口（visualtree.h 声明，必须放在匿名命名空间之外）──────────

bool attachDiagnostics(IXamlDiagnostics* diag) {
    tapLogf("xaml: attachDiagnostics 进入 diag=%p", (void*)diag);
    if (!diag) return false;
    g_diag = diag;

    // 整个挂载过程包异常保护。
    //
    // 实测曾经出现 0x80004002 直接抛出、而日志里一片空白的情况 ——
    // 那种失败完全无法定位。C++/WinRT 的 as<> / get_abi 在
    // MIDL 多级继承接口上会抛异常，必须接住并记录。
    try {
        tapLogf("xaml: 创建回调对象");
        g_callback = winrt::make_self<VisualTreeCallback>();
        tapLogf("xaml: 回调对象已创建，取 ABI 指针");

        // 取 implements<> 对象的 IUnknown。
        //
        // g_callback 是 winrt::com_ptr<VisualTreeCallback>，而 VisualTreeCallback
        // 由 winrt::implements<> 派生，其 QueryInterface 覆盖所有声明过的接口。
        //
        // 这里必须用 as<::IUnknown>()，不能写 get_abi(*g_callback)：
        // 后者返回的是 WinRT 接口引用而不是 IUnknown*，编译报 C2440。
        winrt::com_ptr<::IUnknown> unk = g_callback.as<::IUnknown>();
        tapLogf("xaml: IUnknown = %p", (void*)unk.get());
        if (!unk) {
            tapLogf("xaml: 无法取得 IUnknown");
            return false;
        }

        IVisualTreeServiceCallback* cb = nullptr;
        HRESULT hrQi = unk->QueryInterface(__uuidof(IVisualTreeServiceCallback), (void**)&cb);
        tapLogf("xaml: QI callback -> 0x%08X ptr=%p", (unsigned)hrQi, (void*)cb);
        if (FAILED(hrQi) || !cb) return false;

        // 逐级尝试 QueryInterface + AdviseVisualTreeChange。
        // IXamlDiagnostics 本身不实现 IVisualTreeService，必须显式 QI。
        auto tryAdvise = [&](REFIID iid, const char* name) -> bool {
            winrt::com_ptr<IUnknown> svc;
            HRESULT hr = diag->QueryInterface(iid, svc.put_void());
            tapLogf("xaml: QI %s -> 0x%08X ptr=%p", name, (unsigned)hr, svc.get());
            if (FAILED(hr) || !svc) return false;

            auto vts = svc.try_as<IVisualTreeService>();
            if (!vts) {
                tapLogf("xaml: %s 无法转为 IVisualTreeService", name);
                return false;
            }
            HRESULT hrAdv = vts->AdviseVisualTreeChange(cb);
            tapLogf("xaml: Advise(%s) -> 0x%08X", name, (unsigned)hrAdv);
            if (SUCCEEDED(hrAdv)) {
                // ⚠ 必须把服务接口留下来。
                //
                // XAML 诊断**没有**"主动拉取整棵树"的接口（xamlOM.h 里
                // 只有 Advise/Unadvise 加属性读写，没有遍历）。
                // 所以想重新拿到整棵树的元素，唯一的办法是：
                //   Unadvise → Advise
                // 重新订阅会让系统把整棵树当作初始状态再通知一遍。
                //
                // 岛台功能需要这个：开启时任务栏早已建好、不会再发 Add 事件，
                // 必须靠重新订阅才能抓到「开始」「搜索」这些按钮。
                //
                // 留一个引用（AddRef 由 com_ptr 管理）保证之后还能用。
                g_vts = svc;
                return true;
            }
            return false;
        };

        if (tryAdvise(__uuidof(IVisualTreeService3), "IVisualTreeService3")) return true;
        if (tryAdvise(__uuidof(IVisualTreeService2), "IVisualTreeService2")) return true;
        if (tryAdvise(__uuidof(IVisualTreeService), "IVisualTreeService")) return true;

        tapLogf("xaml: 所有 VisualTreeService 版本都不可用");
        return false;

    } catch (const winrt::hresult_error& ex) {
        tapLogf("xaml: 异常 0x%08X", (unsigned)ex.code().value);
        return false;
    } catch (const std::exception& ex) {
        tapLogf("xaml: std 异常: %s", ex.what());
        return false;
    } catch (...) {
        tapLogf("xaml: 未知异常");
        return false;
    }
}

void detachDiagnostics() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_taskbars.clear();
    g_pendingSources.clear();
    g_callback = nullptr;
    g_diag = nullptr;
}

}  // namespace xaml
}  // namespace tb
}  // namespace hr