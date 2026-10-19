#include "BackdropBrush.h"

#include "../shared/Log.h"

#include <algorithm>
#include <string_view>

#include <combaseapi.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>

namespace wuc = winrt::Windows::UI::Composition;
namespace wge = winrt::Windows::Graphics::Effects;
namespace wxm = winrt::Windows::UI::Xaml::Media;
namespace wfc = winrt::Windows::Foundation::Collections;

namespace tuner::hook {
namespace {

// windows.graphics.effects.interop.h is not available in every SDK/WDK
// combination, so the small part the effect needs is declared here. The GUID
// and the vtable order are documented ABI and have not changed since Windows
// 10 1809.
#ifndef BUILD_WINDOWS
namespace ABI {
#endif
namespace Windows {
namespace Graphics {
namespace Effects {

typedef interface IGraphicsEffectSource IGraphicsEffectSource;
typedef interface IGraphicsEffectD2D1Interop IGraphicsEffectD2D1Interop;

typedef enum GRAPHICS_EFFECT_PROPERTY_MAPPING {
    GRAPHICS_EFFECT_PROPERTY_MAPPING_UNKNOWN,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_VECTORX,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_VECTORY,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_VECTORZ,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_VECTORW,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_RECT_TO_VECTOR4,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_RADIANS_TO_DEGREES,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_COLORMATRIX_ALPHA_MODE,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_COLOR_TO_VECTOR3,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_COLOR_TO_VECTOR4
} GRAPHICS_EFFECT_PROPERTY_MAPPING;

#undef INTERFACE
#define INTERFACE IGraphicsEffectD2D1Interop
DECLARE_INTERFACE_IID_(IGraphicsEffectD2D1Interop, IUnknown,
                       "2FC57384-A068-44D7-A331-30982FCF7177")
{
    STDMETHOD(GetEffectId)(_Out_ GUID * id) PURE;

    STDMETHOD(GetNamedPropertyMapping)(LPCWSTR name, _Out_ UINT * index,
                                       _Out_ GRAPHICS_EFFECT_PROPERTY_MAPPING *
                                           mapping) PURE;

    STDMETHOD(GetPropertyCount)(_Out_ UINT * count) PURE;

    STDMETHOD(GetProperty)(
        UINT index,
        _Outptr_ winrt::impl::abi_t<winrt::Windows::Foundation::IPropertyValue> **
            value) PURE;

    STDMETHOD(GetSource)(UINT index, _Outptr_ IGraphicsEffectSource * *source)
        PURE;

    STDMETHOD(GetSourceCount)(_Out_ UINT * count) PURE;
};

}  // namespace Effects
}  // namespace Graphics
}  // namespace Windows
#ifndef BUILD_WINDOWS
}  // namespace ABI
#endif

namespace awge = ABI::Windows::Graphics::Effects;

// {921F03D6-641C-47DF-852D-B4BB6153AE11} - CLSID_D2D1ColorMatrix, duplicated as
// a local constant so that no d2d1effects.lib import is required.
constexpr GUID kColorMatrixEffectId{
    0x921f03d6, 0x641c, 0x47df, {0x85, 0x2d, 0xb4, 0xbb, 0x61, 0x53, 0xae, 0x11}};

// D2D1_MATRIX_5X4_F encoded as an ID2D1Effect-ready effect. The property
// indexes are hardcoded because d2d1effects.h is version sensitive.
constexpr UINT kColorMatrixProperty = 0;   // D2D1_COLORMATRIX_PROP_COLOR_MATRIX
constexpr UINT kAlphaModeProperty = 1;     // D2D1_COLORMATRIX_PROP_ALPHA_MODE
constexpr UINT kClampOutputProperty = 2;   // D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT
constexpr uint32_t kAlphaModePremultiplied = 0;

struct ColorMatrixEffect
    : winrt::implements<ColorMatrixEffect, wge::IGraphicsEffect,
                        wge::IGraphicsEffectSource,
                        awge::IGraphicsEffectD2D1Interop> {
    wge::IGraphicsEffectSource Source{nullptr};

    // Rows are the output RGBA channels, columns the input RGBA channels, and
    // the last row holds per-channel offsets.
    float Matrix[20] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
        0, 0, 0, 0,
    };

    uint32_t AlphaMode = kAlphaModePremultiplied;
    bool ClampOutput = false;

    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override {
        if (!id) {
            return E_INVALIDARG;
        }
        *id = kColorMatrixEffectId;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(
        LPCWSTR name, UINT* index,
        awge::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!index || !mapping) {
            return E_INVALIDARG;
        }

        const std::wstring_view nameView(name);
        if (nameView == L"ColorMatrix") {
            *index = kColorMatrixProperty;
            *mapping = awge::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        if (nameView == L"AlphaMode") {
            *index = kAlphaModeProperty;
            *mapping = awge::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        if (nameView == L"ClampOutput") {
            *index = kClampOutputProperty;
            *mapping = awge::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }

        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override {
        if (!count) {
            return E_INVALIDARG;
        }
        *count = 3;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(
        UINT index,
        winrt::impl::abi_t<winrt::Windows::Foundation::IPropertyValue>**
            value) noexcept override try {
        if (!value) {
            return E_INVALIDARG;
        }

        using winrt::Windows::Foundation::IPropertyValue;
        using winrt::Windows::Foundation::PropertyValue;

        switch (index) {
            case kColorMatrixProperty:
                *value = reinterpret_cast<
                    winrt::impl::abi_t<IPropertyValue>*>(winrt::detach_abi(
                    PropertyValue::CreateSingleArray(
                        winrt::array_view<const float>(Matrix, Matrix + 20))));
                break;

            case kAlphaModeProperty:
                *value = reinterpret_cast<
                    winrt::impl::abi_t<IPropertyValue>*>(winrt::detach_abi(
                    PropertyValue::CreateUInt32(AlphaMode)));
                break;

            case kClampOutputProperty:
                *value = reinterpret_cast<
                    winrt::impl::abi_t<IPropertyValue>*>(winrt::detach_abi(
                    PropertyValue::CreateBoolean(ClampOutput)));
                break;

            default:
                *value = nullptr;
                return E_BOUNDS;
        }

        return S_OK;
    } catch (...) {
        if (value) {
            *value = nullptr;
        }
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSource(
        UINT index, awge::IGraphicsEffectSource** source) noexcept override {
        if (!source) {
            return E_INVALIDARG;
        }
        if (index == 0 && Source) {
            winrt::copy_to_abi(Source, *reinterpret_cast<void**>(source));
            return S_OK;
        }
        return E_BOUNDS;
    }

    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override {
        if (!count) {
            return E_INVALIDARG;
        }
        *count = 1;
        return S_OK;
    }

    winrt::hstring Name() { return m_name; }
    void Name(winrt::hstring name) { m_name = std::move(name); }

    // IGraphicsEffect requires the Sources property. It stays empty: the only
    // source of this effect is the backdrop, which is bound through
    // CompositionEffectSourceParameter below.
    wfc::IVector<wge::IGraphicsEffectSource> Sources() { return m_sources; }

private:
    winrt::hstring m_name = L"ColorMatrixEffect";
    wfc::IVector<wge::IGraphicsEffectSource> m_sources{
        winrt::single_threaded_vector<wge::IGraphicsEffectSource>()};
};

class BackdropDimBrush : public wxm::XamlCompositionBrushBaseT<BackdropDimBrush> {
public:
    BackdropDimBrush(wuc::Compositor const& compositor, float brightness)
        : m_compositor(compositor), m_brightness(brightness) {}

    void OnConnected() {
        if (CompositionBrush()) {
            return;
        }
        CompositionBrush(CreateEffectBrush());
    }

    void OnDisconnected() { CompositionBrush(nullptr); }

private:
    wuc::CompositionBrush CreateEffectBrush() {
        auto backdropBrush = m_compositor.CreateBackdropBrush();

        auto matrix = winrt::make_self<ColorMatrixEffect>();
        matrix->Source = wuc::CompositionEffectSourceParameter(L"source");

        const float brightness = std::clamp(m_brightness, -1.0f, 0.0f);
        const float scale = 1.0f + brightness;

        auto& m = matrix->Matrix;
        m[0] = scale; m[1] = 0.0f;  m[2] = 0.0f;  m[3] = 0.0f;
        m[4] = 0.0f;  m[5] = scale; m[6] = 0.0f;  m[7] = 0.0f;
        m[8] = 0.0f;  m[9] = 0.0f;  m[10] = scale; m[11] = 0.0f;
        m[12] = 0.0f; m[13] = 0.0f; m[14] = 0.0f; m[15] = 1.0f;
        m[16] = 0.0f; m[17] = 0.0f; m[18] = 0.0f; m[19] = 0.0f;

        matrix->ClampOutput = true;
        matrix->Name(L"BrightnessEffect");

        // The runtime class implements several interfaces, so the conversion to
        // IGraphicsEffect is spelled out: passing the class itself would be
        // ambiguous.
        wge::IGraphicsEffect effect = matrix.as<wge::IGraphicsEffect>();
        auto factory = m_compositor.CreateEffectFactory(effect);
        auto brush = factory.CreateBrush();
        brush.SetSourceParameter(L"source", backdropBrush);
        return brush;
    }

    wuc::Compositor m_compositor{nullptr};
    float m_brightness = 0.0f;
};

}  // namespace

wxm::Brush CreateBackdropDimBrush(wuc::Compositor const& compositor,
                                  float brightness) {
    return winrt::make<BackdropDimBrush>(compositor, brightness);
}

}  // namespace tuner::hook
