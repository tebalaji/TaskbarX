// TaskbarX11Hook - Windows 11 support for TaskbarX.
//
// The Windows 11 taskbar is a XAML island inside explorer.exe, so the Win32 approach TaskbarX uses on
// Windows 10 (moving MSTaskListWClass) no longer works. Instead TaskbarX loads this DLL into explorer
// through the XAML diagnostics API (InitializeXamlDiagnosticsEx). XAML then creates our TAP object
// (CLSID_TaskbarX11Tap), hands it the IXamlDiagnostics site, and we watch the visual tree for every
// Taskbar.TaskbarFrame (one per monitor).
//
// For each taskbar frame we only apply render transforms, never layout changes, so explorer's own
// layout stays intact and everything reverts the moment the transforms are removed:
//   * the Start button (and optionally Search / Task View) is moved to the left edge,
//   * the app icons are centered on the taskbar (or between the left buttons and the tray),
//   * the Widgets button is made invisible, non interactive and moved out of the taskbar's bounds.
//
// With -w11sm TaskbarX also loads the DLL into the Start menu's app (StartMenuExperienceHost.exe), where it
// gives the Start menu background the taskbar style (see the Start menu region).
//
// Settings are read from HKCU\Software\TaskbarX\Win11 and re-read whenever TaskbarX signals the
// Local\TaskbarX11Hook_Reload event.

#include <windows.h>
#include <unknwn.h>
#include <inspectable.h>
#include <xamlOM.h>
#undef GetCurrentTime // clashes with Storyboard::GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxa = winrt::Windows::UI::Xaml::Media::Animation;
using winrt::Windows::System::DispatcherQueue;
using winrt::Windows::System::DispatcherQueuePriority;

// {7C1B5E2A-4F3D-4B8E-9A61-2D5C8E0F3B17} - must match TaskbarX's Taskbar11.vb.
static constexpr CLSID CLSID_TaskbarX11Tap = { 0x7c1b5e2a, 0x4f3d, 0x4b8e, { 0x9a, 0x61, 0x2d, 0x5c, 0x8e, 0x0f, 0x3b, 0x17 } };

static constexpr wchar_t kSettingsKey[] = L"Software\\TaskbarX\\Win11";
static constexpr wchar_t kReloadEventName[] = L"Local\\TaskbarX11Hook_Reload";

#pragma region Settings and logging

struct HookSettings
{
    bool active = true;           // false = revert everything (TaskbarX -stop)
    bool startOnLeft = true;      // move the Start button to the left edge
    bool keepButtonsLeft = true;  // keep Search / Task View next to Start, like TaskbarX on Windows 10
    bool centerIcons = true;      // center the app icons
    bool centerInBetween = false; // center between the left buttons and the tray instead of the screen
    bool hideWidgets = true;      // hide the Widgets button
    int offset = 0;               // extra horizontal offset for the centered icons, in physical pixels
    int startPadding = 6;         // distance between the left edge and the Start button, in pixels
    int taskbarStyle = 0;         // TaskbarX -tbs; non-zero = clear the XAML background so the accent shows
    DWORD styleColor = 0x80000000; // TaskbarX -color as ARGB
    bool startMenuStyle = false;  // also apply the taskbar style to the Start menu
    int animationSpeed = 300;     // ms
    std::wstring animationStyle = L"cubiceaseinout";
    bool debug = false;           // log to %TEMP%\TaskbarX11Hook.log
};

static std::mutex g_settingsLock;
static HookSettings g_settings;

static HookSettings GetSettings()
{
    std::lock_guard lock(g_settingsLock);
    return g_settings;
}

static DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
    {
        return value;
    }
    return fallback;
}

static void LoadSettings()
{
    HookSettings s;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        s.active = ReadDword(key, L"Active", 1) != 0;
        s.startOnLeft = ReadDword(key, L"StartOnLeft", 1) != 0;
        s.keepButtonsLeft = ReadDword(key, L"KeepButtonsLeft", 1) != 0;
        s.centerIcons = ReadDword(key, L"CenterIcons", 1) != 0;
        s.centerInBetween = ReadDword(key, L"CenterInBetween", 0) != 0;
        s.hideWidgets = ReadDword(key, L"HideWidgets", 1) != 0;
        s.offset = static_cast<int>(ReadDword(key, L"Offset", 0));
        s.startPadding = static_cast<int>(ReadDword(key, L"StartPadding", 6));
        s.taskbarStyle = static_cast<int>(ReadDword(key, L"TaskbarStyle", 0));
        s.styleColor = ReadDword(key, L"StyleColor", 0x80000000);
        s.startMenuStyle = ReadDword(key, L"StartMenuStyle", 0) != 0;
        s.animationSpeed = static_cast<int>(ReadDword(key, L"AnimationSpeed", 300));
        s.debug = ReadDword(key, L"Debug", 0) != 0;

        wchar_t style[64] = {};
        DWORD size = sizeof(style);
        if (RegGetValueW(key, nullptr, L"AnimationStyle", RRF_RT_REG_SZ, nullptr, style, &size) == ERROR_SUCCESS)
        {
            s.animationStyle = style;
        }
        RegCloseKey(key);
    }

    std::lock_guard lock(g_settingsLock);
    g_settings = std::move(s);
}

static void Log(const wchar_t* format, ...)
{
    if (!GetSettings().debug)
    {
        return;
    }

    wchar_t message[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);
    OutputDebugStringW(message);

    static std::mutex fileLock;
    std::lock_guard lock(fileLock);
    wchar_t path[MAX_PATH];
    if (GetTempPathW(MAX_PATH, path) && wcscat_s(path, L"TaskbarX11Hook.log") == 0)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"a, ccs=UTF-8") == 0 && file)
        {
            fwprintf(file, L"%s\n", message);
            fclose(file);
        }
    }
}

#pragma endregion

#pragma region Visual tree helpers

static std::wstring AutomationIdOf(wux::DependencyObject const& element)
{
    return std::wstring(wux::Automation::AutomationProperties::GetAutomationId(element));
}

static std::wstring ClassNameOf(wf::IInspectable const& element)
{
    return std::wstring(winrt::get_class_name(element));
}

// Breadth-first search below root for the first FrameworkElement matching pred.
template <typename Pred>
static wux::FrameworkElement FindDescendant(wux::DependencyObject const& root, Pred&& pred, int maxDepth = 32)
{
    std::vector<wux::DependencyObject> level{ root };
    for (int depth = 0; depth < maxDepth && !level.empty(); depth++)
    {
        std::vector<wux::DependencyObject> next;
        for (auto const& parent : level)
        {
            const int count = wuxm::VisualTreeHelper::GetChildrenCount(parent);
            for (int i = 0; i < count; i++)
            {
                auto child = wuxm::VisualTreeHelper::GetChild(parent, i);
                if (auto fe = child.try_as<wux::FrameworkElement>(); fe && pred(fe))
                {
                    return fe;
                }
                next.push_back(child);
            }
        }
        level = std::move(next);
    }
    return nullptr;
}

// The id of a taskbar item: the item's own AutomationId or, for wrapper elements, its first descendant's.
static std::wstring ItemIdOf(wux::FrameworkElement const& item)
{
    auto id = AutomationIdOf(item);
    if (id.empty())
    {
        if (auto inner = FindDescendant(item, [](auto const& fe) { return !AutomationIdOf(fe).empty(); }, 3))
        {
            id = AutomationIdOf(inner);
        }
    }
    return id;
}

static void DumpTree(wux::DependencyObject const& element, int depth = 0)
{
    if (depth > 14)
    {
        return;
    }
    if (auto fe = element.try_as<wux::FrameworkElement>())
    {
        auto offset = fe.ActualOffset();
        Log(L"%*s%s name='%s' id='%s' x=%.1f w=%.1f", depth * 2, L"", ClassNameOf(fe).c_str(),
            std::wstring(fe.Name()).c_str(), AutomationIdOf(fe).c_str(), offset.x, fe.ActualWidth());
    }
    const int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; i++)
    {
        DumpTree(wuxm::VisualTreeHelper::GetChild(element, i), depth + 1);
    }
}

#pragma endregion

#pragma region Element shifting

static wuxa::EasingFunctionBase MakeEasing(std::wstring style)
{
    std::transform(style.begin(), style.end(), style.begin(), ::towlower);

    auto mode = wuxa::EasingMode::EaseInOut;
    if (style.ends_with(L"easeinout"))
    {
        style.resize(style.size() - 9);
    }
    else if (style.ends_with(L"easeout"))
    {
        mode = wuxa::EasingMode::EaseOut;
        style.resize(style.size() - 7);
    }
    else if (style.ends_with(L"easein"))
    {
        mode = wuxa::EasingMode::EaseIn;
        style.resize(style.size() - 6);
    }

    wuxa::EasingFunctionBase easing{ nullptr };
    if (style == L"quad") easing = wuxa::QuadraticEase();
    else if (style == L"cubic") easing = wuxa::CubicEase();
    else if (style == L"quart") easing = wuxa::QuarticEase();
    else if (style == L"quint") easing = wuxa::QuinticEase();
    else if (style == L"sine") easing = wuxa::SineEase();
    else if (style == L"expo") easing = wuxa::ExponentialEase();
    else if (style == L"circ") easing = wuxa::CircleEase();
    else if (style == L"back") easing = wuxa::BackEase();
    else if (style == L"elastic") easing = wuxa::ElasticEase();
    else if (style == L"bounce") easing = wuxa::BounceEase();
    else return nullptr; // linear

    easing.EasingMode(mode);
    return easing;
}

// The local value a property had before we touched it (nullptr when it had none), so that turning
// TaskbarX off puts back exactly what explorer had instead of a value of our own.
static wf::IInspectable LocalValueOf(wux::DependencyObject const& object, wux::DependencyProperty const& property)
{
    auto value = object.ReadLocalValue(property);
    return value == wux::DependencyProperty::UnsetValue() ? nullptr : value;
}

static void RestoreLocalValue(wux::DependencyObject const& object, wux::DependencyProperty const& property, wf::IInspectable const& value)
{
    if (value)
    {
        object.SetValue(property, value);
    }
    else
    {
        object.ClearValue(property);
    }
}

// Interface pointers differ per projected type (UIElement vs FrameworkElement), the IUnknown doesn't.
static void* IdentityOf(wf::IInspectable const& object)
{
    return object ? winrt::get_abi(object.as<winrt::Windows::Foundation::IUnknown>()) : nullptr;
}

struct ShiftState
{
    winrt::weak_ref<wux::UIElement> element;
    wuxm::TranslateTransform transform{ nullptr };
    wuxm::Transform applied{ nullptr };            // what we set as RenderTransform (ours, or a group with ours)
    wf::IInspectable originalTransform{ nullptr }; // the RenderTransform local value before that
    wuxa::Storyboard storyboard{ nullptr };
    double target = 0;
};

// Keyed by the element's COM identity; the weak ref detects an address reused by a new element.
// Each frame has its own map, only touched on that frame's UI thread.
using ShiftMap = std::map<void*, ShiftState>;

static ShiftState* FindShiftState(ShiftMap& shifts, wux::UIElement const& element)
{
    auto it = shifts.find(IdentityOf(element));
    if (it == shifts.end() || !it->second.transform)
    {
        return nullptr;
    }
    auto tracked = it->second.element.get();
    return tracked && IdentityOf(tracked) == it->first ? &it->second : nullptr;
}

static ShiftState& ShiftStateFor(ShiftMap& shifts, wux::UIElement const& element)
{
    if (auto existingState = FindShiftState(shifts, element))
    {
        return *existingState;
    }

    auto& state = shifts[IdentityOf(element)];
    state = {};
    state.element = winrt::make_weak(element);
    state.transform = wuxm::TranslateTransform();
    state.originalTransform = LocalValueOf(element, wux::UIElement::RenderTransformProperty());

    auto existing = element.RenderTransform();
    bool isIdentity = !existing;
    if (auto matrix = existing.try_as<wuxm::MatrixTransform>())
    {
        const auto m = matrix.Matrix();
        isIdentity = m.M11 == 1 && m.M12 == 0 && m.M21 == 0 && m.M22 == 1 && m.OffsetX == 0 && m.OffsetY == 0;
    }
    if (!isIdentity)
    {
        // Keep whatever transform explorer set and add ours on top of it.
        wuxm::TransformGroup group;
        group.Children().Append(existing);
        group.Children().Append(state.transform);
        state.applied = group;
    }
    else
    {
        state.applied = state.transform;
    }
    element.RenderTransform(state.applied);
    return state;
}

// Takes our transform off the element again and puts back the RenderTransform it had before.
static void RestoreShift(ShiftState& state)
{
    if (state.storyboard)
    {
        state.storyboard.Stop();
        state.storyboard = nullptr;
    }
    auto element = state.element.get();
    if (element && IdentityOf(element.RenderTransform()) == IdentityOf(state.applied))
    {
        RestoreLocalValue(element, wux::UIElement::RenderTransformProperty(), state.originalTransform);
    }
    state = {};
}

static void ShiftElement(ShiftMap& shifts, wux::UIElement const& element, double x, HookSettings const& settings)
{
    if (x == 0 && !FindShiftState(shifts, element))
    {
        return; // never touched and nothing to do
    }

    auto& state = ShiftStateFor(shifts, element);
    if (std::abs(state.target - x) < 0.5)
    {
        return;
    }
    state.target = x;

    auto style = settings.animationStyle;
    std::transform(style.begin(), style.end(), style.begin(), ::towlower);
    if (settings.animationSpeed <= 1 || style == L"none")
    {
        if (state.storyboard)
        {
            state.storyboard.Stop();
            state.storyboard = nullptr;
        }
        state.transform.X(x);
        return;
    }

    // No From value: XAML hands off from the current (possibly animated) value, so re-targeting
    // mid-animation is smooth.
    wuxa::DoubleAnimation animation;
    animation.To(x);
    animation.Duration(wux::DurationHelper::FromTimeSpan(std::chrono::milliseconds(settings.animationSpeed)));
    animation.EasingFunction(MakeEasing(settings.animationStyle));
    wuxa::Storyboard::SetTarget(animation, state.transform);
    wuxa::Storyboard::SetTargetProperty(animation, L"X");

    wuxa::Storyboard storyboard;
    storyboard.Children().Append(animation);
    storyboard.Begin();
    state.storyboard = storyboard;
}

#pragma endregion

#pragma region Taskbar layout

struct WidgetsState
{
    winrt::weak_ref<wux::FrameworkElement> element;
    bool hidden = false;
    // Local values from before we hid it.
    wf::IInspectable opacity{ nullptr };
    wf::IInspectable isHitTestVisible{ nullptr };
    wf::IInspectable isTabStop{ nullptr };
    int64_t opacityCallback = 0;
};

// A taskbar background part we made invisible, with the Opacity local value it had before.
struct BackgroundPart
{
    winrt::weak_ref<wux::UIElement> element;
    wf::IInspectable opacity{ nullptr };
};

struct FrameState
{
    winrt::weak_ref<wux::FrameworkElement> frame;
    DispatcherQueue dispatcher{ nullptr };
    winrt::event_token layoutUpdated{};
    bool listening = false; // subscribed to LayoutUpdated
    ShiftMap shifts;
    WidgetsState widgets;
    std::vector<BackgroundPart> background;
    std::atomic<bool> pending = false;
    bool dumped = false;
};

static std::mutex g_framesLock;
static std::map<InstanceHandle, std::shared_ptr<FrameState>> g_frames;

static void ScheduleRelayout(std::shared_ptr<FrameState> const& state);

static void ShowWidgets(FrameState& state)
{
    auto& widgets = state.widgets;
    if (!widgets.hidden)
    {
        return;
    }
    widgets.hidden = false;

    if (auto fe = widgets.element.get())
    {
        fe.UnregisterPropertyChangedCallback(wux::UIElement::OpacityProperty(), widgets.opacityCallback);
        RestoreLocalValue(fe, wux::UIElement::OpacityProperty(), widgets.opacity);
        RestoreLocalValue(fe, wux::UIElement::IsHitTestVisibleProperty(), widgets.isHitTestVisible);
        if (fe.try_as<wux::Controls::Control>())
        {
            RestoreLocalValue(fe, wux::Controls::Control::IsTabStopProperty(), widgets.isTabStop);
        }
        if (auto shift = FindShiftState(state.shifts, fe))
        {
            RestoreShift(*shift);
            state.shifts.erase(IdentityOf(fe));
        }
    }
    widgets = {};
}

// Makes the Widgets button invisible, unreachable by mouse and keyboard (so hovering can't open the
// widgets board) and moves it below the taskbar, outside the island's bounds.
static void HideWidgets(std::shared_ptr<FrameState> const& state, wux::FrameworkElement const& fe)
{
    auto& widgets = state->widgets;
    if (!widgets.hidden || IdentityOf(widgets.element.get()) != IdentityOf(fe))
    {
        ShowWidgets(*state);
        widgets.element = winrt::make_weak(fe);
        widgets.opacity = LocalValueOf(fe, wux::UIElement::OpacityProperty());
        widgets.isHitTestVisible = LocalValueOf(fe, wux::UIElement::IsHitTestVisibleProperty());
        if (fe.try_as<wux::Controls::Control>())
        {
            widgets.isTabStop = LocalValueOf(fe, wux::Controls::Control::IsTabStopProperty());
        }
        widgets.hidden = true;

        // Explorer may fade the button back in without a layout pass; hide it again when that happens.
        std::weak_ptr<FrameState> weak = state;
        widgets.opacityCallback = fe.RegisterPropertyChangedCallback(
            wux::UIElement::OpacityProperty(), [weak](wux::DependencyObject const& sender, wux::DependencyProperty const&) {
                auto s = weak.lock();
                auto element = sender.try_as<wux::UIElement>();
                if (s && s->widgets.hidden && element && element.Opacity() != 0)
                {
                    element.Opacity(0);
                }
            });
    }

    if (fe.Opacity() != 0)
    {
        fe.Opacity(0);
    }
    if (fe.IsHitTestVisible())
    {
        fe.IsHitTestVisible(false);
    }
    if (auto control = fe.try_as<wux::Controls::Control>(); control && control.IsTabStop())
    {
        control.IsTabStop(false);
    }
    auto& shift = ShiftStateFor(state->shifts, fe);
    if (shift.transform.Y() != 1000.0)
    {
        shift.transform.Y(1000.0);
    }
}

static void RestoreBackground(FrameState& state)
{
    for (auto const& part : state.background)
    {
        if (auto element = part.element.get())
        {
            RestoreLocalValue(element, wux::UIElement::OpacityProperty(), part.opacity);
        }
    }
    state.background.clear();
}

// TaskbarX's transparent / blur / acrylic styles set an accent on the Shell_TrayWnd window, like on
// Windows 10. Windows 11 paints an opaque XAML background (BackgroundFill, plus the BackgroundStroke
// line along the top) over that window, so make those invisible to let the accent show through.
static void ClearBackground(FrameState& state, wux::FrameworkElement const& frame)
{
    const bool applied = !state.background.empty() &&
        std::all_of(state.background.begin(), state.background.end(), [](auto const& part) { return part.element.get() != nullptr; });
    if (!applied)
    {
        RestoreBackground(state);
        for (auto name : { L"BackgroundFill", L"BackgroundStroke" })
        {
            if (auto part = FindDescendant(frame, [name](auto const& fe) { return fe.Name() == name; }, 6))
            {
                state.background.push_back({ winrt::make_weak(part.as<wux::UIElement>()), LocalValueOf(part, wux::UIElement::OpacityProperty()) });
            }
        }
    }
    for (auto const& part : state.background)
    {
        if (auto element = part.element.get(); element && element.Opacity() != 0)
        {
            element.Opacity(0);
        }
    }
}

// Turning TaskbarX off: every value we changed goes back to what explorer had, and we stop listening
// for layout changes, so the taskbar is left exactly as Windows draws it.
static void RevertFrame(FrameState& state)
{
    ShowWidgets(state);
    RestoreBackground(state);
    for (auto& [identity, shift] : state.shifts)
    {
        RestoreShift(shift);
    }
    state.shifts.clear();

    if (state.listening)
    {
        if (auto frame = state.frame.get())
        {
            frame.LayoutUpdated(state.layoutUpdated);
        }
        state.listening = false;
    }
}

struct Item
{
    wux::FrameworkElement element{ nullptr };
    double x = 0; // left edge in frame coordinates, without our transform
    double width = 0;
    std::wstring id;
    bool isTaskListButton = false;
};

// Left edge of the notification area in frame coordinates (DIPs), or -1 if unknown. The tray is its own
// window (TrayNotifyWnd) rather than part of the frame, and only the primary taskbar has one.
static double TrayLeftInFrame(double frameWidth, double scale)
{
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT taskbarRect{};
    RECT trayRect{};
    if (!tray || !GetWindowRect(taskbar, &taskbarRect) || !GetWindowRect(tray, &trayRect))
    {
        return -1;
    }
    if (std::abs((taskbarRect.right - taskbarRect.left) - frameWidth * scale) > 2)
    {
        return -1; // this frame is a secondary taskbar
    }
    return (trayRect.left - taskbarRect.left) / scale;
}

static void Relayout(std::shared_ptr<FrameState> const& statePtr)
{
    auto& state = *statePtr;
    auto frame = state.frame.get();
    if (!frame)
    {
        return;
    }

    const auto settings = GetSettings();
    if (!settings.active)
    {
        RevertFrame(state);
        return;
    }

    if (!state.listening)
    {
        // LayoutUpdated fires after every layout pass on this thread: windows opening and closing,
        // pinning, resolution and DPI changes. Our transforms don't cause layout passes themselves.
        std::weak_ptr<FrameState> weak = statePtr;
        state.layoutUpdated = frame.LayoutUpdated([weak](auto&&, auto&&) {
            if (auto s = weak.lock())
            {
                ScheduleRelayout(s);
            }
        });
        state.listening = true;
    }

    const double frameWidth = frame.ActualWidth();
    if (frameWidth <= 0)
    {
        return;
    }

    if (settings.debug && !state.dumped)
    {
        state.dumped = true;
        Log(L"--- Taskbar.TaskbarFrame tree ---");
        DumpTree(frame);
    }

    double scale = 1.0;
    if (auto root = frame.XamlRoot())
    {
        scale = root.RasterizationScale();
    }

    if (settings.taskbarStyle != 0)
    {
        ClearBackground(state, frame);
    }
    else
    {
        RestoreBackground(state);
    }

    // The Widgets button may live outside the icon repeater, so search the whole frame.
    if (settings.hideWidgets)
    {
        auto widgets = state.widgets.element.get();
        if (!widgets)
        {
            widgets = FindDescendant(frame, [](auto const& fe) { return AutomationIdOf(fe) == L"WidgetsButton"; });
        }
        if (widgets)
        {
            HideWidgets(statePtr, widgets);
        }
    }
    else
    {
        ShowWidgets(state);
    }

    auto repeater = FindDescendant(frame, [](auto const& fe) { return fe.Name() == L"TaskbarFrameRepeater"; });
    if (!repeater)
    {
        if (auto start = FindDescendant(frame, [](auto const& fe) { return AutomationIdOf(fe) == L"StartButton"; }))
        {
            repeater = wuxm::VisualTreeHelper::GetParent(start).try_as<wux::FrameworkElement>();
        }
    }
    if (!repeater)
    {
        return;
    }

    const auto origin = repeater.TransformToVisual(frame).TransformPoint({ 0, 0 });

    std::vector<Item> items;
    const int count = wuxm::VisualTreeHelper::GetChildrenCount(repeater);
    for (int i = 0; i < count; i++)
    {
        auto fe = wuxm::VisualTreeHelper::GetChild(repeater, i).try_as<wux::FrameworkElement>();
        if (!fe || fe.Visibility() != wux::Visibility::Visible || fe.ActualWidth() <= 0)
        {
            continue;
        }
        const auto offset = fe.ActualOffset();
        if (offset.x < -1000 || offset.y < -1000)
        {
            continue; // recycled by the ItemsRepeater, parked off-screen
        }

        Item item;
        item.element = fe;
        item.x = origin.X + offset.x;
        item.width = fe.ActualWidth();
        item.id = ItemIdOf(fe);
        item.isTaskListButton = ClassNameOf(fe) == L"Taskbar.TaskListButton";

        if (item.id == L"WidgetsButton")
        {
            continue; // never part of the layout; hidden or left alone above
        }
        items.push_back(std::move(item));
    }
    if (items.empty())
    {
        return;
    }
    std::sort(items.begin(), items.end(), [](auto const& a, auto const& b) { return a.x < b.x; });

    // Split into the buttons that go to the left edge and the icons that get centered.
    size_t leftCount = 0;
    if (settings.startOnLeft)
    {
        auto start = std::find_if(items.begin(), items.end(), [](auto const& item) { return item.id == L"StartButton"; });
        if (start == items.begin())
        {
            leftCount = 1;
            if (settings.keepButtonsLeft)
            {
                while (leftCount < items.size() && !items[leftCount].isTaskListButton)
                {
                    leftCount++;
                }
            }
        }
    }

    double leftDelta = 0;
    double leftEnd = 0;
    if (leftCount > 0)
    {
        // With Windows' own left alignment (TaskbarX sets it) the buttons are already on the left. Leave them
        // there: Windows opens the Start menu, the search box and its popup at their real positions.
        const bool alreadyLeft = items[0].x < frameWidth / 4;
        leftDelta = alreadyLeft ? 0 : settings.startPadding / scale - items[0].x;
        leftEnd = items[leftCount - 1].x + items[leftCount - 1].width + leftDelta;
    }

    double centerDelta = 0;
    if (leftCount < items.size())
    {
        const double first = items[leftCount].x;
        const double last = items.back().x + items.back().width;

        if (settings.centerIcons)
        {
            double rightLimit = frameWidth;
            if (settings.centerInBetween)
            {
                const double trayLeft = TrayLeftInFrame(frameWidth, scale);
                if (trayLeft > leftEnd && trayLeft < frameWidth)
                {
                    rightLimit = trayLeft;
                }
            }
            const double center = settings.centerInBetween ? (leftEnd + rightLimit) / 2 : frameWidth / 2;
            centerDelta = center + settings.offset / scale - (first + last) / 2;
        }

        // Never slide the icons under the buttons on the left.
        if (leftCount > 0)
        {
            centerDelta = std::max(centerDelta, leftEnd - first);
        }
    }

    for (size_t i = 0; i < items.size(); i++)
    {
        ShiftElement(state.shifts, items[i].element, i < leftCount ? leftDelta : centerDelta, settings);
    }
}

static void ScheduleRelayout(std::shared_ptr<FrameState> const& state)
{
    if (!state->dispatcher || state->pending.exchange(true))
    {
        return; // already queued; one pass handles every change made in the meantime
    }
    state->dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [state] {
        state->pending = false;
        try
        {
            Relayout(state);
        }
        catch (winrt::hresult_error const& e)
        {
            Log(L"Relayout failed: 0x%08X %s", static_cast<unsigned>(e.code()), e.message().c_str());
        }
    });
}

static void RegisterFrame(InstanceHandle handle, wux::FrameworkElement const& frame)
{
    auto state = std::make_shared<FrameState>();
    state->frame = winrt::make_weak(frame);
    state->dispatcher = DispatcherQueue::GetForCurrentThread();

    {
        std::lock_guard lock(g_framesLock);
        g_frames[handle] = state;
    }
    Log(L"Taskbar frame added (%llu), width %.1f", static_cast<unsigned long long>(handle), frame.ActualWidth());
    ScheduleRelayout(state);
}

// Called on the frame's UI thread when it leaves the tree (monitor unplugged, explorer shutting down).
static void UnregisterFrame(InstanceHandle handle)
{
    std::shared_ptr<FrameState> state;
    {
        std::lock_guard lock(g_framesLock);
        auto it = g_frames.find(handle);
        if (it == g_frames.end())
        {
            return;
        }
        state = it->second;
        g_frames.erase(it);
    }
    RevertFrame(*state);
    Log(L"Taskbar frame removed (%llu)", static_cast<unsigned long long>(handle));
}

static void RelayoutAllFrames()
{
    std::vector<std::shared_ptr<FrameState>> frames;
    {
        std::lock_guard lock(g_framesLock);
        for (auto const& [handle, state] : g_frames)
        {
            frames.push_back(state);
        }
    }
    for (auto const& state : frames)
    {
        ScheduleRelayout(state);
    }
}

static DWORD WINAPI ReloadThread(LPVOID param)
{
    const HANDLE reloadEvent = static_cast<HANDLE>(param);
    while (WaitForSingleObject(reloadEvent, INFINITE) == WAIT_OBJECT_0)
    {
        LoadSettings();
        Log(L"Settings reloaded");
        RelayoutAllFrames();
    }
    return 0;
}

#pragma endregion

#pragma region Start menu

// The Windows 11 Start menu runs in its own app process (StartMenuExperienceHost.exe), so the same DLL is
// loaded there too. In that process it only restyles the Start menu's background.
static bool g_isStartMenu = false;
static HWND g_startWindow = nullptr;

// The Start menu's background is the Border "AcrylicBorder" (its acrylic brush), with the Border
// "AcrylicOverlay" adding a tint on top. Both are only touched on the Start menu's UI thread.
struct StartMenuPart
{
    winrt::weak_ref<wux::Controls::Border> border;
    bool isOverlay = false;
    bool styled = false;
    wf::IInspectable original{ nullptr }; // local Background (or Opacity for the overlay) from before
};

static std::vector<StartMenuPart> g_startMenuParts;
static bool g_startMenuVisibilityHooked = false;

// The taskbar style (-tbs) and color (-color) applied to the Start menu background.
static wuxm::Brush MakeStartMenuBrush(HookSettings const& settings)
{
    const auto argb = settings.styleColor;
    const winrt::Windows::UI::Color color{ static_cast<uint8_t>(argb >> 24), static_cast<uint8_t>(argb >> 16),
                                           static_cast<uint8_t>(argb >> 8), static_cast<uint8_t>(argb) };
    auto opaque = color;
    opaque.A = 255;

    switch (settings.taskbarStyle)
    {
    case 2: // blur
    case 3: // acrylic
    {
        // Backdrop, like the Start menu's own acrylic: HostBackdrop doesn't render in this window.
        wuxm::AcrylicBrush acrylic;
        acrylic.BackgroundSource(wuxm::AcrylicBackgroundSource::Backdrop);
        acrylic.TintColor(opaque);
        acrylic.FallbackColor(color);
        const double tint = settings.taskbarStyle == 2 ? 0.0 : color.A / 255.0;
        acrylic.TintOpacity(tint);
        acrylic.TintLuminosityOpacity(winrt::box_value(tint).as<wf::IReference<double>>());
        return acrylic;
    }
    case 5: // opaque
        return wuxm::SolidColorBrush(opaque);
    default: // transparent, transparent gradient
        return wuxm::SolidColorBrush(color);
    }
}

static void ApplyStartMenuStyle()
{
    const auto settings = GetSettings();
    const bool on = settings.active && settings.startMenuStyle && settings.taskbarStyle != 0;
    const auto brush = on ? MakeStartMenuBrush(settings) : nullptr;

    std::erase_if(g_startMenuParts, [](auto const& part) { return !part.border.get(); });
    for (auto& part : g_startMenuParts)
    {
        auto border = part.border.get();
        const auto property = part.isOverlay ? wux::UIElement::OpacityProperty() : wux::Controls::Border::BackgroundProperty();
        if (on)
        {
            if (!part.styled)
            {
                part.original = LocalValueOf(border, property);
                part.styled = true;
            }
            if (part.isOverlay)
            {
                border.Opacity(0);
            }
            else
            {
                border.Background(brush);
            }
        }
        else if (part.styled)
        {
            RestoreLocalValue(border, property, part.original);
            part.styled = false;
            part.original = nullptr;
        }
    }
}

static void OnStartMenuTreeChange(IXamlDiagnostics* diagnostics, VisualElement const& element, VisualMutationType mutationType)
{
    if (mutationType != Add || !element.Name)
    {
        return;
    }
    const bool isBackground = wcscmp(element.Name, L"AcrylicBorder") == 0;
    const bool isOverlay = wcscmp(element.Name, L"AcrylicOverlay") == 0;
    if (!isBackground && !isOverlay)
    {
        return;
    }

    wf::IInspectable inspectable;
    winrt::check_hresult(diagnostics->GetIInspectableFromHandle(element.Handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(inspectable))));
    auto border = inspectable.try_as<wux::Controls::Border>();
    if (!border)
    {
        return;
    }
    Log(L"Start menu %s found", element.Name);
    g_startMenuParts.push_back({ winrt::make_weak(border), isOverlay });

    if (!g_startMenuVisibilityHooked)
    {
        // TaskbarX can't signal this process, so pick up setting changes (including turning off) every
        // time the Start menu opens.
        if (auto window = wux::Window::Current())
        {
            window.VisibilityChanged([](auto&&, winrt::Windows::UI::Core::VisibilityChangedEventArgs const& args) {
                if (args.Visible())
                {
                    LoadSettings();
                    ApplyStartMenuStyle();
                }
            });
            g_startMenuVisibilityHooked = true;
        }
    }
    ApplyStartMenuStyle();
}

#pragma endregion

#pragma region XAML diagnostics (TAP)

struct VisualTreeWatcher : winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2, winrt::non_agile>
{
    explicit VisualTreeWatcher(winrt::com_ptr<IXamlDiagnostics> diagnostics) : m_diagnostics(std::move(diagnostics))
    {
    }

    void Advise()
    {
        // AdviseVisualTreeChange hangs when called on the thread that called SetSite, so call it from
        // a new thread. It then replays the existing tree as "Add" mutations.
        auto self = get_strong();
        HANDLE thread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
            winrt::com_ptr<VisualTreeWatcher> watcher;
            watcher.attach(static_cast<VisualTreeWatcher*>(param));
            const HRESULT hr = watcher->m_diagnostics.as<IVisualTreeService3>()->AdviseVisualTreeChange(watcher.get());
            Log(L"AdviseVisualTreeChange: 0x%08X", static_cast<unsigned>(hr));
            return 0;
        }, self.detach(), 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
        }
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element, VisualMutationType mutationType) override
    {
        try
        {
            if (g_isStartMenu)
            {
                OnStartMenuTreeChange(m_diagnostics.get(), element, mutationType);
            }
            else if (mutationType == Add)
            {
                if (element.Type && wcscmp(element.Type, L"Taskbar.TaskbarFrame") == 0)
                {
                    wf::IInspectable inspectable;
                    winrt::check_hresult(m_diagnostics->GetIInspectableFromHandle(
                        element.Handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(inspectable))));
                    if (auto frame = inspectable.try_as<wux::FrameworkElement>())
                    {
                        RegisterFrame(element.Handle, frame);
                    }
                }
            }
            else if (mutationType == Remove)
            {
                UnregisterFrame(element.Handle);
            }
        }
        catch (...)
        {
            Log(L"OnVisualTreeChange failed: 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) override
    {
        return S_OK;
    }

private:
    winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
};

static std::mutex g_watcherLock;
static winrt::com_ptr<VisualTreeWatcher> g_watcher;

static void StartReloadListener()
{
    static std::once_flag once;
    std::call_once(once, [] {
        // TaskbarX checks for this mutex to know the hook is already loaded in this explorer.exe.
        const std::wstring activeName = L"Local\\TaskbarX11Hook_Active_" + std::to_wstring(GetCurrentProcessId());
        CreateMutexW(nullptr, FALSE, activeName.c_str());

        if (HANDLE reloadEvent = CreateEventW(nullptr, FALSE, FALSE, kReloadEventName))
        {
            if (HANDLE thread = CreateThread(nullptr, 0, ReloadThread, reloadEvent, 0, nullptr))
            {
                CloseHandle(thread);
            }
        }
    });
}

struct TaskbarX11Tap : winrt::implements<TaskbarX11Tap, IObjectWithSite, winrt::non_agile>
{
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override
    {
        try
        {
            LoadSettings();
            Log(L"SetSite (pid %lu%s)", GetCurrentProcessId(), g_isStartMenu ? L", Start menu" : L", explorer");

            m_site.copy_from(site);
            std::lock_guard lock(g_watcherLock);
            g_watcher = nullptr; // only one watcher at a time; a second injection replaces the first
            if (m_site)
            {
                if (g_isStartMenu)
                {
                    // TaskbarX checks this window property to know the hook is loaded in the Start menu.
                    SetPropW(g_startWindow, L"TaskbarX11Hook", reinterpret_cast<HANDLE>(1));
                }
                else
                {
                    StartReloadListener();
                }
                g_watcher = winrt::make_self<VisualTreeWatcher>(m_site.as<IXamlDiagnostics>());
                g_watcher->Advise();
            }
            return S_OK;
        }
        catch (...)
        {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** site) override
    {
        if (!m_site)
        {
            *site = nullptr;
            return E_FAIL;
        }
        return m_site->QueryInterface(riid, site);
    }

private:
    winrt::com_ptr<IUnknown> m_site;
};

struct TapFactory : winrt::implements<TapFactory, IClassFactory>
{
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override
    {
        if (outer)
        {
            return CLASS_E_NOAGGREGATION;
        }
        try
        {
            return winrt::make_self<TaskbarX11Tap>()->QueryInterface(riid, object);
        }
        catch (...)
        {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override
    {
        return S_OK;
    }
};

#pragma endregion

#pragma region Injection

static HMODULE g_module = nullptr;

// Runs inside explorer.exe. InitializeXamlDiagnosticsEx only finds the XAML of the calling process, so
// it can't be called from TaskbarX itself; it also pins this DLL, so TaskbarX can drop its hook after.
static DWORD WINAPI InitializeDiagnosticsThread(LPVOID)
{
    using InitializeXamlDiagnosticsEx_t = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);

    LoadSettings();
    const HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto initialize = xaml ? reinterpret_cast<InitializeXamlDiagnosticsEx_t>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
    if (!initialize)
    {
        Log(L"InitializeXamlDiagnosticsEx not found");
        return 1;
    }

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    Log(L"Connecting XAML diagnostics for %s", path);

    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 60; attempt++)
    {
        // XAML only accepts endpoints named VisualDiagConnectionN.
        hr = initialize(L"VisualDiagConnection1", GetCurrentProcessId(), L"", path, CLSID_TaskbarX11Tap, nullptr);
        Log(L"InitializeXamlDiagnosticsEx attempt %d: 0x%08X", attempt, static_cast<unsigned>(hr));
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            break;
        }
        Sleep(500); // the taskbar's XAML isn't up yet (explorer is starting)
    }
    Log(L"InitializeXamlDiagnosticsEx: 0x%08X", static_cast<unsigned>(hr));
    return 0;
}

static void StartInjection(HWND window)
{
    static std::once_flag once;
    std::call_once(once, [window] {
        // Keep the DLL loaded after TaskbarX removes its hook; our threads and XAML callbacks live on.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&StartInjection), &pinned);

        g_startWindow = window;
        LoadSettings();
        Log(L"Inject message received in pid %lu%s", GetCurrentProcessId(), g_isStartMenu ? L" (Start menu)" : L"");
        if (HANDLE thread = CreateThread(nullptr, 0, InitializeDiagnosticsThread, nullptr, 0, nullptr))
        {
            CloseHandle(thread);
        }
    });
}

static const UINT g_injectMessage = RegisterWindowMessageW(L"TaskbarX11Hook_Inject");

// WH_CALLWNDPROC hook that TaskbarX installs on the taskbar's thread to get this DLL into explorer.exe.
// It starts the XAML diagnostics connection when TaskbarX sends the TaskbarX11Hook_Inject message.
extern "C" LRESULT CALLBACK TaskbarX11HookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const auto message = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (message->message == g_injectMessage)
        {
            StartInjection(message->hwnd);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// WH_GETMESSAGE variant for the Start menu. Windows suspends the Start menu app while it's closed, so
// TaskbarX posts the message instead of sending it; it's picked up the next time the Start menu opens.
extern "C" LRESULT CALLBACK TaskbarX11GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE)
    {
        const auto message = reinterpret_cast<const MSG*>(lParam);
        if (message->message == g_injectMessage)
        {
            StartInjection(message->hwnd);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

#pragma endregion

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object)
{
    if (clsid != CLSID_TaskbarX11Tap)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    try
    {
        return winrt::make_self<TapFactory>()->QueryInterface(riid, object);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

// The hook stays loaded for the lifetime of explorer.exe; reverting is done through the Active setting.
STDAPI DllCanUnloadNow()
{
    return S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);

        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring_view exePath(exe);
        g_isStartMenu = exePath.size() >= 27 && _wcsicmp(exe + exePath.size() - 27, L"StartMenuExperienceHost.exe") == 0;
    }
    return TRUE;
}
