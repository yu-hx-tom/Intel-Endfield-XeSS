#pragma once
#include <windows.h>
#include <uxtheme.h>
#include <array>
#include <vector>

namespace xess_vk::launcher_ui {
inline constexpr int iconId = 101;
inline constexpr COLORREF canvas = RGB(244, 247, 251), paper = RGB(255, 255, 255),
    ink = RGB(28, 43, 63), muted = RGB(102, 120, 140), edge = RGB(222, 230, 239),
    navy = RGB(13, 31, 51), accent = RGB(0, 111, 180), cyan = RGB(84, 220, 235);
enum class Font { Body, Small, Section, Title };
struct Child { HWND window{}; int x{}, y{}, w{}, h{}; Font font{}; };

class Theme {
    UINT dpi_ = 96;
    std::array<HFONT, 4> fonts_{};
    std::vector<Child> children_;
public:
    HBRUSH background = CreateSolidBrush(canvas), card = CreateSolidBrush(paper),
           field = CreateSolidBrush(RGB(248, 250, 253));
    HICON icon{}, smallIcon{};
    bool development{};
    Theme() = default;
    Theme(const Theme&) = delete;
    ~Theme() {
        for (auto font : fonts_) if (font) DeleteObject(font);
        DeleteObject(background); DeleteObject(card); DeleteObject(field);
        if (icon) DestroyIcon(icon);
        if (smallIcon) DestroyIcon(smallIcon);
    }
    int px(int logical) const { return MulDiv(logical, int(dpi_), 96); }
    HFONT font(Font role = Font::Body) const { return fonts_[size_t(role)]; }
    void setDpi(UINT value) {
        dpi_ = value;
        for (auto handle : fonts_) if (handle) DeleteObject(handle);
        const std::array<int, 4> sizes{14, 12, 15, 24};
        for (size_t i = 0; i < sizes.size(); ++i)
            fonts_[i] = CreateFontW(-px(sizes[i]), 0, 0, 0, i >= 2 ? FW_SEMIBOLD : FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        for (const auto& child : children_) {
            MoveWindow(child.window, px(child.x), px(child.y), px(child.w), px(child.h), TRUE);
            SendMessageW(child.window, WM_SETFONT, WPARAM(font(child.font)), TRUE);
        }
    }
    void add(HWND window, int x, int y, int w, int h, Font role) {
        children_.push_back({window, x, y, w, h, role});
        SendMessageW(window, WM_SETFONT, WPARAM(font(role)), TRUE);
        SetWindowTheme(window, L"Explorer", nullptr);
    }
    void loadIcons(HINSTANCE instance) {
        if (icon) DestroyIcon(icon);
        if (smallIcon) DestroyIcon(smallIcon);
        icon = HICON(LoadImageW(instance, MAKEINTRESOURCEW(iconId), IMAGE_ICON, px(48), px(48), 0));
        smallIcon = HICON(LoadImageW(instance, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
                                  GetSystemMetricsForDpi(SM_CXSMICON, dpi_), GetSystemMetricsForDpi(SM_CYSMICON, dpi_), 0));
    }
    void text(HDC dc, const wchar_t* value, RECT rect, Font role, COLORREF color,
              UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX) const {
        const auto previous = SelectObject(dc, font(role));
        SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, value, -1, &rect, flags);
        SelectObject(dc, previous);
    }
    void rounded(HDC dc, RECT rect, COLORREF fill, COLORREF line, int radius = 12) const {
        const auto brush = CreateSolidBrush(fill);
        const auto pen = CreatePen(PS_SOLID, 1, line);
        const auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, px(radius), px(radius));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
        DeleteObject(brush); DeleteObject(pen);
    }
    RECT rect(int x, int y, int w, int h) const { return {px(x), px(y), px(x + w), px(y + h)}; }
    void paint(HWND window) const {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
        RECT bounds{}; GetClientRect(window, &bounds);
        FillRect(dc, &bounds, background);
        const auto headerBrush = CreateSolidBrush(navy);
        auto header = rect(0, 0, 560, 72); FillRect(dc, &header, headerBrush); DeleteObject(headerBrush);
        if (icon) DrawIconEx(dc, px(22), px(18), icon, px(36), px(36), 0, nullptr, DI_NORMAL);
        text(dc, L"终末地  /  XeSS", rect(74, 9, 350, 34), Font::Title, paper);
        text(dc, L"超分辨率与帧生成", rect(76, 42, 330, 22), Font::Small, RGB(175, 198, 219));
        rounded(dc, rect(442, 24, 98, 24), RGB(22, 59, 78), RGB(36, 94, 112), 12);
        text(dc, development ? L"开发版" : L"精简发行版", rect(442, 24, 98, 24), Font::Small, cyan, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        rounded(dc, rect(16, 84, 528, 100), paper, edge);
        rounded(dc, rect(16, 194, 528, 160), paper, edge);
        rounded(dc, rect(16, 366, 528, 40), paper, edge);
        if (development) rounded(dc, rect(16, 418, 528, 84), paper, edge);
        const auto oldPen = SelectObject(dc, GetStockObject(DC_PEN)); SetDCPenColor(dc, edge);
        const int offset = development ? 96 : 0;
        MoveToEx(dc, px(16), px(516 + offset), nullptr); LineTo(dc, px(544), px(516 + offset));
        SelectObject(dc, oldPen);
        EndPaint(window, &paint);
    }
    bool drawButton(const DRAWITEMSTRUCT& item, bool primary) const {
        if (item.CtlType != ODT_BUTTON) return false;
        const bool enabled = IsWindowEnabled(item.hwndItem), pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool focused = (item.itemState & ODS_FOCUS) != 0;
        const auto dc = item.hDC;
        RECT placement{}; GetWindowRect(item.hwndItem, &placement);
        MapWindowPoints(nullptr, GetParent(item.hwndItem), reinterpret_cast<POINT*>(&placement), 2);
        FillRect(dc, &item.rcItem, placement.top >= px(84) && placement.top < px(development ? 502 : 406) ? card : background);
        RECT bounds = item.rcItem; InflateRect(&bounds, -1, -1);
        const COLORREF fill = !enabled ? RGB(234, 239, 245) : primary ? (pressed ? RGB(0, 85, 145) : accent)
            : pressed ? RGB(230, 240, 250) : paper;
        rounded(dc, bounds, fill, primary && enabled ? fill : edge, 10);
        wchar_t label[128]{}; GetWindowTextW(item.hwndItem, label, int(std::size(label)));
        if (pressed) OffsetRect(&bounds, 0, px(1));
        text(dc, label, bounds, Font::Body, !enabled ? muted : primary ? paper : ink,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (focused && !(item.itemState & ODS_NOFOCUSRECT)) {
            InflateRect(&bounds, -px(5), -px(5)); DrawFocusRect(dc, &bounds);
        }
        return true;
    }
};
}
