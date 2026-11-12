#pragma once
#include <Windows.h>
#include <CommCtrl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <string>
#include <vector>

namespace qbui {
inline constexpr COLORREF Canvas = RGB(255, 248, 220), Card = RGB(255, 254, 246);
inline constexpr COLORREF Text = RGB(57, 49, 29), Muted = RGB(128, 115, 80), Line = RGB(232, 221, 181);
inline constexpr COLORREF Gold = RGB(229, 183, 65), GoldHover = RGB(239, 199, 92), SoftGold = RGB(255, 242, 190);
inline constexpr COLORREF Error = RGB(165, 58, 43), Green = RGB(68, 115, 79);
inline HWND root;
inline UINT dpi = 96;
inline int scrollY = 0, originX = 0, badgeTone = 0;
inline HFONT body, smallFace, title, brand;
inline HBRUSH cardBrush;
inline constexpr int Width = 800, Height = 928;
struct ControlRect { HWND handle; int x, y, width, height; };
inline std::vector<ControlRect> controls;
inline int Dp(int value) { return MulDiv(value, static_cast<int>(dpi), 96); }
inline Gdiplus::Color Color(COLORREF color) { return {255, GetRValue(color), GetGValue(color), GetBValue(color)}; }
inline RECT Rect(int x, int y, int width, int height) { return {x, y, x + width, y + height}; }
inline void Rounded(HDC dc, RECT rect, float radius, COLORREF fill, COLORREF stroke = CLR_INVALID) {
    using namespace Gdiplus;
    Graphics graphics(dc); graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    float x = static_cast<float>(rect.left) + .5f, y = static_cast<float>(rect.top) + .5f;
    float width = static_cast<float>(rect.right - rect.left - 1), height = static_cast<float>(rect.bottom - rect.top - 1);
    float diameter = std::min({radius * 2, width, height});
    GraphicsPath path; path.AddArc(x, y, diameter, diameter, 180, 90); path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90); path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90); path.CloseFigure();
    SolidBrush brush(Color(fill)); graphics.FillPath(&brush, &path);
    if (stroke != CLR_INVALID) { Pen pen(Color(stroke), 1); graphics.DrawPath(&pen, &path); }
}
inline void Label(HDC dc, const wchar_t* text, RECT rect, HFONT face, COLORREF color, UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, face); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, flags); SelectObject(dc, old);
}
inline void Fonts() {
    for (auto face : {body, smallFace, title, brand}) if (face) DeleteObject(face);
    body = CreateFontW(-Dp(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    smallFace = CreateFontW(-Dp(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    title = CreateFontW(-Dp(17), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    brand = CreateFontW(-Dp(32), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    for (auto item : controls) SendMessageW(item.handle, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
}
inline void Init(HWND window) {
    root = window; dpi = GetDpiForWindow(window); Fonts(); cardBrush = CreateSolidBrush(Card);
    COLORREF caption = Canvas, border = Line;
    DwmSetWindowAttribute(root, 35, &caption, sizeof(caption)); DwmSetWindowAttribute(root, 34, &border, sizeof(border));
}
inline RECT CanvasRect(int x, int y, int width, int height) { return Rect(originX + Dp(x), Dp(y) - scrollY, Dp(width), Dp(height)); }
inline void Layout() {
    RECT client{}; GetClientRect(root, &client);
    originX = std::max<int>(0, (client.right - Dp(Width)) / 2);
    scrollY = std::clamp(scrollY, 0, std::max<int>(0, Dp(Height) - client.bottom));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, Dp(Height) - 1, static_cast<UINT>(client.bottom), scrollY};
    SetScrollInfo(root, SB_VERT, &info, TRUE);
    auto deferred = BeginDeferWindowPos(static_cast<int>(controls.size()));
    for (auto item : controls) deferred = DeferWindowPos(deferred, item.handle, nullptr, originX + Dp(item.x), Dp(item.y) - scrollY,
        Dp(item.width), Dp(item.height), SWP_NOZORDER | SWP_NOACTIVATE);
    if (deferred) EndDeferWindowPos(deferred);
    InvalidateRect(root, nullptr, FALSE);
}
inline void Track(HWND handle, int x, int y, int width, int height) { controls.push_back({handle, x, y, width, height}); }
inline void Scroll(int delta) { scrollY += delta; Layout(); }
inline void PaintCanvas() {
    PAINTSTRUCT paint{}; HDC target = BeginPaint(root, &paint); RECT client{}; GetClientRect(root, &client);
    HDC dc = CreateCompatibleDC(target); auto bitmap = CreateCompatibleBitmap(target, std::max(1L, client.right), std::max(1L, client.bottom));
    auto old = SelectObject(dc, bitmap); auto background = CreateSolidBrush(Canvas); FillRect(dc, &client, background); DeleteObject(background);
    auto card = [&](int x, int y, int width, int height) {
        Rounded(dc, CanvasRect(x, y + 2, width, height), static_cast<float>(Dp(18)), RGB(245, 235, 199));
        Rounded(dc, CanvasRect(x, y, width, height), static_cast<float>(Dp(18)), Card, Line);
    };
    card(24, 96, 752, 100); card(24, 212, 366, 270); card(406, 212, 370, 270);
    card(24, 498, 752, 190); card(24, 704, 752, 82); card(24, 802, 752, 106);
    Rounded(dc, CanvasRect(28, 24, 52, 52), static_cast<float>(Dp(18)), Gold);
    Label(dc, L"Q", CanvasRect(28, 22, 52, 52), brand, Text, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    Label(dc, L"q弹狒狒", CanvasRect(94, 17, 290, 44), brand, Text);
    Label(dc, L"会让所有人变得q弹 · someboy · 主要由 Codex 生成", CanvasRect(96, 60, 520, 24), smallFace, Muted);
    auto badgeColor = badgeTone == 3 ? RGB(255, 232, 219) : badgeTone == 2 ? RGB(231, 242, 218) : SoftGold;
    Rounded(dc, CanvasRect(630, 34, 142, 30), static_cast<float>(Dp(15)), badgeColor);
    const wchar_t* badge = badgeTone == 3 ? L"需要处理" : badgeTone == 2 ? L"Q 弹进行中" : badgeTone == 1 ? L"已连接" : L"等待连接";
    Label(dc, badge, CanvasRect(630, 34, 142, 30), smallFace, badgeTone == 3 ? Error : badgeTone == 2 ? Green : Text, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    Label(dc, L"游戏客户端", CanvasRect(44, 109, 600, 24), smallFace, Muted);
    Label(dc, L"角色范围", CanvasRect(426, 231, 310, 26), title, Text);
    Label(dc, L"自动跟踪当前加载的对象", CanvasRect(426, 259, 312, 22), smallFace, Muted);
    Label(dc, L"背景音乐", CanvasRect(44, 513, 300, 28), title, Text);
    Label(dc, L"F6 开关  ·  F7 强度  ·  F8 重置", CanvasRect(319, 715, 430, 24), smallFace, Muted);
    Label(dc, L"仅在游戏前台且未输入文字时生效；请留意游戏中的同名按键绑定。", CanvasRect(44, 750, 712, 20), smallFace, Muted);
    Rounded(dc, CanvasRect(44, 591, 594, 40), static_cast<float>(Dp(10)), RGB(255, 255, 251), Line);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); EndPaint(root, &paint);
}
inline void SetBadge(int value) {
    if (badgeTone == value) return; badgeTone = value;
    auto rect = CanvasRect(620, 24, 160, 48); InvalidateRect(root, &rect, FALSE);
}
inline LRESULT CALLBACK ControlPaint(HWND handle, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR type) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_MOUSEMOVE) {
        if (!GetPropW(handle, L"QBounceHover")) {
            SetPropW(handle, L"QBounceHover", reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, handle, 0}; TrackMouseEvent(&tracking); InvalidateRect(handle, nullptr, FALSE);
        }
    }
    if (message == WM_MOUSELEAVE) { RemovePropW(handle, L"QBounceHover"); InvalidateRect(handle, nullptr, FALSE); }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE || message == BM_SETCHECK || message == TBM_SETPOS) {
        auto result = DefSubclassProc(handle, message, wparam, lparam); InvalidateRect(handle, nullptr, FALSE); return result;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(handle, &paint); RECT rect{}; GetClientRect(handle, &rect); FillRect(dc, &rect, cardBrush);
        bool enabled = IsWindowEnabled(handle) != FALSE, hover = GetPropW(handle, L"QBounceHover") != nullptr;
        if (type == 1) {
            bool checkbox = (GetWindowLongPtrW(handle, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX;
            bool checked = SendMessageW(handle, BM_GETCHECK, 0, 0) == BST_CHECKED;
            wchar_t text[512]{}; GetWindowTextW(handle, text, 512); auto id = GetDlgCtrlID(handle);
            if (checkbox) {
                auto fill = checked ? SoftGold : hover ? RGB(251, 246, 225) : Card;
                Rounded(dc, Rect(0, 0, rect.right, rect.bottom), static_cast<float>(Dp(10)), fill);
                int trackWidth = Dp(34), trackHeight = Dp(20), x = Dp(10), y = (rect.bottom - trackHeight) / 2;
                Rounded(dc, Rect(x, y, trackWidth, trackHeight), static_cast<float>(Dp(10)), !enabled ? RGB(228, 224, 207) : checked ? Gold : RGB(225, 222, 206));
                Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::SolidBrush knob(Color(RGB(255, 255, 252)));
                graphics.FillEllipse(&knob, x + (checked ? trackWidth - trackHeight + Dp(3) : Dp(3)), y + Dp(3), trackHeight - Dp(6), trackHeight - Dp(6));
                Label(dc, text, Rect(Dp(56), 0, rect.right - Dp(62), rect.bottom), body, enabled ? Text : Muted);
            } else {
                bool primary = id == 102, dark = id == 117;
                COLORREF fill = primary ? (hover ? GoldHover : Gold) : dark ? RGB(83, 70, 39) : hover ? SoftGold : RGB(255, 253, 239);
                Rounded(dc, Rect(0, 0, rect.right, rect.bottom), static_cast<float>(Dp(10)), fill, primary || dark ? CLR_INVALID : Line);
                Label(dc, text, Rect(Dp(8), 0, rect.right - Dp(16), rect.bottom), body, dark ? RGB(255, 250, 228) : Text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        } else if (type == 2) {
            RECT thumb{}, channel{}; SendMessageW(handle, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            SendMessageW(handle, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            int x = (thumb.left + thumb.right) / 2, y = rect.bottom / 2;
            Rounded(dc, Rect(channel.left, y - Dp(3), channel.right - channel.left, Dp(6)), static_cast<float>(Dp(3)), RGB(235, 228, 201));
            Rounded(dc, Rect(channel.left, y - Dp(3), std::max(1L, x - channel.left), Dp(6)), static_cast<float>(Dp(3)), Gold);
            Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush ring(Color(Gold)), knob(Color(RGB(255, 255, 251)));
            graphics.FillEllipse(&ring, x - Dp(9), y - Dp(9), Dp(18), Dp(18));
            graphics.FillEllipse(&knob, x - Dp(5), y - Dp(5), Dp(10), Dp(10));
        }
        if (GetFocus() == handle) { RECT focus = rect; InflateRect(&focus, -Dp(3), -Dp(3)); DrawFocusRect(dc, &focus); }
        EndPaint(handle, &paint); return 0;
    }
    if (message == WM_NCDESTROY) { RemovePropW(handle, L"QBounceHover"); RemoveWindowSubclass(handle, ControlPaint, 1); }
    return DefSubclassProc(handle, message, wparam, lparam);
}
inline void Style(HWND handle, const wchar_t* type) {
    if (!_wcsicmp(type, L"BUTTON")) SetWindowSubclass(handle, ControlPaint, 1, 1);
    else if (!_wcsicmp(type, TRACKBAR_CLASSW)) SetWindowSubclass(handle, ControlPaint, 1, 2);
    else if (!_wcsicmp(type, L"COMBOBOX")) SetWindowTheme(handle, L"Explorer", nullptr);
}
inline void Dispose() {
    controls.clear(); for (auto face : {body, smallFace, title, brand}) if (face) DeleteObject(face);
    if (cardBrush) DeleteObject(cardBrush);
}
}
