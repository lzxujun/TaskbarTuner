// The settings window: a panel drawn by hand for the background and the labels,
// with the interactive parts left to the system controls so that the sliders
// and the check boxes look and behave exactly like every other Windows window.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "../shared/Config.h"

namespace tuner {

class Panel {
public:
    using ChangeHandler = std::function<void(Config const&)>;
    using ExitHandler = std::function<void()>;

    static constexpr int kSliderCount = 3;
    static constexpr int kCheckboxCount = 3;
    static constexpr int kButtonCount = 2;

    Panel();
    ~Panel();

    bool Create(HINSTANCE instance);
    void Show();
    void Hide();
    void Toggle();
    bool IsVisible() const;

    void SetConfig(Config const& config);
    Config const& GetConfig() const { return m_config; }

    void SetChangeHandler(ChangeHandler handler);
    void SetExitHandler(ExitHandler handler);
    // Kept for the tray notifications; the panel itself has no status row.
    void SetStatusText(std::wstring text, bool isError);

    HWND Handle() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam,
                                    LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls();
    void LayoutControls();
    void SyncControlsFromConfig();
    void OnPaint();
    void OnMouseDown(int x, int y);
    void OnMouseMove(int x, int y, bool tracking);
    void OnMouseUp();
    void OnSliderScroll(int index, WORD code);
    void OnCommand(WORD id, WORD code);
    void NotifyChanged();

    // Layout helpers, all in physical pixels.
    int Scale(int value) const;
    RECT SliderTrack(int index) const;
    RECT SliderLabelRect(int index) const;
    RECT SliderValueRect(int index) const;
    RECT CheckboxRect(int index) const;
    RECT ActionButton(int index) const;
    RECT InfoButton() const;
    int SliderValue(int index) const;
    void SetSliderValue(int index, int value);

    void PaintInfoButton(HDC dc);
    void PaintActionButton(HDC dc, int index);
    void PaintText(HDC dc, PCWSTR text, RECT rect, COLORREF color, HFONT font,
                   UINT format);
    void FillRounded(HDC dc, RECT rect, int diameter, COLORREF color);
    void StrokeRounded(HDC dc, RECT rect, int diameter, COLORREF color,
                       int width);
    void RebuildFonts();
    void UpdateBackBuffer(HDC reference, int width, int height);

    HWND m_hwnd = nullptr;
    Config m_config;
    ChangeHandler m_onChange;
    ExitHandler m_onExit;

    // Sliders and check boxes are real system controls; only the frame, the
    // labels and the two buttons are drawn by hand.
    HWND m_sliders[kSliderCount]{};
    HWND m_checkboxes[kCheckboxCount]{};

    int m_dpi = 96;
    bool m_hoverInfo = false;
    int m_hoverButton = -1;
    int m_pressedButton = -1;
    ULONGLONG m_shownTick = 0;

    std::wstring m_status;
    bool m_statusIsError = false;

    HFONT m_titleFont = nullptr;
    HFONT m_versionFont = nullptr;
    HFONT m_sectionFont = nullptr;
    HFONT m_labelFont = nullptr;
    HFONT m_valueFont = nullptr;

    HDC m_backDc = nullptr;
    HBITMAP m_backBitmap = nullptr;
    int m_backWidth = 0;
    int m_backHeight = 0;
};

}  // namespace tuner
