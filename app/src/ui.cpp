#include "ui.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>

#include <algorithm>

// GDI+ headers call min/max unqualified; the project builds with NOMINMAX.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
// The GDI+ headers shadow their own members, which /W4 reports.
#pragma warning(push)
#pragma warning(disable : 4458)
#include <gdiplus.h>
#pragma warning(pop)

#include "log.h"

namespace ui {
namespace {

constexpr wchar_t kHotProperty[] = L"AirWindows.Hot";
// Colour of the surface a button sits on, stored with a marker bit so black is not "unset".
constexpr wchar_t kBehindProperty[] = L"AirWindows.Behind";
constexpr ULONG_PTR kBehindSet = 0x01000000;
ULONG_PTR g_gdiplus = 0;
wchar_t g_iconFace[LF_FACESIZE] = L"Segoe MDL2 Assets";

int CALLBACK FontExists(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

bool HasFont(const wchar_t* face) {
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(query.lfFaceName, face);
    bool found = false;
    HDC screen = GetDC(nullptr);
    EnumFontFamiliesExW(screen, &query, FontExists, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, screen);
    return found;
}

// Popup menus follow the app's dark mode only through uxtheme entry points that
// are exported by ordinal and not documented. They have kept this shape since
// Windows 10 1903; when they are missing the menus simply stay light.
void UseDarkMenus() {
    OSVERSIONINFOEXW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    version.dwBuildNumber = 18362;
    const DWORDLONG atLeast = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    if (!VerifyVersionInfoW(&version, VER_BUILDNUMBER, atLeast)) {
        return;
    }
    HMODULE theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!theme) {
        return;
    }
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    const auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(theme, MAKEINTRESOURCEA(135)));
    const auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(theme, MAKEINTRESOURCEA(136)));
    if (setMode && flush) {
        setMode(2);  // ForceDark
        flush();
    }
}

Gdiplus::Color ToColor(COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void RoundedPath(Gdiplus::GraphicsPath& path, const RECT& rect, int radius) {
    const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(rect.left);
    const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(rect.top);
    const Gdiplus::REAL width = static_cast<Gdiplus::REAL>(rect.right - rect.left) - 1.0f;
    const Gdiplus::REAL height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top) - 1.0f;
    const Gdiplus::REAL d = static_cast<Gdiplus::REAL>(std::max(1, std::min(radius * 2, static_cast<int>(std::min(width, height)))));
    path.AddArc(left, top, d, d, 180, 90);
    path.AddArc(left + width - d, top, d, d, 270, 90);
    path.AddArc(left + width - d, top + height - d, d, d, 0, 90);
    path.AddArc(left, top + height - d, d, d, 90, 90);
    path.CloseFigure();
}

LRESULT CALLBACK HoverProc(HWND button, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_MOUSEMOVE:
        if (!GetPropW(button, kHotProperty)) {
            SetPropW(button, kHotProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, button, 0};
            TrackMouseEvent(&track);
            InvalidateRect(button, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(button, kHotProperty);
        InvalidateRect(button, nullptr, FALSE);
        break;
    case WM_ERASEBKGND:
        return 1;  // DrawButton paints every pixel
    case WM_LBUTTONDBLCLK:
        // A fast second click is another click, not a double-click to ignore.
        return DefSubclassProc(button, WM_LBUTTONDOWN, wParam, lParam);
    case WM_NCDESTROY:
        RemovePropW(button, kHotProperty);
        RemovePropW(button, kBehindProperty);
        RemoveWindowSubclass(button, HoverProc, 0);
        break;
    }
    return DefSubclassProc(button, message, wParam, lParam);
}

struct Parts {
    int pad, gap, dot, glyph, chevron;
};

Parts Measure(HWND window) {
    return {Dip(window, 10), Dip(window, 7), Dip(window, 8), Dip(window, 16), Dip(window, 10)};
}

int TextWidth(HWND button, HFONT font, const std::wstring& text) {
    HDC dc = GetDC(button);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    ReleaseDC(button, dc);
    return size.cx;
}

std::wstring WindowText(HWND window) {
    wchar_t text[160];
    const int length = GetWindowTextW(window, text, ARRAYSIZE(text));
    return std::wstring(text, static_cast<size_t>(std::max(0, length)));
}

}  // namespace

void Startup() {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiplus, &input, nullptr) != Gdiplus::Ok) {
        LOGW("[UI] GDI+ could not start; buttons are drawn without rounded corners");
        g_gdiplus = 0;
    }
    if (HasFont(L"Segoe Fluent Icons")) {
        wcscpy_s(g_iconFace, L"Segoe Fluent Icons");
    } else if (!HasFont(g_iconFace)) {
        LOGW("[UI] no Segoe icon font found; toolbar icons will be missing");
    }
    UseDarkMenus();
}

void Shutdown() {
    if (g_gdiplus) {
        Gdiplus::GdiplusShutdown(g_gdiplus);
        g_gdiplus = 0;
    }
}

void StyleWindow(HWND window) {
    // 20: DWMWA_USE_IMMERSIVE_DARK_MODE. 34/35/36: border, caption and caption
    // text colours, Windows 11 only; older systems ignore them and keep the
    // dark caption from the first call.
    const BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(window, 20, &dark, sizeof(dark)))) {
        LOGI("[UI] dark title bar is not available on this Windows version");
    }
    const COLORREF chrome = kChrome;
    const COLORREF text = kText;
    DwmSetWindowAttribute(window, 34, &chrome, sizeof(chrome));
    DwmSetWindowAttribute(window, 35, &chrome, sizeof(chrome));
    DwmSetWindowAttribute(window, 36, &text, sizeof(text));
}

int Dip(HWND window, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

HFONT CreateTextFont(HWND window, int points, bool semibold) {
    return CreateFontW(-MulDiv(points, static_cast<int>(GetDpiForWindow(window)), 72), 0, 0, 0,
                       semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

HFONT CreateIconFont(HWND window, int points) {
    return CreateFontW(-MulDiv(points, static_cast<int>(GetDpiForWindow(window)), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, g_iconFace);
}

void TrackHover(HWND button, COLORREF behind) {
    SetPropW(button, kBehindProperty, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(behind) | kBehindSet));
    SetWindowSubclass(button, HoverProc, 0, 0);
}

bool IsHot(HWND button) {
    return GetPropW(button, kHotProperty) != nullptr;
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF line) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    if (!g_gdiplus) {
        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::GraphicsPath path;
    RoundedPath(path, rect, radius);
    Gdiplus::SolidBrush brush(ToColor(fill));
    graphics.FillPath(&brush, &path);
    if (line != CLR_INVALID) {
        Gdiplus::Pen pen(ToColor(line), 1.0f);
        graphics.DrawPath(&pen, &path);
    }
}

void FillCircle(HDC dc, int centerX, int centerY, int radius, COLORREF fill) {
    if (!g_gdiplus) {
        HBRUSH brush = CreateSolidBrush(fill);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, centerX - radius, centerY - radius, centerX + radius + 1, centerY + radius + 1);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(ToColor(fill));
    graphics.FillEllipse(&brush, static_cast<Gdiplus::REAL>(centerX - radius), static_cast<Gdiplus::REAL>(centerY - radius),
                         static_cast<Gdiplus::REAL>(radius * 2), static_cast<Gdiplus::REAL>(radius * 2));
}

void DrawGlyph(HDC dc, HFONT iconFont, wchar_t glyph, const RECT& rect, COLORREF color) {
    HGDIOBJ old = SelectObject(dc, iconFont);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT area = rect;
    DrawTextW(dc, &glyph, 1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    SelectObject(dc, old);
}

int ButtonWidth(HWND button, const ButtonLook& look, HFONT textFont) {
    const Parts parts = Measure(button);
    int width = parts.pad * 2;
    int items = 0;
    if (look.dot != CLR_INVALID) { width += parts.dot; ++items; }
    if (look.glyph) { width += parts.glyph; ++items; }
    if (look.showLabel) { width += TextWidth(button, textFont, WindowText(button)); ++items; }
    if (look.chevron) { width += parts.chevron; ++items; }
    return width + parts.gap * std::max(0, items - 1);
}

void DrawButton(const DRAWITEMSTRUCT& item, const ButtonLook& look, HFONT textFont, HFONT iconFont) {
    const HWND button = item.hwndItem;
    const RECT bounds = item.rcItem;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool down = (item.itemState & ODS_SELECTED) != 0;
    const bool hot = IsHot(button) && !disabled;
    const bool focus = (item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0;

    COLORREF fill = kChrome;
    COLORREF line = CLR_INVALID;
    COLORREF ink = disabled ? kTextOff : kText;
    switch (look.kind) {
    case ButtonKind::Plain:
        fill = down ? kControlDown : hot ? kControlHot : kControl;
        line = kControlLine;
        break;
    case ButtonKind::Subtle:
        fill = down ? kControlDown : hot ? kControlHot : kChrome;
        break;
    case ButtonKind::Accent:
        fill = disabled ? kControl : down ? kAccent : hot ? kAccentHot : kAccent;
        ink = disabled ? kTextOff : kAccentText;
        break;
    case ButtonKind::Record:
        fill = down ? kRecord : hot ? kRecordHot : kRecord;
        ink = RGB(0xFF, 0xFF, 0xFF);
        break;
    }

    // Paint off screen: the parent's colour first, so the rounded corners blend in.
    HDC dc = CreateCompatibleDC(item.hDC);
    HBITMAP bitmap = CreateCompatibleBitmap(item.hDC, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    const RECT local{0, 0, width, height};
    const ULONG_PTR stored = reinterpret_cast<ULONG_PTR>(GetPropW(button, kBehindProperty));
    HBRUSH behind = CreateSolidBrush(stored ? static_cast<COLORREF>(stored & 0x00FFFFFF) : kChrome);
    FillRect(dc, &local, behind);
    DeleteObject(behind);
    FillRounded(dc, local, Dip(button, 5), fill, line);
    if (focus) {
        RECT ring{1, 1, width - 1, height - 1};
        FillRounded(dc, ring, Dip(button, 4), fill, kText);
    }

    const Parts parts = Measure(button);
    const std::wstring label = look.showLabel ? WindowText(button) : std::wstring();
    HGDIOBJ oldFont = SelectObject(dc, textFont);
    SetBkMode(dc, TRANSPARENT);
    SIZE textSize{};
    if (!label.empty()) {
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &textSize);
    }
    int items = 0;
    int content = 0;
    if (look.dot != CLR_INVALID) { content += parts.dot; ++items; }
    if (look.glyph) { content += parts.glyph; ++items; }
    if (!label.empty()) { ++items; }
    if (look.chevron) { content += parts.chevron; ++items; }
    const int spacing = parts.gap * std::max(0, items - 1);
    // A long phone name gives way to the fixed parts and ends in an ellipsis.
    const int textRoom = std::max(0, width - parts.pad * 2 - content - spacing);
    const int textWidth = std::min<int>(textSize.cx, textRoom);
    content += textWidth + spacing;
    int x = look.alignLeft ? parts.pad : std::max(parts.pad, (width - content) / 2);

    if (look.dot != CLR_INVALID) {
        FillCircle(dc, x + parts.dot / 2, height / 2, parts.dot / 2, look.dot);
        x += parts.dot + parts.gap;
    }
    if (look.glyph) {
        const RECT glyphRect{x, 0, x + parts.glyph, height};
        DrawGlyph(dc, iconFont, look.glyph, glyphRect, ink);
        x += parts.glyph + parts.gap;
    }
    if (!label.empty()) {
        SelectObject(dc, textFont);
        SetTextColor(dc, ink);
        RECT textRect{x, 0, x + textWidth, height};
        DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        x += textWidth + parts.gap;
    }
    if (look.chevron) {
        // Kept at the right edge so the menu affordance does not move with the label.
        const int left = look.alignLeft ? width - parts.pad - parts.chevron : x;
        const RECT chevronRect{left, 0, left + parts.chevron, height};
        HFONT chevronFont = CreateIconFont(button, 7);
        DrawGlyph(dc, chevronFont, kGlyphChevronDown, chevronRect, disabled ? kTextOff : look.kind == ButtonKind::Accent ? ink : kTextDim);
        DeleteObject(chevronFont);
    }

    BitBlt(item.hDC, bounds.left, bounds.top, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

}  // namespace ui
