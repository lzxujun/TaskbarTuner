#include "VisualTree.h"

#include "BackdropBrush.h"

#include "../shared/Log.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.h>

using namespace winrt::Windows::UI::Xaml;

namespace Media = winrt::Windows::UI::Xaml::Media;
namespace Shapes = winrt::Windows::UI::Xaml::Shapes;
namespace Controls = winrt::Windows::UI::Xaml::Controls;

namespace tuner::hook {
namespace {

struct Settings {
    int dimming = 0;        // 0..100, how much to darken the background
    int opacity = 100;      // 0..100, 100 = unchanged
    int iconOpacity = 100;  // 0..100, 100 = unchanged
    bool topLine = true;
    bool hideGrip = false;

    bool operator==(Settings const& other) const {
        return dimming == other.dimming && opacity == other.opacity &&
               iconOpacity == other.iconOpacity && topLine == other.topLine &&
               hideGrip == other.hideGrip;
    }
    bool operator!=(Settings const& other) const { return !(*this == other); }

    bool IsNeutral() const {
        return dimming == 0 && opacity == 100 && iconOpacity == 100 && topLine &&
               !hideGrip;
    }
};

// Registry values are the only input; the hook never has a second copy of the
// settings that could drift away from the host's panel.
Settings SettingsFromConfig(Config const& config) {
    Settings settings;
    // The window shows brightness as 0..100 with 100 = unchanged; the brush
    // work below is expressed as an amount of dimming, exactly like the
    // original mod's backgroundDimming.
    settings.dimming = 100 - std::clamp(config.brightness, 0, 100);
    settings.opacity = std::clamp(config.opacity, 0, 100);
    settings.iconOpacity = std::clamp(config.iconOpacity, 0, 100);
    settings.topLine = config.topLine;
    settings.hideGrip = config.hideGrip;
    return settings;
}

Settings g_settings;

struct AppearanceState {
    winrt::weak_ref<FrameworkElement> root;
    winrt::weak_ref<FrameworkElement> backgroundFill;
    double originalFillOpacity = 1.0;

    // The values this state was built from. The periodic check compares them
    // with the settings the caller wants: a slider that moved since the last
    // pass has to reach the taskbar even though every element is still alive.
    Settings applied;

    // The fill was replaced with a dimmed brush. The original is kept and put
    // back by assignment: template elements hold local values, so ClearValue
    // would erase the fill instead of reverting it. installedFillBrush is weak
    // on purpose - if Windows replaced the fill in the meantime (theme switch,
    // transparency toggle), it already restored itself and the saved brush must
    // not overwrite the newer value.
    bool backgroundFillReplaced = false;
    Media::Brush originalFillBrush{nullptr};
    winrt::weak_ref<Media::Brush> installedFillBrush;

    // The acrylic tint was dimmed in place. The brush itself is never replaced,
    // so the DWM material behind it stays exactly as Windows drew it.
    // appliedTintColor is an ownership marker on restore: if the taskbar has
    // changed the tint in the meantime (accent color change, theme switch), the
    // saved original must not overwrite the newer value.
    bool tintAdjusted = false;
    winrt::Windows::UI::Color originalTintColor{};
    winrt::Windows::UI::Color appliedTintColor{};

    bool topLineHidden = false;
    winrt::weak_ref<FrameworkElement> topLine;
    double originalTopLineOpacity = 1.0;

    bool gripHidden = false;
    winrt::weak_ref<FrameworkElement> grip;
    double originalGripOpacity = 1.0;

    std::vector<std::pair<winrt::weak_ref<FrameworkElement>, double>>
        foregroundOpacity;
};

// Every taskbar (one per monitor) gets its own entry. A unique_ptr keeps the
// addresses stable when the list grows, so a pointer handed out earlier stays
// valid.
std::vector<std::unique_ptr<AppearanceState>> g_states;

FrameworkElement FindChildByName(FrameworkElement element, PCWSTR name) {
    const int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < childrenCount; i++) {
        auto child =
            Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
        if (!child) {
            continue;
        }
        try {
            if (child.Name() == name) {
                return child;
            }
        } catch (...) {
        }
    }
    return nullptr;
}

FrameworkElement FindChildByClassName(FrameworkElement element,
                                      PCWSTR className) {
    const int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < childrenCount; i++) {
        auto child =
            Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
        if (!child) {
            continue;
        }
        try {
            if (winrt::get_class_name(child) == className) {
                return child;
            }
        } catch (...) {
        }
    }
    return nullptr;
}

template <typename T>
void EnumDescendants(FrameworkElement element, T&& enumCallback, int depth = 0) {
    constexpr int kMaxDepth = 12;
    if (depth > kMaxDepth) {
        return;
    }

    const int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (!child) {
            continue;
        }

        if (enumCallback(child)) {
            return;
        }

        EnumDescendants(child, enumCallback, depth + 1);
    }
}

FrameworkElement FindDescendantByName(FrameworkElement element, PCWSTR name) {
    FrameworkElement result{nullptr};
    EnumDescendants(element, [&](FrameworkElement child) {
        try {
            if (child.Name() == name) {
                result = child;
                return true;
            }
        } catch (...) {
        }
        return false;
    });
    return result;
}

std::wstring DescribeElement(FrameworkElement element) {
    std::wstring result;
    try {
        result = winrt::get_class_name(element).c_str();
    } catch (...) {
        result = L"<unknown>";
    }

    try {
        auto name = element.Name();
        if (!name.empty()) {
            result += L"#";
            result += name.c_str();
        }
    } catch (...) {
    }

    return result;
}

std::wstring FormatColor(winrt::Windows::UI::Color const& color) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%02X%02X%02X%02X", color.A, color.R, color.G, color.B);
    return buffer;
}

// A brush is not just "acrylic or solid": what the pixels end up looking like
// depends on TintOpacity, TintLuminosityOpacity and FallbackColor as well. The
// numbers are written to the log so a build whose material does not react to a
// tint change can be told apart from one where the element is simply invisible.
std::wstring DescribeBrush(Media::Brush const& brush) {
    if (!brush) {
        return L"<none>";
    }

    std::wstring result;
    try {
        result = winrt::get_class_name(brush).c_str();
    } catch (...) {
        result = L"<unknown>";
    }

    try {
        if (auto acrylic = brush.try_as<Media::AcrylicBrush>()) {
            wchar_t buffer[256]{};
            auto luminosity = acrylic.TintLuminosityOpacity();
            if (luminosity) {
                swprintf_s(buffer, L" tint=%s tintOpacity=%.2f lumiOpacity=%.2f",
                           FormatColor(acrylic.TintColor()).c_str(),
                           acrylic.TintOpacity(), luminosity.Value());
            } else {
                swprintf_s(buffer, L" tint=%s tintOpacity=%.2f lumiOpacity=<null>",
                           FormatColor(acrylic.TintColor()).c_str(),
                           acrylic.TintOpacity());
            }
            result += buffer;
            result += L" fallback=";
            result += FormatColor(acrylic.FallbackColor());
        } else if (auto solid = brush.try_as<Media::SolidColorBrush>()) {
            result += L" color=";
            result += FormatColor(solid.Color());
        }
    } catch (...) {
        result += L" <unreadable>";
    }

    return result;
}

std::wstring DescribeRect(FrameworkElement element, FrameworkElement content) {
    try {
        auto origin = element.TransformToVisual(content)
                          .TransformPoint(winrt::Windows::Foundation::Point{0, 0});
        wchar_t buffer[128]{};
        swprintf_s(buffer, L"(%.0f,%.0f %.0fx%.0f)", origin.X, origin.Y,
                   element.ActualWidth(), element.ActualHeight());
        return buffer;
    } catch (...) {
        return L"(?)";
    }
}

// Flyouts (hover previews, the tray overflow menu, Alt+Tab) are hosted inside
// the taskbar tree but render outside of it, so adjusting them leaks a gray
// strip above the taskbar.
bool IsFlyoutHost(FrameworkElement element) {
    try {
        if (element.try_as<Controls::Primitives::Popup>()) {
            return true;
        }
    } catch (...) {
    }

    try {
        // The hstring has to outlive the view: taking a view of a temporary
        // leaves it dangling the moment the statement ends.
        auto className = winrt::get_class_name(element);
        std::wstring_view view(className);
        return view.find(L"Switcher") != std::wstring_view::npos ||
               view.find(L"TaskbarExperienceHost") != std::wstring_view::npos ||
               view.find(L"Flyout") != std::wstring_view::npos;
    } catch (...) {
        return false;
    }
}

bool IsBackgroundRectangle(FrameworkElement element) {
    try {
        auto name = element.Name();
        return name == L"BackgroundFill" || name == L"BackgroundStroke";
    } catch (...) {
        return false;
    }
}

// System chrome that is neither icon nor text content. GripperControl is
// excluded from the icon layer because Windows renders the drag handle
// permanently; fading the layer made it surface on top of the taskbar, which is
// the bug the upstream mod fixed in v1.12.0. It is handled by its own switch.
bool IsSystemChromeElement(FrameworkElement element) {
    try {
        auto name = element.Name();
        if (name == L"ScreenEdgeStroke" || name == L"GripperControl") {
            return true;
        }
    } catch (...) {
    }
    return false;
}

bool IsPureIconSubtree(FrameworkElement element) {
    constexpr int kMaxDepth = 12;
    constexpr int kMaxElements = 400;

    std::vector<std::pair<FrameworkElement, int>> stack;
    stack.push_back({element, 0});

    int visited = 0;
    while (!stack.empty()) {
        auto [current, depth] = stack.back();
        stack.pop_back();

        if (!current || IsBackgroundRectangle(current) || IsFlyoutHost(current) ||
            depth > kMaxDepth) {
            return false;
        }

        if (++visited > kMaxElements) {
            return false;
        }

        int childrenCount = 0;
        try {
            childrenCount = Media::VisualTreeHelper::GetChildrenCount(current);
        } catch (...) {
            return false;
        }

        for (int i = 0; i < childrenCount; i++) {
            FrameworkElement child{nullptr};
            try {
                child = Media::VisualTreeHelper::GetChild(current, i)
                            .try_as<FrameworkElement>();
            } catch (...) {
            }
            if (child) {
                stack.push_back({child, depth + 1});
            }
        }
    }

    return true;
}

void CollectIconLayers(FrameworkElement element, FrameworkElement content,
                       double bandTop, double bandBottom, int depth,
                       std::vector<FrameworkElement>& elements) {
    constexpr int kMaxDepth = 12;

    if (!element || depth > kMaxDepth) {
        return;
    }

    if (IsFlyoutHost(element) || IsBackgroundRectangle(element) ||
        IsSystemChromeElement(element)) {
        return;
    }

    bool visible = true;
    try {
        visible = element.Visibility() == Visibility::Visible &&
                  element.ActualWidth() > 0 && element.ActualHeight() > 0;
    } catch (...) {
    }

    if (visible && IsPureIconSubtree(element)) {
        // The XAML island is taller than the visible taskbar: anything that
        // does not fit inside the background rectangle would leak the
        // adjustment above the taskbar.
        bool withinVisibleTaskbar = true;
        try {
            auto origin = element.TransformToVisual(content)
                              .TransformPoint(
                                  winrt::Windows::Foundation::Point{0, 0});
            withinVisibleTaskbar =
                origin.X >= -1 && origin.Y >= -1 &&
                origin.X + element.ActualWidth() <= content.ActualWidth() + 1 &&
                origin.Y + element.ActualHeight() <= content.ActualHeight() + 1;

            if (withinVisibleTaskbar && bandBottom > bandTop) {
                withinVisibleTaskbar =
                    origin.Y >= bandTop - 1 &&
                    origin.Y + element.ActualHeight() <= bandBottom + 1;
            }
        } catch (...) {
        }

        if (withinVisibleTaskbar) {
            elements.push_back(element);
            TLOG(L"icon layer: %s %s", DescribeElement(element).c_str(),
                 DescribeRect(element, content).c_str());
            return;
        }
    }

    int childrenCount = 0;
    try {
        childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
    } catch (...) {
        return;
    }

    for (int i = 0; i < childrenCount; i++) {
        FrameworkElement child{nullptr};
        try {
            child = Media::VisualTreeHelper::GetChild(element, i)
                        .try_as<FrameworkElement>();
        } catch (...) {
        }
        CollectIconLayers(child, content, bandTop, bandBottom, depth + 1,
                          elements);
    }
}

void DumpVisualTree(FrameworkElement root, int maxDepth, int maxElements) {
    int logged = 0;

    auto walk = [&](auto&& self, FrameworkElement element, int depth) -> void {
        if (!element || depth > maxDepth || logged >= maxElements) {
            return;
        }

        logged++;
        TLOG(L"%s%s", std::wstring(static_cast<size_t>(depth) * 2, L' ').c_str(),
             DescribeElement(element).c_str());

        int childrenCount = 0;
        try {
            childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
        } catch (...) {
        }

        for (int i = 0; i < childrenCount && logged < maxElements; i++) {
            FrameworkElement child{nullptr};
            try {
                child = Media::VisualTreeHelper::GetChild(element, i)
                            .try_as<FrameworkElement>();
            } catch (...) {
            }
            self(self, child, depth + 1);
        }
    };

    walk(walk, root, 0);
}

// A build whose taskbar does not expose BackgroundFill cannot be adjusted, and
// there is nothing to retry: the tree is written to the log once so that the
// element names used by that build can be compared with the ones expected here.
void DumpTreeOnce(FrameworkElement content) {
    static bool dumped = false;
    if (dumped) {
        return;
    }
    dumped = true;

    TLOG(L"BackgroundFill is missing, dumping the visual tree once");
    DumpVisualTree(content, 6, 80);
}

std::vector<FrameworkElement> CollectForegroundElements(
    FrameworkElement content, double bandTop, double bandBottom) {
    std::vector<FrameworkElement> elements;
    CollectIconLayers(content, content, bandTop, bandBottom, 0, elements);

    TLOG(L"icon layers: %d, content %.0fx%.0f, visible band %.0f..%.0f",
         static_cast<int>(elements.size()), content.ActualWidth(),
         content.ActualHeight(), bandTop, bandBottom);

    return elements;
}

std::pair<double, double> GetVisibleBand(FrameworkElement content,
                                         FrameworkElement backgroundFill) {
    double bandTop = 0.0;
    double bandBottom = content.ActualHeight();
    try {
        auto origin = backgroundFill.TransformToVisual(content)
                          .TransformPoint(winrt::Windows::Foundation::Point{0, 0});
        bandTop = origin.Y;
        bandBottom = origin.Y + backgroundFill.ActualHeight();
    } catch (...) {
    }
    return {bandTop, bandBottom};
}

BYTE ScaleColorChannel(BYTE value, float scale) {
    const int scaled = static_cast<int>(value * scale + 0.5f);
    return static_cast<BYTE>(std::clamp(scaled, 0, 255));
}

// Two projected objects are the same object when their ABI pointers match.
// Comparing the projections directly would depend on comparison operators that
// are not generated for every WinRT type.
template <typename T>
bool SameObject(T const& left, T const& right) {
    return winrt::get_abi(left) == winrt::get_abi(right);
}

bool SameColor(winrt::Windows::UI::Color const& left,
               winrt::Windows::UI::Color const& right) {
    return left.A == right.A && left.R == right.R && left.G == right.G &&
           left.B == right.B;
}

void ApplyTopLineStyle(FrameworkElement content,
                       FrameworkElement backgroundFillElem,
                       AppearanceState& state) {
    FrameworkElement topLineElem{nullptr};

    // Look among the siblings first: flyouts contain rectangles with the same
    // names, and a depth-first search would find one of those on some builds.
    if (auto parent = Media::VisualTreeHelper::GetParent(backgroundFillElem)
                          .try_as<FrameworkElement>()) {
        const int childrenCount = Media::VisualTreeHelper::GetChildrenCount(parent);
        for (int i = 0; i < childrenCount; i++) {
            auto child = Media::VisualTreeHelper::GetChild(parent, i)
                             .try_as<FrameworkElement>();
            if (child && child.Name() == L"BackgroundStroke") {
                topLineElem = child;
                break;
            }
        }
    }

    if (!topLineElem) {
        topLineElem = FindDescendantByName(content, L"BackgroundStroke");
    }

    if (!topLineElem) {
        TLOG(L"BackgroundStroke not found");
        return;
    }

    auto topLineRect = topLineElem.try_as<Shapes::Rectangle>();
    if (!topLineRect) {
        TLOG(L"BackgroundStroke is not a Rectangle");
        return;
    }

    state.topLine = winrt::make_weak(topLineRect.as<FrameworkElement>());
    state.originalTopLineOpacity = topLineRect.Opacity();

    if (!g_settings.topLine) {
        state.topLineHidden = true;
        // Opacity only: Visibility, Fill and StrokeThickness belong to the
        // taskbar template and overwriting them destroys the original look for
        // good.
        topLineRect.Opacity(0.0);
        TLOG(L"top line hidden");
    }
}

void ApplyGripStyle(FrameworkElement content, AppearanceState& state) {
    if (!g_settings.hideGrip) {
        return;
    }

    auto gripElem = FindDescendantByName(content, L"GripperControl");
    if (!gripElem) {
        TLOG(L"GripperControl not found");
        return;
    }

    try {
        state.grip = winrt::make_weak(gripElem);
        state.originalGripOpacity = gripElem.Opacity();
        state.gripHidden = true;
        gripElem.Opacity(0.0);
        TLOG(L"gripper hidden");
    } catch (...) {
    }
}

void ApplyBackgroundStyle(Shapes::Rectangle const& backgroundFill,
                          AppearanceState& state) {
    const bool hasOpacity = g_settings.opacity != 100;
    const bool hasBrightness = g_settings.dimming != 0;

    if (hasOpacity) {
        backgroundFill.Opacity(g_settings.opacity / 100.0);
    }

    if (!hasBrightness) {
        return;
    }

    const float scale = 1.0f - g_settings.dimming / 100.0f;

    // 1. Dim the acrylic tint in place. With transparency effects on, the
    //    taskbar fill is an acrylic brush and the accent color (when "Show
    //    accent color on Start and taskbar" is enabled) only changes its
    //    TintColor. Scaling the RGB channels of the tint and keeping the alpha
    //    darkens exactly the color the taskbar is showing, while the brush - and
    //    with it the DWM material behind it - is left untouched. This is the
    //    strategy the original Windhawk mod uses and the only one it applies to
    //    an acrylic fill; nothing else on the brush is written.
    if (auto acrylic = backgroundFill.Fill().try_as<Media::AcrylicBrush>()) {
        try {
            TLOG(L"background fill: %s (element opacity=%.2f, visible=%d)",
                 DescribeBrush(acrylic).c_str(), backgroundFill.Opacity(),
                 backgroundFill.Visibility() == Visibility::Visible ? 1 : 0);

            auto tint = acrylic.TintColor();
            auto dimmed = winrt::Windows::UI::Color{};
            dimmed.A = tint.A;
            dimmed.R = ScaleColorChannel(tint.R, scale);
            dimmed.G = ScaleColorChannel(tint.G, scale);
            dimmed.B = ScaleColorChannel(tint.B, scale);

            acrylic.TintColor(dimmed);
            state.tintAdjusted = true;
            state.originalTintColor = tint;
            state.appliedTintColor = dimmed;
            TLOG(L"acrylic tint dimmed %s -> %s (scale=%.2f)",
                 FormatColor(tint).c_str(), FormatColor(dimmed).c_str(), scale);
            return;
        } catch (...) {
            // Fall through to the other strategies.
        }
    }

    // 2. Dim a solid fill directly. The backdrop approach would darken the
    //    content behind the taskbar instead of the color on it.
    if (auto solid = backgroundFill.Fill().try_as<Media::SolidColorBrush>()) {
        try {
            auto color = solid.Color();
            auto dimmed = winrt::Windows::UI::Color{};
            dimmed.A = color.A;
            dimmed.R = ScaleColorChannel(color.R, scale);
            dimmed.G = ScaleColorChannel(color.G, scale);
            dimmed.B = ScaleColorChannel(color.B, scale);

            auto dimBrush = Media::SolidColorBrush();
            dimBrush.Color(dimmed);
            dimBrush.Opacity(solid.Opacity());

            state.originalFillBrush = solid;
            state.installedFillBrush = winrt::make_weak(dimBrush.as<Media::Brush>());
            backgroundFill.Fill(dimBrush);
            state.backgroundFillReplaced = true;
            TLOG(L"solid fill dimmed %s -> %s (scale=%.2f)",
                 FormatColor(color).c_str(), FormatColor(dimmed).c_str(), scale);
            return;
        } catch (...) {
        }
    }

    // 3. Replace the fill with a live backdrop brush filtered by a color matrix.
    try {
        auto compositor =
            winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(backgroundFill)
                    .Compositor();
        const float brightness = -g_settings.dimming / 100.0f;
        auto brush = CreateBackdropDimBrush(compositor, brightness);

        state.originalFillBrush = backgroundFill.Fill();
        state.installedFillBrush = winrt::make_weak(brush);
        backgroundFill.Fill(brush);
        state.backgroundFillReplaced = true;
        TLOG(L"backdrop brush installed (brightness=%.2f)", brightness);
    } catch (winrt::hresult_error const& e) {
        TLOG(L"failed to create backdrop brush: %08X",
             static_cast<unsigned>(e.code().value));
    } catch (...) {
        TLOG(L"failed to create backdrop brush (unknown error)");
    }
}

void ApplyForegroundStyle(std::vector<FrameworkElement> const& elements,
                          AppearanceState& state) {
    if (g_settings.iconOpacity == 100) {
        return;
    }

    for (auto const& element : elements) {
        try {
            state.foregroundOpacity.emplace_back(winrt::make_weak(element),
                                                 element.Opacity());
            // Multiplicative fade: an element the system hid (0) stays hidden,
            // an element the system shows (1) gets exactly the layer opacity.
            element.Opacity(element.Opacity() * (g_settings.iconOpacity / 100.0));
        } catch (...) {
        }
    }

    TLOG(L"foreground elements adjusted: %d",
         static_cast<int>(state.foregroundOpacity.size()));
}

FrameworkElement FindBackgroundFill(FrameworkElement content) {
    FrameworkElement child = content;
    if ((child = FindChildByName(child, L"RootGrid")) &&
        (child = FindChildByName(child, L"BackgroundControl")) &&
        (child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.Grid")) &&
        (child = FindChildByName(child, L"BackgroundFill"))) {
        return child;
    }

    return FindDescendantByName(content, L"BackgroundFill");
}

// Logs the fill element and its ancestors once per taskbar. Which rectangle a
// build paints its background with is the one fact that decides whether any of
// the adjustments below can be seen at all, and it differs between builds.
void LogFillContext(FrameworkElement backgroundFillElem,
                    FrameworkElement content) {
    try {
        TLOG(L"background element: %s %s opacity=%.2f",
             DescribeElement(backgroundFillElem).c_str(),
             DescribeRect(backgroundFillElem, content).c_str(),
             backgroundFillElem.Opacity());
    } catch (...) {
        return;
    }

    FrameworkElement current = backgroundFillElem;
    for (int level = 0; level < 5; level++) {
        try {
            auto parent =
                Media::VisualTreeHelper::GetParent(current).try_as<FrameworkElement>();
            if (!parent) {
                break;
            }
            TLOG(L"  parent: %s %s opacity=%.2f visible=%d",
                 DescribeElement(parent).c_str(),
                 DescribeRect(parent, content).c_str(), parent.Opacity(),
                 parent.Visibility() == Visibility::Visible ? 1 : 0);
            current = parent;
        } catch (...) {
            break;
        }
    }
}

void RestoreState(AppearanceState& state) {
    if (state.tintAdjusted) {
        state.tintAdjusted = false;
        if (auto backgroundFill = state.backgroundFill.get()) {
            if (auto rect = backgroundFill.try_as<Shapes::Rectangle>()) {
                // The tint was adjusted in place. Put the saved original back
                // only if the fill still shows the dimmed value this state
                // wrote: if Windows has rewritten the tint in the meantime
                // (accent color change, theme switch), it has already restored
                // itself and the saved value must not overwrite the newer one.
                try {
                    if (auto acrylic = rect.Fill().try_as<Media::AcrylicBrush>()) {
                        if (SameColor(acrylic.TintColor(), state.appliedTintColor)) {
                            acrylic.TintColor(state.originalTintColor);
                        }
                    }
                } catch (...) {
                }
            }
        }
    }

    if (state.backgroundFillReplaced) {
        state.backgroundFillReplaced = false;
        if (auto backgroundFill = state.backgroundFill.get()) {
            if (auto rect = backgroundFill.try_as<Shapes::Rectangle>()) {
                try {
                    // The brush that is installed now has to be the one this
                    // state put there; anything else belongs to Windows.
                    auto installed = state.installedFillBrush.get();
                    if (installed && SameObject<Media::Brush>(installed, rect.Fill())) {
                        rect.Fill(state.originalFillBrush);
                    }
                } catch (...) {
                }
            }
        }
        state.originalFillBrush = nullptr;
        state.installedFillBrush = {};
    }

    if (auto backgroundFill = state.backgroundFill.get()) {
        try {
            backgroundFill.Opacity(state.originalFillOpacity);
        } catch (...) {
        }
    }

    if (state.topLineHidden) {
        state.topLineHidden = false;
        if (auto topLine = state.topLine.get()) {
            try {
                topLine.Opacity(state.originalTopLineOpacity);
            } catch (...) {
            }
        }
    }

    if (state.gripHidden) {
        state.gripHidden = false;
        if (auto grip = state.grip.get()) {
            try {
                grip.Opacity(state.originalGripOpacity);
            } catch (...) {
            }
        }
    }

    for (auto& [element, originalOpacity] : state.foregroundOpacity) {
        if (auto elem = element.get()) {
            try {
                elem.Opacity(originalOpacity);
            } catch (...) {
            }
        }
    }
    state.foregroundOpacity.clear();
}

AppearanceState* FindStateForRoot(FrameworkElement root) {
    for (auto& state : g_states) {
        auto stateRoot = state->root.get();
        if (stateRoot &&
            SameObject<FrameworkElement>(stateRoot, root)) {
            return state.get();
        }
    }
    return nullptr;
}

ApplyOutcome ApplyConfigImpl(XamlRoot const& xamlRoot, const Config& config) {
    // try_as, not as: an empty root must be reported as "not laid out yet"
    // rather than throwing and being counted as a hard failure.
    auto content = xamlRoot.Content().try_as<FrameworkElement>();
    if (!content) {
        return ApplyOutcome::NoBackground;
    }

    if (!config.enabled) {
        RestoreAll();
        return ApplyOutcome::Restored;
    }

    g_settings = SettingsFromConfig(config);

    if (g_settings.IsNeutral()) {
        RestoreAll();
        return ApplyOutcome::Restored;
    }

    auto backgroundFillElem = FindBackgroundFill(content);
    if (!backgroundFillElem) {
        DumpTreeOnce(content);
        return ApplyOutcome::NoBackground;
    }

    auto backgroundFill = backgroundFillElem.try_as<Shapes::Rectangle>();
    if (!backgroundFill) {
        TLOG(L"BackgroundFill is not a Rectangle");
        return ApplyOutcome::NoBackground;
    }

    const bool hasForegroundOpacity = g_settings.iconOpacity != 100;

    // Collect the icon layer before touching anything: if it is requested but
    // empty, the taskbar exists without having been laid out yet, and applying
    // the background anyway would make it flicker on every retry.
    std::vector<FrameworkElement> foregroundElements;
    if (hasForegroundOpacity) {
        auto [bandTop, bandBottom] = GetVisibleBand(content, backgroundFillElem);
        foregroundElements = CollectForegroundElements(content, bandTop, bandBottom);
        if (foregroundElements.empty()) {
            TLOG(L"icon and text layer is empty, taskbar not laid out yet");
            return ApplyOutcome::NotReady;
        }
    }

    auto previous = FindStateForRoot(content);
    if (!previous) {
        LogFillContext(backgroundFillElem, content);
    } else {
        RestoreState(*previous);
        for (auto it = g_states.begin(); it != g_states.end(); ++it) {
            if (it->get() == previous) {
                g_states.erase(it);
                break;
            }
        }
    }

    // The state is registered before it is filled in: if a step below throws,
    // the partially applied state is still tracked and restored like any other,
    // so a replaced fill is never left behind unaccounted for.
    auto state = std::make_unique<AppearanceState>();
    state->root = winrt::make_weak(content);
    state->backgroundFill = winrt::make_weak(backgroundFill.as<FrameworkElement>());
    state->originalFillOpacity = backgroundFill.Opacity();
    state->applied = g_settings;

    ApplyTopLineStyle(content, backgroundFillElem, *state);
    ApplyGripStyle(content, *state);
    ApplyBackgroundStyle(backgroundFill, *state);

    if (hasForegroundOpacity) {
        ApplyForegroundStyle(foregroundElements, *state);
    }

    g_states.push_back(std::move(state));
    return ApplyOutcome::Applied;
}

}  // namespace

ApplyOutcome ApplyConfig(XamlRoot const& root, const Config& config) {
    try {
        return ApplyConfigImpl(root, config);
    } catch (...) {
        TLOG(L"ApplyConfig failed with an exception");
        return ApplyOutcome::Failed;
    }
}

void RestoreAll() {
    for (auto& state : g_states) {
        try {
            RestoreState(*state);
        } catch (...) {
            TLOG(L"RestoreState failed with an exception");
        }
    }
    g_states.clear();
}

bool HasTrackedRoots() {
    return !g_states.empty();
}

bool IsRootTrackedAndAlive(XamlRoot const& root, const Config& config) {
    if (!config.enabled) {
        return false;
    }

    const Settings wanted = SettingsFromConfig(config);
    if (wanted.IsNeutral()) {
        // Nothing to keep in place: the restore still has to run.
        return false;
    }

    try {
        auto content = root.Content().try_as<FrameworkElement>();
        if (!content) {
            return false;
        }

        auto* state = FindStateForRoot(content);
        if (!state) {
            return false;
        }

        if (state->applied != wanted) {
            return false;
        }

        if (!state->backgroundFill.get()) {
            return false;
        }

        if (state->topLineHidden && !state->topLine.get()) {
            return false;
        }

        if (state->gripHidden && !state->grip.get()) {
            return false;
        }

        for (auto& [element, opacity] : state->foregroundOpacity) {
            if (!element.get()) {
                return false;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace tuner::hook
