// The look of the window: one dark palette, the fonts, the icon glyphs and the
// drawing shared by the toolbar, the status bar and the setup guide.
#pragma once

#include <windows.h>

#include <string>

namespace ui {

// Palette. The title bar, toolbar and status bar share kChrome so they read as
// one frame around the picture.
constexpr COLORREF kChrome = RGB(0x20, 0x20, 0x20);
constexpr COLORREF kSurface = RGB(0x17, 0x17, 0x17);      // idle screen behind the picture
constexpr COLORREF kPanel = RGB(0x26, 0x26, 0x26);        // setup guide
constexpr COLORREF kDivider = RGB(0x33, 0x33, 0x33);
constexpr COLORREF kControl = RGB(0x2D, 0x2D, 0x2D);      // resting button
constexpr COLORREF kControlHot = RGB(0x3A, 0x3A, 0x3A);
constexpr COLORREF kControlDown = RGB(0x30, 0x30, 0x30);
constexpr COLORREF kControlLine = RGB(0x3F, 0x3F, 0x3F);
constexpr COLORREF kText = RGB(0xF3, 0xF3, 0xF3);
constexpr COLORREF kTextDim = RGB(0xB4, 0xB4, 0xB4);
constexpr COLORREF kTextFaint = RGB(0x80, 0x80, 0x80);
constexpr COLORREF kTextOff = RGB(0x66, 0x66, 0x66);      // disabled
constexpr COLORREF kAccent = RGB(0x4C, 0xC2, 0xFF);
constexpr COLORREF kAccentHot = RGB(0x6B, 0xCE, 0xFF);
constexpr COLORREF kAccentText = RGB(0x00, 0x1A, 0x2B);   // text on an accent button
constexpr COLORREF kGood = RGB(0x6C, 0xCB, 0x5F);
constexpr COLORREF kWarn = RGB(0xFC, 0xC4, 0x5A);
constexpr COLORREF kBad = RGB(0xFF, 0x6B, 0x62);
constexpr COLORREF kRecord = RGB(0xE8, 0x3B, 0x30);
constexpr COLORREF kRecordHot = RGB(0xF2, 0x55, 0x4A);

// Glyphs present in both Segoe Fluent Icons (Windows 11) and Segoe MDL2 Assets (Windows 10).
constexpr wchar_t kGlyphMenu = 0xE700;
constexpr wchar_t kGlyphBluetooth = 0xE702;
constexpr wchar_t kGlyphChevronDown = 0xE70D;
constexpr wchar_t kGlyphClose = 0xE711;
constexpr wchar_t kGlyphStop = 0xE71A;
constexpr wchar_t kGlyphCamera = 0xE722;
constexpr wchar_t kGlyphCheck = 0xE73E;
constexpr wchar_t kGlyphBackToWindow = 0xE73F;
constexpr wchar_t kGlyphFullScreen = 0xE740;
constexpr wchar_t kGlyphRecord = 0xE7C8;
constexpr wchar_t kGlyphHome = 0xE80F;
constexpr wchar_t kGlyphPhone = 0xE8EA;

enum class ButtonKind { Plain, Subtle, Accent, Record };

// How one owner-drawn button looks right now. The label is the button's window
// text, which also names it for screen readers and automation.
struct ButtonLook {
    ButtonKind kind = ButtonKind::Plain;
    wchar_t glyph = 0;          // icon before the label
    bool showLabel = true;      // false: icon only, the label stays as the accessible name
    bool chevron = false;       // opens a menu
    COLORREF dot = CLR_INVALID; // state dot before the label
    bool alignLeft = false;
};

// Process-wide setup: drawing library and dark popup menus. Call once each.
void Startup();
void Shutdown();

// Dark title bar and border in the chrome colour.
void StyleWindow(HWND window);

int Dip(HWND window, int value);

// Fonts for `window`'s current DPI. The caller owns the handle.
HFONT CreateTextFont(HWND window, int points, bool semibold);
HFONT CreateIconFont(HWND window, int points);

// Makes `button` report hover so DrawButton can show it, and records the colour
// of the surface behind it for its rounded corners. Call once per button.
void TrackHover(HWND button, COLORREF behind);
bool IsHot(HWND button);

void DrawButton(const DRAWITEMSTRUCT& item, const ButtonLook& look, HFONT textFont, HFONT iconFont);

// Width a button needs for its glyph, dot, label and chevron.
int ButtonWidth(HWND button, const ButtonLook& look, HFONT textFont);

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF line = CLR_INVALID);
void FillCircle(HDC dc, int centerX, int centerY, int radius, COLORREF fill);
void DrawGlyph(HDC dc, HFONT iconFont, wchar_t glyph, const RECT& rect, COLORREF color);

}  // namespace ui
