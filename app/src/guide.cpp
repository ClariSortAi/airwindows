#include "guide.h"

#include <algorithm>

#include "ui.h"

namespace {

constexpr wchar_t kGuideClass[] = L"AirWindowsGuide";
constexpr int kSteps = 5;
constexpr int kCloseButton = 1, kPairButton = 2, kDoneButton = 3, kPhoneButton = 10;

struct TextBlock {
    RECT rect;
    std::wstring text;
    HFONT font;
    COLORREF color;
};

struct Badge {
    int centerX, centerY;
    int number;
    bool done, current;
};

}  // namespace

bool GuideState::operator==(const GuideState& other) const {
    return receiverName == other.receiverName && addresses == other.addresses && mirroring == other.mirroring &&
           mirrorDevice == other.mirrorDevice && pairingOpen == other.pairingOpen && phones == other.phones &&
           chosen == other.chosen && ready == other.ready && controlled == other.controlled &&
           bluetoothStatus == other.bluetoothStatus;
}

struct Guide::Impl {
    HWND window = nullptr;
    HWND closeButton = nullptr, pairButton = nullptr, doneButton = nullptr;
    HWND phoneButtons[Guide::kMaxPhones] = {};
    Guide::Commands commands;
    GuideState state;
    HFONT text = nullptr, bold = nullptr, title = nullptr, icons = nullptr;
    std::vector<TextBlock> blocks;
    std::vector<Badge> badges;
    bool done[kSteps] = {};

    int Dip(int value) const { return ui::Dip(window, value); }

    void Place(HWND button, int x, int y, int width, int height) {
        MoveWindow(button, x, y, width, height, TRUE);
        ShowWindow(button, SW_SHOWNA);
    }

    // Measures `words` wrapped to `width` and queues them at `y`. Returns their height.
    int Add(HDC dc, const std::wstring& words, HFONT font, COLORREF color, int x, int y, int width) {
        if (words.empty()) {
            return 0;
        }
        HGDIOBJ old = SelectObject(dc, font);
        RECT measured{x, y, x + width, y};
        DrawTextW(dc, words.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        blocks.push_back({RECT{x, y, x + width, measured.bottom}, words, font, color});
        return measured.bottom - y;
    }

    // Works out where every line, badge and button goes for the current state.
    // Painting then only draws what was laid out here.
    void Relayout() {
        if (!window || !text) {
            return;
        }
        blocks.clear();
        badges.clear();
        for (HWND button : {pairButton, doneButton, phoneButtons[0], phoneButtons[1], phoneButtons[2]}) {
            ShowWindow(button, SW_HIDE);
        }
        RECT client{};
        GetClientRect(window, &client);
        const int margin = Dip(18);
        const int badge = Dip(22);
        const int textLeft = margin + badge + Dip(12);
        const int textWidth = std::max(Dip(120), static_cast<int>(client.right) - textLeft - margin);
        const int buttonHeight = Dip(32);
        HDC dc = GetDC(window);

        done[0] = !state.addresses.empty();
        done[1] = state.mirroring;
        done[2] = !state.phones.empty() || state.chosen || state.ready;
        done[3] = state.ready;
        done[4] = state.controlled;
        int current = kSteps;
        for (int i = kSteps - 1; i >= 0; --i) {
            if (!done[i]) {
                current = i;
            }
        }

        const std::wstring name = state.receiverName.empty() ? L"this PC" : state.receiverName;
        const std::wstring titles[kSteps] = {
            L"Put this PC on your network",
            L"Mirror your iPhone",
            L"Pair Bluetooth for mouse and keyboard",
            L"Choose your phone",
            L"Take control",
        };
        const std::wstring bodies[kSteps] = {
            L"Connect this PC to the same Wi-Fi or wired network as your iPhone. AirWindows notices as soon as it "
            L"is connected.",
            L"On the iPhone, open Control Center, tap Screen Mirroring and choose “" + name +
                L"”.\n\nNot in the list? Put the iPhone on the same network as this PC, and allow AirWindows "
                L"through Windows Firewall on private networks.",
            L"1. On the iPhone, turn on Settings › Accessibility › Touch › AssistiveTouch.\n"
            L"2. Click Pair Bluetooth below.\n"
            L"3. On the iPhone, open Settings › Bluetooth and tap this PC under Other Devices.",
            state.chosen ? L"Start Screen Mirroring from that phone to finish this step."
                         : L"Pick the phone you are mirroring.",
            L"Click a button in the phone picture, then type. Click any other window to give the keyboard back to "
            L"Windows.\n\nFor a Home button: on the iPhone, open AssistiveTouch › Devices › this PC › "
            L"Customize Additional Buttons, press Home in the toolbar here, and assign Button 2 to Home.",
        };
        const std::wstring results[kSteps] = {
            L"On " + state.addresses,
            state.mirrorDevice.empty() ? L"Mirroring" : L"Mirroring " + state.mirrorDevice,
            state.phones.empty() ? L"Paired" : L"Paired with " + state.phones.front(),
            L"Connected to the phone you are mirroring",
            L"You have controlled the phone from this PC",
        };

        int y = Dip(16);
        y += Add(dc, L"Set up AirWindows", title, ui::kText, margin, y, client.right - margin * 2 - Dip(36)) + Dip(4);
        y += Add(dc, L"Each step ticks itself off as it happens.", text, ui::kTextFaint, margin, y,
                 client.right - margin * 2) + Dip(18);

        for (int i = 0; i < kSteps; ++i) {
            if (i == 2) {
                // Mirroring works without the rest; say so before asking for more.
                y += Add(dc, L"To control an iPhone from this PC:", text, ui::kTextFaint, margin, y,
                         client.right - margin * 2) + Dip(12);
            }
            const bool isCurrent = i == current;
            badges.push_back({margin + badge / 2, y + badge / 2, i + 1, done[i], isCurrent});
            const int top = y + Dip(2);
            int height = Add(dc, titles[i], bold, done[i] || isCurrent ? ui::kText : ui::kTextFaint, textLeft, top, textWidth);
            if (done[i]) {
                height += Dip(3);
                height += Add(dc, results[i], text, ui::kTextDim, textLeft, top + height, textWidth);
            } else if (isCurrent) {
                height += Dip(6);
                height += Add(dc, bodies[i], text, ui::kTextDim, textLeft, top + height, textWidth);
                if (i == 2) {
                    if (!state.bluetoothStatus.empty()) {
                        height += Dip(8);
                        height += Add(dc, state.bluetoothStatus, text, state.pairingOpen ? ui::kAccent : ui::kWarn,
                                      textLeft, top + height, textWidth);
                    }
                    height += Dip(10);
                    Place(pairButton, textLeft, top + height, std::min(textWidth, Dip(170)), buttonHeight);
                    height += buttonHeight;
                } else if (i == 3 && !state.chosen) {
                    for (size_t phone = 0; phone < state.phones.size() && phone < Guide::kMaxPhones; ++phone) {
                        height += Dip(8);
                        SetWindowTextW(phoneButtons[phone], state.phones[phone].c_str());
                        Place(phoneButtons[phone], textLeft, top + height, std::min(textWidth, Dip(220)), buttonHeight);
                        height += buttonHeight;
                    }
                }
            }
            y = top + std::max(height, badge) + Dip(16);
        }

        y += Dip(4);
        if (current == kSteps) {
            y += Add(dc, L"All set. Reopen this guide any time from Menu › Help.", text, ui::kTextDim, margin, y,
                     client.right - margin * 2) + Dip(12);
        }
        SetWindowTextW(doneButton, current == kSteps ? L"Finish" : L"Close guide");
        Place(doneButton, margin, y, Dip(120), buttonHeight);

        MoveWindow(closeButton, client.right - Dip(42), Dip(10), Dip(32), Dip(32), TRUE);
        ReleaseDC(window, dc);
        InvalidateRect(window, nullptr, FALSE);
    }

    void Paint() {
        PAINTSTRUCT paint;
        HDC target = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom));
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        HBRUSH background = CreateSolidBrush(ui::kPanel);
        FillRect(dc, &client, background);
        DeleteObject(background);
        const RECT divider{0, 0, 1, client.bottom};
        HBRUSH line = CreateSolidBrush(ui::kDivider);
        FillRect(dc, &divider, line);
        DeleteObject(line);
        SetBkMode(dc, TRANSPARENT);

        for (const TextBlock& block : blocks) {
            HGDIOBJ old = SelectObject(dc, block.font);
            SetTextColor(dc, block.color);
            RECT rect = block.rect;
            DrawTextW(dc, block.text.c_str(), -1, &rect, DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, old);
        }
        const int radius = Dip(11);
        for (const Badge& badge : badges) {
            if (badge.done) {
                ui::FillCircle(dc, badge.centerX, badge.centerY, radius, ui::kGood);
                HFONT check = ui::CreateIconFont(window, 9);
                const RECT rect{badge.centerX - radius, badge.centerY - radius, badge.centerX + radius, badge.centerY + radius};
                ui::DrawGlyph(dc, check, ui::kGlyphCheck, rect, ui::kChrome);
                DeleteObject(check);
            } else {
                ui::FillCircle(dc, badge.centerX, badge.centerY, radius, badge.current ? ui::kAccent : ui::kControlLine);
                ui::FillCircle(dc, badge.centerX, badge.centerY, radius - Dip(2), ui::kPanel);
                HGDIOBJ old = SelectObject(dc, bold);
                SetTextColor(dc, badge.current ? ui::kAccent : ui::kTextFaint);
                wchar_t digit[2] = {static_cast<wchar_t>(L'0' + badge.number), 0};
                RECT rect{badge.centerX - radius, badge.centerY - radius, badge.centerX + radius, badge.centerY + radius};
                DrawTextW(dc, digit, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, old);
            }
        }

        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window, &paint);
    }

    void DrawItem(const DRAWITEMSTRUCT& item) {
        ui::ButtonLook look;
        if (item.hwndItem == closeButton) {
            look.kind = ui::ButtonKind::Subtle;
            look.glyph = ui::kGlyphClose;
            look.showLabel = false;
        } else if (item.hwndItem == pairButton) {
            look.kind = ui::ButtonKind::Accent;
            look.glyph = ui::kGlyphBluetooth;
        } else if (item.hwndItem == doneButton) {
            const bool all = std::all_of(std::begin(done), std::end(done), [](bool step) { return step; });
            look.kind = all ? ui::ButtonKind::Accent : ui::ButtonKind::Plain;
        } else {
            look.kind = ui::ButtonKind::Plain;
            look.glyph = ui::kGlyphPhone;
            look.alignLeft = true;
        }
        ui::DrawButton(item, look, text, icons);
    }

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window = window;
        }
        if (!self) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            self->Paint();
            return 0;
        case WM_SIZE:
            self->Relayout();
            return 0;
        case WM_DRAWITEM:
            self->DrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_COMMAND: {
            if (HIWORD(wParam) != BN_CLICKED) {
                break;
            }
            const int id = LOWORD(wParam);
            int command = 0;
            if (id == kPairButton) {
                command = self->commands.pair;
            } else if (id == kCloseButton || id == kDoneButton) {
                command = self->commands.close;
            } else if (id >= kPhoneButton && id < kPhoneButton + Guide::kMaxPhones) {
                command = self->commands.firstPhone + (id - kPhoneButton);
            }
            if (command) {
                SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
            }
            return 0;
        }
        case WM_NCDESTROY:
            self->window = nullptr;
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
};

Guide::Guide() : impl_(std::make_unique<Impl>()) {}

Guide::~Guide() {
    if (impl_->window) {
        DestroyWindow(impl_->window);
    }
}

bool Guide::Create(HWND parent, HINSTANCE instance, const Commands& commands) {
    Impl& s = *impl_;
    s.commands = commands;
    WNDCLASSEXW panel{sizeof(WNDCLASSEXW)};
    panel.lpfnWndProc = Impl::Proc;
    panel.hInstance = instance;
    panel.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    panel.lpszClassName = kGuideClass;
    RegisterClassExW(&panel);  // fails harmlessly when a second panel registers it again
    if (!CreateWindowExW(0, kGuideClass, L"Setup guide", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, nullptr,
                         instance, &s)) {
        return false;
    }
    const auto button = [&](const wchar_t* label, int id) {
        HWND handle = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | BS_OWNERDRAW, 0, 0, 1, 1, s.window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        ui::TrackHover(handle, ui::kPanel);
        return handle;
    };
    s.closeButton = button(L"Close setup guide", kCloseButton);
    s.pairButton = button(L"Pair Bluetooth", kPairButton);
    s.doneButton = button(L"Close guide", kDoneButton);
    for (int i = 0; i < kMaxPhones; ++i) {
        s.phoneButtons[i] = button(L"", kPhoneButton + i);
    }
    ShowWindow(s.closeButton, SW_SHOWNA);
    return true;
}

HWND Guide::Window() const {
    return impl_->window;
}

void Guide::SetFonts(HFONT text, HFONT bold, HFONT title, HFONT icons) {
    impl_->text = text;
    impl_->bold = bold;
    impl_->title = title;
    impl_->icons = icons;
    impl_->Relayout();
}

void Guide::Update(const GuideState& state) {
    if (impl_->state == state && !impl_->blocks.empty()) {
        return;
    }
    impl_->state = state;
    impl_->Relayout();
}

bool Guide::Complete() const {
    return std::all_of(std::begin(impl_->done), std::end(impl_->done), [](bool done) { return done; });
}

int Guide::Width(HWND reference) {
    return ui::Dip(reference, 340);
}
