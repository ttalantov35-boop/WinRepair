#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui_theme.h"
#include "win/common.h"
#include <commctrl.h>
#include <cwchar>
#include <algorithm>

namespace wr {
namespace theme {

namespace {
int g_dpi = 96;
HFONT g_body, g_semi, g_title, g_small, g_big;
HBRUSH g_bg, g_log, g_tint;

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

HFONT MakeFont(const wchar_t* face, int pt10, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pt10, g_dpi, 720);
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

COLORREF kBg = RGB(255, 255, 255), kText = RGB(26, 34, 52);
}  // namespace

void Init(int dpi) {
    g_dpi = dpi;
    const wchar_t* face = HasFont(L"Segoe UI") ? L"Segoe UI" : (HasFont(L"Tahoma") ? L"Tahoma" : L"Arial");
    g_body = MakeFont(face, 95, FW_NORMAL);
    g_semi = MakeFont(face, 95, FW_SEMIBOLD);
    g_title = MakeFont(face, 170, FW_SEMIBOLD);
    g_small = MakeFont(face, 90, FW_NORMAL);
    g_big = MakeFont(face, 105, FW_BOLD);
    g_tint = CreateSolidBrush(TintColor());
    g_bg = CreateSolidBrush(kBg);
    g_log = CreateSolidBrush(LogBg());
}

HFONT Body() { return g_body ? g_body : (HFONT)GetStockObject(DEFAULT_GUI_FONT); }
HFONT Semi() { return g_semi ? g_semi : Body(); }
HFONT Title() { return g_title ? g_title : Body(); }
HFONT Small() { return g_small ? g_small : Body(); }
HFONT Big() { return g_big ? g_big : Semi(); }
HBRUSH TintBrush() { return g_tint ? g_tint : BgBrush(); }
COLORREF TintColor() { return RGB(228, 239, 253); }
COLORREF LevelColor(int level, bool* bold) {
    if (bold) *bold = (level == 2 || level == 5);
    switch (level) {
        case 1: return RGB(176, 84, 0);      // warning: dark amber
        case 2: return RGB(200, 36, 36);     // error
        case 3: return RGB(88, 100, 122);    // raw tool output: quieter
        case 4: return RGB(24, 128, 70);     // ok
        case 5: return RGB(20, 90, 180);     // section heading
        default: return RGB(27, 36, 51);     // info
    }
}
HBRUSH BgBrush() { return g_bg ? g_bg : (HBRUSH)(COLOR_BTNFACE + 1); }
HBRUSH LogBrush() { return g_log ? g_log : (HBRUSH)GetStockObject(BLACK_BRUSH); }
COLORREF Text() { return kText; }
COLORREF LogText() { return RGB(27, 36, 51); }
COLORREF LogBg() { return RGB(252, 253, 255); }

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
    if (!filled) {   // colour bar under the text = category
        int bh = down || hover ? S(4) : S(3);
        RECT st = {r.left + S(12), r.bottom - S(6) - bh + S(3), r.right - S(12), r.bottom - S(6) + S(3)};
        HBRUSH sb = CreateSolidBrush(acc);
        FillRect(dc, &st, sb);
        DeleteObject(sb);
        tr.left += S(12);
        tr.right -= S(10);
        tr.top += S(2);
        tr.bottom -= S(9);
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

void PaintHeader(HDC dc, const RECT& rc, HICON icon, const std::wstring& title, const std::wstring& subtitle, const std::vector<Chip>& chips) {
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
    int isz = S(48), x = rc.left + S(14), y = rc.top + (h - lh - isz) / 2;
    if (icon) DrawIconEx(dc, x, y, icon, isz, isz, 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    int tx = x + (icon ? isz + S(14) : 0), right = rc.right - S(230);
    HGDIOBJ of = SelectObject(dc, Title());
    SetTextColor(dc, RGB(255, 255, 255));
    RECT tr = {tx, rc.top + S(6), right, rc.top + S(38)};
    DrawTextW(dc, title.c_str(), -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    // second line: OS name (bright, semibold) followed by status chips
    int ly = rc.top + S(40), lhgt = S(22);
    SelectObject(dc, Semi());
    SetTextColor(dc, RGB(240, 247, 255));
    RECT m{0, 0, 0, 0};
    DrawTextW(dc, subtitle.c_str(), -1, &m, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    int sw = std::min<int>(m.right - m.left, (right - tx) * 55 / 100);
    RECT sr = {tx, ly, tx + sw, ly + lhgt};
    DrawTextW(dc, subtitle.c_str(), -1, &sr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    int cx = tx + sw + S(12);
    for (const Chip& c : chips) {
        RECT cm{0, 0, 0, 0};
        DrawTextW(dc, c.text.c_str(), -1, &cm, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        int cw = (cm.right - cm.left) + S(18);
        if (cx + cw > right) break;
        COLORREF fill = c.kind == 1 ? RGB(33, 160, 90) : c.kind == 2 ? RGB(214, 48, 49) : Blend(RGB(18, 52, 104), RGB(255, 255, 255), 24);
        HBRUSH fb = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, fill);
        HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, pen);
        RoundRect(dc, cx, ly, cx + cw, ly + lhgt, lhgt, lhgt);   // pill
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(fb); DeleteObject(pen);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT ct = {cx, ly, cx + cw, ly + lhgt};
        DrawTextW(dc, c.text.c_str(), -1, &ct, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        cx += cw + S(6);
    }
    SelectObject(dc, of);
}

void PaintTargetPanel(HDC dc, const RECT& r) {
    HBRUSH fb = CreateSolidBrush(TintColor());
    HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(31, 122, 224));
    HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(12), S(12));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(fb); DeleteObject(pen);
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

void DrawTab(const DRAWITEMSTRUCT* d, const wchar_t* text, Cat c, bool selected) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    COLORREF acc = Accent(c);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);   // the rounded shape must not spill into the page below
    FillRect(dc, &r, BgBrush());
    RECT t = r;
    InflateRect(&t, -S(2), 0);
    t.top += S(3);
    t.bottom -= selected ? 0 : S(2);
    COLORREF fill = selected ? acc : RGB(238, 242, 248);
    HBRUSH fb = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, selected ? acc : RGB(214, 221, 232));
    HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, pen);
    RoundRect(dc, t.left, t.top, t.right, t.bottom + S(10), S(12), S(12));   // bottom corners hidden under the page border
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(fb); DeleteObject(pen);
    if (!selected) {   // colour hint under inactive tabs
        RECT u = {t.left + S(12), t.bottom - S(5), t.right - S(12), t.bottom - S(2)};
        HBRUSH ub = CreateSolidBrush(Blend(RGB(238, 242, 248), acc, 70));
        FillRect(dc, &u, ub);
        DeleteObject(ub);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, selected ? RGB(255, 255, 255) : RGB(40, 52, 74));
    HGDIOBJ of = SelectObject(dc, selected ? Big() : Semi());
    RECT tr = t;
    InflateRect(&tr, -S(4), 0);
    tr.bottom -= S(4);
    RECT cal = tr;
    DrawTextW(dc, text, -1, &cal, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    if (cal.right - cal.left <= tr.right - tr.left)
        DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    else {   // a long caption wraps to two lines instead of being cut off
        RECT w = tr;
        DrawTextW(dc, text, -1, &w, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
        int hgt = w.bottom - w.top;
        tr.top += std::max(0, (int)((tr.bottom - tr.top - hgt) / 2));
        DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, of);
    RestoreDC(dc, saved);
}

void DrawInfoCard(const DRAWITEMSTRUCT* d, const std::wstring& title, const std::wstring& text, Cat c, const std::wstring& catLabel) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    FillRect(dc, &r, BgBrush());
    COLORREF acc = Accent(c);
    HBRUSH fb = CreateSolidBrush(TintColor());
    HPEN pen = CreatePen(PS_SOLID, 1, Blend(TintColor(), acc, 45));
    HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right - 1, r.bottom - 1, S(12), S(12));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(fb); DeleteObject(pen);
    RECT bar = {r.left + S(1), r.top + S(10), r.left + S(6), r.bottom - S(11)};
    HBRUSH bb = CreateSolidBrush(acc);
    FillRect(dc, &bar, bb);
    DeleteObject(bb);
    SetBkMode(dc, TRANSPARENT);
    int x = r.left + S(18), right = r.right - S(14), y = r.top + S(8);
    if (!title.empty()) {
        HGDIOBJ of = SelectObject(dc, Small());
        SetTextColor(dc, acc);
        RECT lm{0, 0, 0, 0};
        DrawTextW(dc, catLabel.c_str(), -1, &lm, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        int lw = lm.right - lm.left;
        RECT lr = {right - lw, y + S(3), right, y + S(21)};
        DrawTextW(dc, catLabel.c_str(), -1, &lr, DT_SINGLELINE | DT_NOPREFIX | DT_RIGHT);
        SelectObject(dc, Big());
        SetTextColor(dc, RGB(16, 30, 58));
        RECT tr = {x, y, right - lw - S(12), y + S(24)};
        DrawTextW(dc, title.c_str(), -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        y += S(26);
        SelectObject(dc, of);
    }
    HGDIOBJ of2 = SelectObject(dc, Body());
    SetTextColor(dc, title.empty() ? RGB(88, 100, 122) : RGB(34, 44, 64));
    RECT dr = {x, title.empty() ? r.top + S(28) : y, right, r.bottom - S(6)};
    DrawTextW(dc, text.c_str(), -1, &dr, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of2);
}

}  // namespace theme
}  // namespace wr
