#include "ui_theme.h"
#include "win/common.h"
#include <commctrl.h>
#include <cwchar>

namespace wr {
namespace theme {

namespace {
int g_dpi = 96;
HFONT g_body, g_semi, g_title, g_small;
HBRUSH g_bg, g_log;

int S(int v) { return MulDiv(v, g_dpi, 96); }
COLORREF Blend(COLORREF a, COLORREF b, int pct) {   // pct % of b
    auto ch = [&](int sh) { int x = (a >> sh) & 255, y = (b >> sh) & 255; return (COLORREF)(x + (y - x) * pct / 100) << sh; };
    return ch(0) | ch(8) | ch(16);
}

int CALLBACK FontProbe(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) { *(bool*)p = true; return 0; }
bool HasFont(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, FontProbe, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

HFONT MakeFont(const wchar_t* face, int pt, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pt, g_dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

const wchar_t* Prop() { return L"WRHover"; }

LRESULT CALLBACK HoverProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (m) {
        case WM_MOUSEMOVE:
            if (!GetPropW(h, Prop())) {
                SetPropW(h, Prop(), (HANDLE)1);
                TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, h, 0};
                TrackMouseEvent(&te);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(h, Prop());
            InvalidateRect(h, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            RemovePropW(h, Prop());
            RemoveWindowSubclass(h, HoverProc, 1);
            break;
    }
    return DefSubclassProc(h, m, wp, lp);
}

COLORREF kBg = RGB(243, 246, 251), kText = RGB(26, 34, 52);
}  // namespace

void Init(int dpi) {
    g_dpi = dpi;
    const wchar_t* face = HasFont(L"Segoe UI") ? L"Segoe UI" : (HasFont(L"Tahoma") ? L"Tahoma" : L"Arial");
    g_body = MakeFont(face, 9, FW_NORMAL);
    g_semi = MakeFont(face, 9, FW_SEMIBOLD);
    g_title = MakeFont(face, 17, FW_SEMIBOLD);
    g_small = MakeFont(face, 9, FW_NORMAL);
    g_bg = CreateSolidBrush(kBg);
    g_log = CreateSolidBrush(LogBg());
}

HFONT Body() { return g_body ? g_body : (HFONT)GetStockObject(DEFAULT_GUI_FONT); }
HFONT Semi() { return g_semi ? g_semi : Body(); }
HFONT Title() { return g_title ? g_title : Body(); }
HFONT Small() { return g_small ? g_small : Body(); }
HBRUSH BgBrush() { return g_bg ? g_bg : (HBRUSH)(COLOR_BTNFACE + 1); }
HBRUSH LogBrush() { return g_log ? g_log : (HBRUSH)GetStockObject(BLACK_BRUSH); }
COLORREF Text() { return kText; }
COLORREF LogText() { return RGB(214, 226, 244); }
COLORREF LogBg() { return RGB(15, 27, 45); }

COLORREF Accent(Cat c) {
    switch (c) {
        case Cat::Diag: return RGB(31, 122, 224);
        case Cat::Guide: return RGB(12, 166, 120);
        case Cat::Repair: return RGB(232, 89, 12);
        case Cat::Protect: return RGB(47, 158, 68);
        case Cat::Exit: return RGB(201, 42, 42);
        case Cat::Header: return RGB(255, 255, 255);
        default: return RGB(110, 120, 140);
    }
}

void AttachHover(HWND b) { SetWindowSubclass(b, HoverProc, 1, 0); }
bool IsHover(HWND b) { return GetPropW(b, Prop()) != nullptr; }

void DrawButton(const DRAWITEMSTRUCT* d, Cat c, bool hover) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    bool dis = (d->itemState & ODS_DISABLED) != 0, down = (d->itemState & ODS_SELECTED) != 0;
    COLORREF acc = Accent(c);
    wchar_t buf[512] = {0};
    GetWindowTextW(d->hwndItem, buf, 511);

    // area behind the rounded corners
    HBRUSH under = (c == Cat::Header || c == Cat::Exit) ? nullptr : BgBrush();
    if (under) FillRect(dc, &r, under);

    COLORREF fill, border, txt;
    bool filled = (c == Cat::Header || c == Cat::Exit);
    if (c == Cat::Header) {
        COLORREF base = RGB(20, 56, 112);
        fill = Blend(base, RGB(255, 255, 255), down ? 34 : hover ? 26 : 14);
        border = Blend(base, RGB(255, 255, 255), 40);
        txt = RGB(255, 255, 255);
    } else if (c == Cat::Exit) {
        fill = down ? Blend(acc, RGB(0, 0, 0), 25) : hover ? Blend(acc, RGB(255, 255, 255), 14) : acc;
        border = Blend(acc, RGB(0, 0, 0), 25);
        txt = RGB(255, 255, 255);
    } else if (dis) {
        fill = RGB(236, 239, 244); border = RGB(214, 219, 228); txt = RGB(140, 148, 162); acc = RGB(176, 184, 198);
    } else {
        fill = Blend(RGB(255, 255, 255), acc, down ? 22 : hover ? 11 : 0);
        border = hover || down ? acc : Blend(RGB(255, 255, 255), acc, 38);
        txt = kText;
    }
    HBRUSH fb = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(10), S(10));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fb);
    DeleteObject(pen);

    RECT tr = r;
    if (!filled) {   // colour stripe on the left = category
        RECT st = {r.left + S(1), r.top + S(5), r.left + S(1) + S(5), r.bottom - S(5)};
        HBRUSH sb = CreateSolidBrush(acc);
        FillRect(dc, &st, sb);
        DeleteObject(sb);
        tr.left += S(14);
        tr.right -= S(8);
    } else {
        tr.left += S(6);
        tr.right -= S(6);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, txt);
    HGDIOBJ of = SelectObject(dc, Semi());
    UINT fl = DT_WORDBREAK | DT_NOPREFIX | (filled ? DT_CENTER : DT_LEFT);
    RECT calc = tr;
    DrawTextW(dc, buf, -1, &calc, fl | DT_CALCRECT);
    int off = ((tr.bottom - tr.top) - (calc.bottom - calc.top)) / 2;
    if (off > 0) tr.top += off;
    if (down) OffsetRect(&tr, 0, 1);
    DrawTextW(dc, buf, -1, &tr, fl);
    SelectObject(dc, of);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT fr = r;
        InflateRect(&fr, -S(3), -S(3));
        DrawFocusRect(dc, &fr);
    }
}

void PaintHeader(HDC dc, const RECT& rc, HICON icon, const std::wstring& title, const std::wstring& subtitle) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const COLORREF c1 = RGB(10, 26, 54), c2 = RGB(24, 108, 204);
    int strips = 64;
    for (int i = 0; i < strips; ++i) {   // horizontal gradient without msimg32
        RECT s = {rc.left + w * i / strips, rc.top, rc.left + w * (i + 1) / strips + 1, rc.bottom};
        HBRUSH b = CreateSolidBrush(Blend(c1, c2, i * 100 / (strips - 1)));
        FillRect(dc, &s, b);
        DeleteObject(b);
    }
    // four-colour line = the four purposes (same colours as the buttons)
    Cat cats[4] = {Cat::Diag, Cat::Guide, Cat::Repair, Cat::Protect};
    int lh = S(4);
    for (int i = 0; i < 4; ++i) {
        RECT s = {rc.left + w * i / 4, rc.bottom - lh, rc.left + w * (i + 1) / 4, rc.bottom};
        HBRUSH b = CreateSolidBrush(Accent(cats[i]));
        FillRect(dc, &s, b);
        DeleteObject(b);
    }
    int isz = S(44), x = rc.left + S(14), y = rc.top + (h - lh - isz) / 2;
    if (icon) DrawIconEx(dc, x, y, icon, isz, isz, 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    int tx = x + (icon ? isz + S(12) : 0);
    HGDIOBJ of = SelectObject(dc, Title());
    SetTextColor(dc, RGB(255, 255, 255));
    RECT tr = {tx, rc.top + S(7), rc.right - S(220), rc.top + S(36)};
    DrawTextW(dc, title.c_str(), -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, Small());
    SetTextColor(dc, RGB(176, 204, 240));
    RECT sr = {tx, rc.top + S(38), rc.right - S(220), rc.bottom - lh - S(2)};
    DrawTextW(dc, subtitle.c_str(), -1, &sr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

int PaintLegend(HDC dc, int x, int y, int maxWidth) {
    struct Item { Cat c; const wchar_t* t; } items[] = {
        {Cat::Diag, L"Диагностика (только чтение)"},
        {Cat::Guide, L"Мастера и сценарии"},
        {Cat::Repair, L"Изменяет систему (с подтверждением)"},
        {Cat::Protect, L"Защита и сохранение"},
    };
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, Small());
    int cx = x, h = S(18);
    for (auto& it : items) {
        std::wstring t = W(U(std::wstring(it.t)));
        RECT m{0, 0, 0, 0};
        DrawTextW(dc, t.c_str(), -1, &m, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        int need = S(18) + (m.right - m.left) + S(18);
        if (cx + need > x + maxWidth) break;
        RECT sq = {cx, y + S(3), cx + S(12), y + S(15)};
        HBRUSH b = CreateSolidBrush(Accent(it.c));
        HPEN p = CreatePen(PS_SOLID, 1, Accent(it.c));
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        RoundRect(dc, sq.left, sq.top, sq.right, sq.bottom, S(4), S(4));
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(b); DeleteObject(p);
        SetTextColor(dc, RGB(70, 80, 100));
        RECT tr = {cx + S(18), y, cx + need, y + h};
        DrawTextW(dc, t.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        cx += need;
    }
    SelectObject(dc, of);
    return h;
}

}  // namespace theme
}  // namespace wr
