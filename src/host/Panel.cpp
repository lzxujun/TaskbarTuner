#include "Panel.h"

#include "../../res/resource.h"
#include "../../res/version.h"
#include "../shared/Config.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

namespace tuner {
namespace {

// Control identifiers: the sliders and the check boxes are child windows, so
// their notifications arrive as WM_HSCROLL and WM_COMMAND.
constexpr int kSliderIdBase = 100;
constexpr int kCheckboxIdBase = 200;

// One slider per row. The label sits above the track, the current value is
// printed right aligned on the same line as the label.
struct SliderSpec {
    const wchar_t* label;
    int labelTop;
    int trackCenter;
};

// Order matches the window: brightness first, then the two opacity sliders.
const SliderSpec kSliders[Panel::kSliderCount] = {
    {L"亮度", 86, 123},
    {L"不透明度", 148, 185},
    {L"不透明度", 241, 278},
};

const wchar_t* kCheckboxLabels[Panel::kCheckboxCount] = {
    L"显示任务栏顶部分隔线",
    L"显示所有托盘图标",
    L"随 Windows 开机自动启动",
};

const wchar_t* kButtonLabels[Panel::kButtonCount] = {
    L"恢复默认",
    L"退出程序",
};

// Layout in logical (96 DPI) pixels, measured off the original window: a
// 398 x 457 client area, 20 px side padding, two sections, three sliders on a
// 62 px stride, three check boxes on a 30 px stride and a 35 px button row.
constexpr int kClientWidth = 398;
constexpr int kClientHeight = 457;
constexpr int kPadX = 20;

constexpr int kTitleTop = 12;
constexpr int kTitleHeight = 26;
constexpr int kInfoTop = 20;
constexpr int kInfoSize = 12;

constexpr int kSection1Top = 60;
constexpr int kSection2Top = 214;
constexpr int kSectionHeight = 18;

constexpr int kSliderLabelHeight = 18;
constexpr int kTrackInset = 5;
constexpr int kSliderHeight = 32;   // system track bar, vertically centred

constexpr int kCheckboxTop = 306;
constexpr int kCheckboxStride = 30;
constexpr int kCheckboxHeight = 24;

constexpr int kButtonTop = 404;
constexpr int kButtonHeight = 35;
constexpr int kButtonGap = 9;

struct Palette {
    COLORREF background;
    COLORREF text;
    COLORREF secondary;
    COLORREF accent;
    COLORREF accentHover;
    COLORREF onAccent;
    COLORREF control;
    COLORREF controlHover;
    COLORREF error;
};

// The sliders and the check boxes are system controls and always render in the
// light theme colours, so the panel uses the same palette: the standard dialog
// face as the background and text colours that match it. This keeps the hand
// drawn parts and the native parts in one visual language regardless of the
// system theme.
Palette NativePalette() {
    const COLORREF face = GetSysColor(COLOR_BTNFACE);  // standard dialog grey
    return {face,
            GetSysColor(COLOR_BTNTEXT),
            RGB(104, 108, 116),
            RGB(0, 120, 212),
            RGB(16, 140, 232),
            RGB(255, 255, 255),
            RGB(236, 237, 240),
            RGB(220, 222, 228),
            RGB(190, 40, 40)};
}

Palette g_palette = NativePalette();

// The project repository: the info button in the top right corner opens it
// in the default browser.
constexpr wchar_t kRepoUrl[] = L"https://github.com/lzxujun/TaskbarTuner";

HPEN CreateRoundPen(COLORREF color, int width) {
    LOGBRUSH brush{};
    brush.lbStyle = BS_SOLID;
    brush.lbColor = color;
    return ExtCreatePen(
        PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
        static_cast<DWORD>(std::max(1, width)), &brush, 0, nullptr);
}

}  // namespace

Panel::Panel() {
    m_config = LoadConfig();
}

Panel::~Panel() {
    if (m_backBitmap) {
        DeleteObject(m_backBitmap);
    }
    if (m_backDc) {
        DeleteDC(m_backDc);
    }

    for (HFONT* font :
         {&m_titleFont, &m_versionFont, &m_sectionFont, &m_labelFont,
          &m_valueFont}) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

bool Panel::Create(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &Panel::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.lpszClassName = L"TaskbarTunerPanel";

    if (!RegisterClassExW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    // A normal frame, like the original program: it has a title bar with the
    // program icon, a minimise box and a close box.
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;

    m_hwnd = CreateWindowExW(exStyle, L"TaskbarTunerPanel", L"Taskbar Tuner",
                             style, CW_USEDEFAULT, CW_USEDEFAULT, 200, 200,
                             nullptr, nullptr, instance, this);
    if (!m_hwnd) {
        return false;
    }

    m_dpi = GetDpiForWindow(m_hwnd);
    if (m_dpi == 0) {
        m_dpi = 96;
    }
    RebuildFonts();

    RECT frame{0, 0, Scale(kClientWidth), Scale(kClientHeight)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, m_dpi);
    SetWindowPos(m_hwnd, nullptr, 0, 0, frame.right - frame.left,
                 frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    CreateControls();
    SyncControlsFromConfig();
    LayoutControls();

    // Windows 11 rounds the frame of a normal window on its own; asking for it
    // explicitly keeps the corners round on builds that do not.
    const DWORD cornerPreference = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                          &cornerPreference, sizeof(cornerPreference));

    return true;
}

void Panel::CreateControls() {
    for (int i = 0; i < kSliderCount; i++) {
        m_sliders[i] = CreateWindowExW(
            0, TRACKBAR_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 0, 0, 10, 10,
            m_hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSliderIdBase + i)),
            nullptr, nullptr);

        if (!m_sliders[i]) {
            continue;
        }

        SendMessageW(m_sliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(m_sliders[i], TBM_SETPAGESIZE, 0, 5);
        SendMessageW(m_sliders[i], TBM_SETLINESIZE, 0, 1);
        SendMessageW(m_sliders[i], WM_SETFONT,
                     reinterpret_cast<WPARAM>(m_labelFont), TRUE);
    }

    for (int i = 0; i < kCheckboxCount; i++) {
        m_checkboxes[i] = CreateWindowExW(
            0, L"BUTTON", kCheckboxLabels[i],
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_LEFT, 0, 0, 10, 10,
            m_hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxIdBase + i)),
            nullptr, nullptr);

        if (!m_checkboxes[i]) {
            continue;
        }

        SendMessageW(m_checkboxes[i], WM_SETFONT,
                     reinterpret_cast<WPARAM>(m_labelFont), TRUE);
    }
}

void Panel::LayoutControls() {
    for (int i = 0; i < kSliderCount; i++) {
        if (!m_sliders[i]) {
            continue;
        }
        RECT track = SliderTrack(i);
        const int height = Scale(kSliderHeight);
        const int center = Scale(kSliders[i].trackCenter);
        SetWindowPos(m_sliders[i], nullptr, track.left, center - height / 2,
                     track.right - track.left, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    for (int i = 0; i < kCheckboxCount; i++) {
        if (!m_checkboxes[i]) {
            continue;
        }
        RECT rect = CheckboxRect(i);
        SetWindowPos(m_checkboxes[i], nullptr, rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void Panel::SyncControlsFromConfig() {
    for (int i = 0; i < kSliderCount; i++) {
        if (m_sliders[i]) {
            SendMessageW(m_sliders[i], TBM_SETPOS, TRUE, SliderValue(i));
        }
    }

    for (int i = 0; i < kCheckboxCount; i++) {
        if (!m_checkboxes[i]) {
            continue;
        }
        bool enabled = false;
        switch (i) {
            case 0:
                enabled = m_config.topLine;
                break;
            case 1:
                enabled = m_config.showAllTray;
                break;
            case 2:
                enabled = m_config.autoStart;
                break;
            default:
                break;
        }
        SendMessageW(m_checkboxes[i], BM_SETCHECK,
                     enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void Panel::Show() {
    if (!m_hwnd) {
        return;
    }

    if (IsVisible()) {
        SetForegroundWindow(m_hwnd);
        return;
    }

    SyncControlsFromConfig();

    // Centre on the monitor that holds the cursor.
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);

    RECT window{};
    GetWindowRect(m_hwnd, &window);
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;

    const int x = info.rcWork.left +
                  ((info.rcWork.right - info.rcWork.left) - width) / 2;
    const int y = info.rcWork.top +
                  ((info.rcWork.bottom - info.rcWork.top) - height) / 2;

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_SHOWWINDOW);
    m_shownTick = GetTickCount64();
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
}

void Panel::Hide() {
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_HIDE);
    }
}

void Panel::Toggle() {
    if (IsVisible()) {
        Hide();
    } else {
        Show();
    }
}

bool Panel::IsVisible() const {
    return m_hwnd && IsWindowVisible(m_hwnd);
}

void Panel::SetConfig(Config const& config) {
    m_config = config;
    if (m_hwnd) {
        SyncControlsFromConfig();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Panel::SetChangeHandler(ChangeHandler handler) {
    m_onChange = std::move(handler);
}

void Panel::SetExitHandler(ExitHandler handler) {
    m_onExit = std::move(handler);
}

void Panel::SetStatusText(std::wstring text, bool isError) {
    m_status = std::move(text);
    m_statusIsError = isError;
}

int Panel::Scale(int value) const {
    return MulDiv(value, m_dpi, 96);
}

RECT Panel::SliderTrack(int index) const {
    const int centerY = Scale(kSliders[index].trackCenter);
    const int height = Scale(kSliderHeight);
    RECT rect{Scale(kPadX + kTrackInset), centerY - height / 2,
              Scale(kClientWidth - kPadX - kTrackInset),
              centerY - height / 2 + height};
    return rect;
}

RECT Panel::SliderLabelRect(int index) const {
    const int top = Scale(kSliders[index].labelTop);
    RECT rect{Scale(kPadX), top, Scale(kClientWidth - kPadX),
              top + Scale(kSliderLabelHeight)};
    return rect;
}

RECT Panel::SliderValueRect(int index) const {
    RECT label = SliderLabelRect(index);
    RECT rect{label.right - Scale(60), label.top, label.right, label.bottom};
    return rect;
}

RECT Panel::CheckboxRect(int index) const {
    const int centerY =
        Scale(kCheckboxTop + index * kCheckboxStride + kCheckboxStride / 2);
    const int height = Scale(kCheckboxHeight);
    RECT rect{Scale(kPadX), centerY - height / 2, Scale(kClientWidth - kPadX),
              centerY - height / 2 + height};
    return rect;
}

RECT Panel::ActionButton(int index) const {
    const int total = Scale(kClientWidth) - 2 * Scale(kPadX);
    const int gap = Scale(kButtonGap);
    const int width = (total - gap) / 2;
    const int left = Scale(kPadX) + index * (width + gap);
    RECT rect{left, Scale(kButtonTop), left + width,
              Scale(kButtonTop + kButtonHeight)};
    return rect;
}

RECT Panel::InfoButton() const {
    const int size = Scale(kInfoSize);
    const int right = Scale(kClientWidth - kPadX);
    RECT rect{right - size, Scale(kInfoTop), right, Scale(kInfoTop) + size};
    return rect;
}

int Panel::SliderValue(int index) const {
    switch (index) {
        case 0:
            return m_config.brightness;
        case 1:
            return m_config.opacity;
        case 2:
            return m_config.iconOpacity;
        default:
            return 100;
    }
}

void Panel::SetSliderValue(int index, int value) {
    switch (index) {
        case 0:
            m_config.brightness = value;
            break;
        case 1:
            m_config.opacity = value;
            break;
        case 2:
            m_config.iconOpacity = value;
            break;
        default:
            break;
    }
}

void Panel::RebuildFonts() {
    for (HFONT* font :
         {&m_titleFont, &m_versionFont, &m_sectionFont, &m_labelFont,
          &m_valueFont}) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }

    auto make = [&](int pixels, int weight) -> HFONT {
        LOGFONTW logFont{};
        logFont.lfHeight = -Scale(pixels);
        logFont.lfWeight = weight;
        logFont.lfCharSet = DEFAULT_CHARSET;
        logFont.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(logFont.lfFaceName, L"Microsoft YaHei UI");
        return CreateFontIndirectW(&logFont);
    };

    m_titleFont = make(19, FW_SEMIBOLD);
    m_versionFont = make(12, FW_NORMAL);
    m_sectionFont = make(12, FW_NORMAL);
    m_labelFont = make(13, FW_NORMAL);
    m_valueFont = make(13, FW_SEMIBOLD);

    for (int i = 0; i < kSliderCount; i++) {
        if (m_sliders[i]) {
            SendMessageW(m_sliders[i], WM_SETFONT,
                         reinterpret_cast<WPARAM>(m_labelFont), TRUE);
        }
    }
    for (int i = 0; i < kCheckboxCount; i++) {
        if (m_checkboxes[i]) {
            SendMessageW(m_checkboxes[i], WM_SETFONT,
                         reinterpret_cast<WPARAM>(m_labelFont), TRUE);
        }
    }
}

void Panel::UpdateBackBuffer(HDC reference, int width, int height) {
    if (m_backDc && m_backWidth == width && m_backHeight == height) {
        return;
    }

    if (m_backBitmap) {
        DeleteObject(m_backBitmap);
        m_backBitmap = nullptr;
    }
    if (!m_backDc) {
        m_backDc = CreateCompatibleDC(reference);
    }

    m_backBitmap = CreateCompatibleBitmap(reference, width, height);
    SelectObject(m_backDc, m_backBitmap);

    m_backWidth = width;
    m_backHeight = height;
}

void Panel::FillRounded(HDC dc, RECT rect, int diameter, COLORREF color) {
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, color);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, diameter,
              diameter);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

void Panel::StrokeRounded(HDC dc, RECT rect, int diameter, COLORREF color,
                          int width) {
    HPEN pen = CreateRoundPen(color, Scale(width));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, diameter,
              diameter);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void Panel::PaintText(HDC dc, PCWSTR text, RECT rect, COLORREF color, HFONT font,
                      UINT format) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &rect, format);
    SelectObject(dc, oldFont);
}

void Panel::PaintActionButton(HDC dc, int index) {
    RECT rect = ActionButton(index);
    const bool primary = (index == 0);
    const bool hovered =
        (m_hoverButton == index) || (m_pressedButton == index);

    COLORREF fill;
    COLORREF text;
    if (primary) {
        fill = hovered ? g_palette.accentHover : g_palette.accent;
        text = g_palette.onAccent;
    } else {
        fill = hovered ? g_palette.controlHover : g_palette.control;
        text = g_palette.text;
    }

    FillRounded(dc, rect, Scale(4) * 2, fill);
    PaintText(dc, kButtonLabels[index], rect, text, m_labelFont,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

void Panel::PaintInfoButton(HDC dc) {
    RECT rect = InfoButton();
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const int radius = (rect.right - rect.left) / 2;
    const COLORREF color =
        m_hoverInfo ? g_palette.accentHover : g_palette.accent;

    HPEN pen = CreateRoundPen(color, Scale(1));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, centerX - radius, centerY - radius, centerX + radius + 1,
            centerY + radius + 1);
    SelectObject(dc, oldBrush);

    // Lower case "i": a dot above a stem.
    const int dotY = centerY - Scale(3);
    const int stemTop = centerY - Scale(1);
    const int stemBottom = centerY + Scale(3);
    MoveToEx(dc, centerX, stemTop, nullptr);
    LineTo(dc, centerX, stemBottom);
    MoveToEx(dc, centerX, dotY, nullptr);
    LineTo(dc, centerX, dotY + 1);

    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void Panel::OnPaint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(m_hwnd, &paint);

    RECT client{};
    GetClientRect(m_hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;

    UpdateBackBuffer(dc, width, height);

    HDC target = m_backDc;
    HBRUSH background = CreateSolidBrush(g_palette.background);
    FillRect(target, &client, background);
    DeleteObject(background);

    // Title, with the version right behind it.
    RECT titleRect{Scale(kPadX), Scale(kTitleTop), Scale(kClientWidth - kPadX),
                   Scale(kTitleTop + kTitleHeight)};
    PaintText(target, L"任务栏调节器", titleRect, g_palette.text, m_titleFont,
              DT_LEFT | DT_SINGLELINE | DT_BOTTOM);

    SIZE titleSize{};
    {
        HGDIOBJ oldFont = SelectObject(target, m_titleFont);
        GetTextExtentPoint32W(target, L"任务栏调节器", 6, &titleSize);
        SelectObject(target, oldFont);
    }
    RECT versionRect{Scale(kPadX) + titleSize.cx + Scale(7), Scale(kTitleTop),
                     Scale(kClientWidth - kPadX),
                     Scale(kTitleTop + kTitleHeight)};
    PaintText(target, L"v" TUNER_VERSION_DOT3W, versionRect,
              g_palette.secondary, m_versionFont,
              DT_LEFT | DT_SINGLELINE | DT_BOTTOM);

    PaintInfoButton(target);

    // Section 1: taskbar background.
    RECT section1{Scale(kPadX), Scale(kSection1Top),
                  Scale(kClientWidth - kPadX),
                  Scale(kSection1Top + kSectionHeight)};
    PaintText(target, L"任务栏背景", section1, g_palette.secondary,
              m_sectionFont, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // Labels and values of the sliders; the sliders themselves are child
    // windows and paint on their own.
    for (int i = 0; i < 2; i++) {
        PaintText(target, kSliders[i].label, SliderLabelRect(i), g_palette.text,
                  m_labelFont, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        wchar_t valueText[8]{};
        swprintf_s(valueText, L"%d%%", SliderValue(i));
        PaintText(target, valueText, SliderValueRect(i), g_palette.accent,
                  m_valueFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    // Section 2: task buttons and tray.
    RECT section2{Scale(kPadX), Scale(kSection2Top),
                  Scale(kClientWidth - kPadX),
                  Scale(kSection2Top + kSectionHeight)};
    PaintText(target, L"任务按钮和系统托盘", section2, g_palette.secondary,
              m_sectionFont, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    PaintText(target, kSliders[2].label, SliderLabelRect(2), g_palette.text,
              m_labelFont, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    {
        wchar_t valueText[8]{};
        swprintf_s(valueText, L"%d%%", SliderValue(2));
        PaintText(target, valueText, SliderValueRect(2), g_palette.accent,
                  m_valueFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    for (int i = 0; i < kButtonCount; i++) {
        PaintActionButton(target, i);
    }

    BitBlt(dc, 0, 0, width, height, target, 0, 0, SRCCOPY);
    EndPaint(m_hwnd, &paint);
}

void Panel::NotifyChanged() {
    SaveConfig(m_config);

    if (m_onChange) {
        m_onChange(m_config);
    }
}

void Panel::OnSliderScroll(int index, WORD code) {
    if (!m_sliders[index]) {
        return;
    }

    const int value =
        static_cast<int>(SendMessageW(m_sliders[index], TBM_GETPOS, 0, 0));
    SetSliderValue(index, std::clamp(value, 0, 100));

    RECT valueRect = SliderValueRect(index);
    InflateRect(&valueRect, Scale(4), Scale(2));
    InvalidateRect(m_hwnd, &valueRect, FALSE);

    // While the thumb is being dragged only the panel is redrawn; the value is
    // written to the registry and pushed once the drag ends, so that a single
    // drag does not hammer the shell with hundreds of updates.
    if (code != TB_THUMBTRACK) {
        NotifyChanged();
    }
}

void Panel::OnCommand(WORD id, WORD code) {
    if (id < kCheckboxIdBase || id >= kCheckboxIdBase + kCheckboxCount) {
        return;
    }
    if (code != BN_CLICKED) {
        return;
    }

    const int index = id - kCheckboxIdBase;
    const bool checked =
        SendMessageW(m_checkboxes[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
    switch (index) {
        case 0:
            m_config.topLine = checked;
            break;
        case 1:
            m_config.showAllTray = checked;
            break;
        case 2:
            m_config.autoStart = checked;
            SetAutoStart(checked);
            break;
        default:
            return;
    }

    NotifyChanged();
}

void Panel::OnMouseDown(int x, int y) {
    POINT point{x, y};

    RECT info = InfoButton();
    if (PtInRect(&info, point)) {
        // Quick access to the project repository in the default browser.
        ShellExecuteW(m_hwnd, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    for (int i = 0; i < kButtonCount; i++) {
        RECT rect = ActionButton(i);
        if (PtInRect(&rect, point)) {
            m_pressedButton = i;
            SetCapture(m_hwnd);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
    }
}

void Panel::OnMouseMove(int x, int y, bool tracking) {
    if (!tracking) {
        return;
    }

    POINT point{x, y};

    int hoverButton = -1;
    for (int i = 0; i < kButtonCount; i++) {
        RECT rect = ActionButton(i);
        if (PtInRect(&rect, point)) {
            hoverButton = i;
            break;
        }
    }

    RECT info = InfoButton();
    const bool hoverInfo = PtInRect(&info, point) != 0;

    if (hoverButton != m_hoverButton || hoverInfo != m_hoverInfo) {
        m_hoverButton = hoverButton;
        m_hoverInfo = hoverInfo;

        TRACKMOUSEEVENT leave{};
        leave.cbSize = sizeof(leave);
        leave.dwFlags = TME_LEAVE;
        leave.hwndTrack = m_hwnd;
        TrackMouseEvent(&leave);

        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Panel::OnMouseUp() {
    if (m_pressedButton < 0) {
        return;
    }

    const int button = m_pressedButton;
    m_pressedButton = -1;
    ReleaseCapture();
    InvalidateRect(m_hwnd, nullptr, FALSE);

    if (button == 0) {
        // Restore defaults: the three sliders go back to "no change" and the
        // check boxes to their shipped state. The startup entry is left as the
        // user set it.
        Config defaults;
        defaults.enabled = m_config.enabled;
        defaults.autoStart = m_config.autoStart;
        m_config = defaults;
        SyncControlsFromConfig();
        NotifyChanged();
    } else if (m_onExit) {
        m_onExit();
    }
}

LRESULT Panel::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_HSCROLL: {
            HWND source = reinterpret_cast<HWND>(lParam);
            for (int i = 0; i < kSliderCount; i++) {
                if (source == m_sliders[i]) {
                    OnSliderScroll(i, LOWORD(wParam));
                    return 0;
                }
            }
            break;
        }

        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;

        case WM_LBUTTONDOWN:
            OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP:
            OnMouseUp();
            return 0;

        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true);
            return 0;

        case WM_MOUSELEAVE:
            // Without this the last hovered button keeps its highlight after
            // the pointer leaves the window.
            if (m_hoverButton >= 0 || m_hoverInfo) {
                m_hoverButton = -1;
                m_hoverInfo = false;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_DPICHANGED: {
            m_dpi = HIWORD(wParam);
            if (m_dpi == 0) {
                m_dpi = 96;
            }
            RebuildFonts();

            const DWORD style =
                static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
            const DWORD exStyle =
                static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
            RECT frame{0, 0, Scale(kClientWidth), Scale(kClientHeight)};
            AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, m_dpi);

            auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top,
                         frame.right - frame.left,
                         frame.bottom - frame.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);

            LayoutControls();
            InvalidateRect(m_hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_SETTINGCHANGE:
            // The palette is fixed to the native light look, so theme changes
            // do not require a repaint decision here.
            break;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && GetCapture() != m_hwnd) {
                // Clicking elsewhere hides the panel. The guard skips the
                // deactivation that arrives right after Show() when the window
                // manager refuses to give the panel the foreground.
                if (GetTickCount64() - m_shownTick > 250) {
                    Hide();
                }
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                Hide();
                return 0;
            }
            return 0;

        case WM_CLOSE:
            Hide();
            return 0;

        case WM_DESTROY:
            if (m_backBitmap) {
                DeleteObject(m_backBitmap);
                m_backBitmap = nullptr;
            }
            if (m_backDc) {
                DeleteDC(m_backDc);
                m_backDc = nullptr;
            }
            return 0;

        default:
            break;
    }

    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

LRESULT CALLBACK Panel::WndProc(HWND hwnd, UINT message, WPARAM wParam,
                                LPARAM lParam) {
    Panel* instance =
        reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        instance = static_cast<Panel*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(instance));
    }

    if (instance && message != WM_NCCREATE) {
        return instance->HandleMessage(message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace tuner
