// A XAML brush that renders the live backdrop behind the taskbar with a
// color-matrix dimming effect. This is the last-resort strategy in
// ApplyBackgroundStyle: it is only used when the taskbar background is neither
// an acrylic brush nor a plain solid color brush.
#pragma once

#include <unknwn.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace tuner::hook {

// brightness is in -1..0: 0 leaves the backdrop unchanged, -1 renders it
// black. Must be called on the taskbar UI thread.
winrt::Windows::UI::Xaml::Media::Brush CreateBackdropDimBrush(
    winrt::Windows::UI::Composition::Compositor const& compositor,
    float brightness);

}  // namespace tuner::hook
