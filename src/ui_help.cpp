// Справка WinRepair: окно «содержание + текст + поиск»; открывается по F1 и кнопкой «Справка».
// Текст встроен в exe (help\help.ru.txt и help\help.en.txt → RCDATA 3 и 4).
//
// Защита от подмены: обычная (выпускаемая) сборка показывает ТОЛЬКО встроенный текст — файлы рядом с программой
// не читаются, кнопки правки нет. Поэтому изменить справку в официальном WinRepair.exe, не пересобирая его,
// нельзя; а пересобранный exe — уже не официальная сборка Tynychsoft (другая SHA-256, нет подписи).
//
// Режим автора: сборка с WR_HELP_EDIT (build_msvc.bat edit) читает Help\help.<язык>.txt рядом с exe, если он
// есть, и показывает кнопку «Изменить справку…» (создаёт файл из встроенного текста и открывает в Блокноте)
// и «Обновить». Готовый текст переносится в help\help.ru.txt исходников и попадает в exe при обычной сборке.
#include "ui_tools_common.h"
#include <richedit.h>
#include <shellapi.h>

namespace wr {
namespace ui {

namespace {

enum { ID_TOC = 9301, ID_TEXT, ID_FIND, ID_BFIND, ID_BEDIT, ID_BRELOAD, ID_BCLOSE, ID_SRC };

#ifdef WR_HELP_EDIT
constexpr bool kAuthorMode = true;    // сборка автора: справку можно править файлом рядом с exe
#else
constexpr bool kAuthorMode = false;   // выпускаемая сборка: только встроенный текст
#endif

struct HelpSec { std::wstring title, anchor; int level = 1; LONG pos = 0; };

struct HelpWnd {
    HWND h = nullptr, toc = nullptr, text = nullptr, find = nullptr, bFind = nullptr, bEdit = nullptr, bReload = nullptr, bClose = nullptr, src = nullptr;
    bool rich = false;
    std::vector<HelpSec> secs;
    std::wstring plain;     // текст в окне в том виде, как его видит элемент (для поиска и позиций)
    std::string srcPath;    // файл, из которого взят текст ("" — встроенный), UTF-8
    HFONT hFont = nullptr;
};
HelpWnd HW;

const char* LangCode() { return CurrentLang() == Lang::EN ? "en" : "ru"; }

std::string EmbeddedHelp(bool en) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(en ? 4 : 3), RT_RCDATA);
    if (!r) return "";
    HGLOBAL g = LoadResource(nullptr, r);
    DWORD n = SizeofResource(nullptr, r);
    const char* p = g ? (const char*)LockResource(g) : nullptr;
    return p ? std::string(p, n) : std::string();
}

std::string ReadUtf8File(const std::string& path) {
    std::vector<uint8_t> b;
    if (!ReadBytes(path, b, 8u << 20)) return "";
    std::string s(b.begin(), b.end());
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

// Где лежит редактируемая справка: рядом с программой, иначе в Data (если папка программы только для чтения).
std::string HelpFileFor(const char* code, bool forWrite) {
    std::string name = std::string("help.") + code + ".txt";
    std::string a = ExeDir() + "Help\\" + name, b = DataDir() + "Help\\" + name;
    if (!forWrite) return PathExists(a) ? a : PathExists(b) ? b : "";
    return a;
}

// Текст справки на языке интерфейса. Выпускаемая сборка — только встроенный текст; сборка автора — сначала файл.
std::string LoadHelpText(std::string& from) {
    from.clear();
    bool en = CurrentLang() == Lang::EN;
    if (kAuthorMode) {
        std::string f = HelpFileFor(LangCode(), false);
        if (!f.empty()) { std::string t = ReadUtf8File(f); if (!t.empty()) { from = f; return t; } }
    }
    std::string t = EmbeddedHelp(en);
    if (t.empty() && en) t = EmbeddedHelp(false);
    return t;
}

std::wstring StripMarks(std::wstring s) {
    for (const wchar_t* m : {L"**", L"`"}) {
        size_t n = wcslen(m);
        for (size_t p; (p = s.find(m)) != std::wstring::npos;) s.erase(p, n);
    }
    return s;
}

// Разбор файла справки в текст для окна + список разделов с позициями.
void BuildText(const std::string& utf8, std::wstring& out, std::vector<HelpSec>& secs, std::vector<std::pair<LONG, LONG>>& heads1,
               std::vector<std::pair<LONG, LONG>>& heads2, bool rich) {
    const std::wstring nl = rich ? L"\r" : L"\r\n";   // RichEdit хранит перевод строки одним символом
    out.clear();
    secs.clear();
    std::wstring all = WRaw(utf8);
    size_t p = 0;
    bool lastBlank = true;
    while (p <= all.size()) {
        size_t e = all.find(L'\n', p);
        std::wstring line = all.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
        p = e == std::wstring::npos ? all.size() + 1 : e + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        if (!line.empty() && line[0] == L';') continue;   // комментарий
        if (line.empty()) { if (!lastBlank) { out += nl; lastBlank = true; } continue; }
        int level = 0;
        if (line.compare(0, 3, L"## ") == 0) level = 2;
        else if (line.compare(0, 2, L"# ") == 0) level = 1;
        if (level) {
            std::wstring t = line.substr(level + 1), anchor;
            size_t a = t.find(L"{#");
            if (a != std::wstring::npos) {
                size_t z = t.find(L'}', a);
                anchor = t.substr(a + 2, z == std::wstring::npos ? std::wstring::npos : z - a - 2);
                t = t.substr(0, a);
            }
            while (!t.empty() && t.back() == L' ') t.pop_back();
            t = StripMarks(t);
            if (!out.empty() && !lastBlank) out += nl;
            if (level == 1 && !out.empty()) out += nl;   // отступ перед разделом
            LONG pos = (LONG)out.size();
            secs.push_back({t, anchor, level, pos});
            out += t;
            (level == 1 ? heads1 : heads2).push_back({pos, (LONG)out.size()});
            out += nl;
            lastBlank = false;
            continue;
        }
        if (line.compare(0, 2, L"- ") == 0) line = L"   \x2022  " + StripMarks(line.substr(2));
        else line = StripMarks(line);
        out += line;
        out += nl;
        lastBlank = false;
    }
}

void ApplyFormat(const std::vector<std::pair<LONG, LONG>>& h1, const std::vector<std::pair<LONG, LONG>>& h2) {
    if (!HW.rich) return;
    SendMessageW(HW.text, WM_SETREDRAW, FALSE, 0);
    auto fmtRange = [](LONG a, LONG b, int sizeTw, COLORREF col) {
        CHARRANGE cr{a, b};
        SendMessageW(HW.text, EM_EXSETSEL, 0, (LPARAM)&cr);
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_BOLD | CFM_SIZE | CFM_COLOR;
        cf.dwEffects = CFE_BOLD;
        cf.yHeight = sizeTw;
        cf.crTextColor = col;
        SendMessageW(HW.text, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    };
    for (auto& r : h1) fmtRange(r.first, r.second, 15 * 20, RGB(16, 78, 168));
    for (auto& r : h2) fmtRange(r.first, r.second, 11 * 20, RGB(30, 40, 60));
    CHARRANGE z{0, 0};
    SendMessageW(HW.text, EM_EXSETSEL, 0, (LPARAM)&z);
    SendMessageW(HW.text, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(HW.text, nullptr, TRUE);
}

// Показать позицию pos первой строкой окна.
void ScrollTo(LONG pos, LONG selEnd = -1) {
    if (selEnd < 0) selEnd = pos;
    SendMessageW(HW.text, EM_SETSEL, (WPARAM)pos, (LPARAM)selEnd);
    LONG line = (LONG)SendMessageW(HW.text, EM_LINEFROMCHAR, (WPARAM)pos, 0);
    LONG first = (LONG)SendMessageW(HW.text, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(HW.text, EM_LINESCROLL, 0, (LPARAM)(line - first));
}

void Load() {
    std::string utf8 = LoadHelpText(HW.srcPath);
    if (utf8.empty()) utf8 = U(TrW(L"# Справка не найдена\nВстроенная справка отсутствует, файла Help\\help.ru.txt рядом с программой тоже нет."));
    std::vector<std::pair<LONG, LONG>> h1, h2;
    BuildText(utf8, HW.plain, HW.secs, h1, h2, HW.rich);
    SendMessageW(HW.text, HW.rich ? EM_EXLIMITTEXT : EM_SETLIMITTEXT, 0, 0x7FFFFFFE);
    SendMessageW(HW.text, WM_SETTEXT, 0, (LPARAM)HW.plain.c_str());   // напрямую: текст справки не переводится словарём
    ApplyFormat(h1, h2);
    SendMessageW(HW.toc, LB_RESETCONTENT, 0, 0);
    for (auto& s : HW.secs) {
        std::wstring t = (s.level == 2 ? L"      " : L"") + s.title;
        SendMessageW(HW.toc, LB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    std::wstring src = !HW.srcPath.empty() ? L"Режим автора — файл справки: " + WRaw(HW.srcPath)
                     : kAuthorMode ? std::wstring(L"Режим автора — встроенная справка. Чтобы изменить текст, нажмите «Изменить справку…».")
                                   : W(std::string("Справка встроена в программу — ") + WR_COMPANY + ", версия " + kAppVersion + ".");
    SetWindowTextW(HW.src, src.c_str());
}

void GoTo(const std::string& topic) {
    if (topic.empty() || HW.secs.empty()) { ScrollTo(0); return; }
    std::wstring t = WRaw(topic);
    for (size_t i = 0; i < HW.secs.size(); ++i)
        if (_wcsicmp(HW.secs[i].anchor.c_str(), t.c_str()) == 0) {
            SendMessageW(HW.toc, LB_SETCURSEL, (WPARAM)i, 0);
            ScrollTo(HW.secs[i].pos);
            return;
        }
    ScrollTo(0);
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

void FindNext() {
    std::wstring q = Lower(GetText(HW.find));
    if (q.empty()) { SetFocus(HW.find); return; }
    std::wstring all = Lower(HW.plain);
    DWORD a = 0, b = 0;
    SendMessageW(HW.text, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    size_t from = b > a ? b : a;
    size_t p = all.find(q, from);
    if (p == std::wstring::npos) p = all.find(q);   // по кругу с начала
    if (p == std::wstring::npos) { MessageBeep(MB_ICONASTERISK); SetWindowTextW(HW.src, (L"Не найдено: " + GetText(HW.find)).c_str()); return; }
    ScrollTo((LONG)p, (LONG)(p + q.size()));
    SetFocus(HW.text);
}

void EditHelp() {
    if (!kAuthorMode) return;
    const char* code = LangCode();
    std::string f = HelpFileFor(code, false);
    if (f.empty()) {
        std::string from;
        std::string t = LoadHelpText(from);
        f = HelpFileFor(code, true);
        MakeDirs(ExeDir() + "Help\\");
        if (!WriteText(f, t, true)) {   // папка программы только для чтения (например, на компакт-диске) — в Data
            f = DataDir() + "Help\\help." + code + ".txt";
            MakeDirs(DataDir() + "Help\\");
            if (!WriteText(f, t, true)) { MessageBoxW(HW.h, (L"Не удалось создать файл справки: " + WRaw(f)).c_str(), L"Справка", MB_OK | MB_ICONWARNING); return; }
        }
        Logger::I().log(Lvl::Info, "Создан файл справки для правки: " + f);
        Load();
    }
    std::wstring np = WRaw(SysTool("notepad.exe")), arg = L"\"" + WRaw(f) + L"\"";
    if ((INT_PTR)ShellExecuteW(HW.h, L"open", np.c_str(), arg.c_str(), nullptr, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(HW.h, L"open", WRaw(f).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    SetWindowTextW(HW.src, (L"Правьте файл в Блокноте, сохраните и нажмите «Обновить»: " + WRaw(f)).c_str());
}

void Layout() {
    RECT rc;
    GetClientRect(HW.h, &rc);
    int m = S(8), W_ = rc.right, H = rc.bottom, tw = S(250), bh = S(28), y = m;
    Place(HW.find, m, y + 1, tw - S(84), S(24));
    Place(HW.bFind, m + tw - S(80), y, S(80), bh);
    if (HW.bEdit) Place(HW.bEdit, W_ - m - S(150) - S(8) - S(100) - S(8) - S(100), y, S(150), bh);
    if (HW.bReload) Place(HW.bReload, W_ - m - S(100) - S(8) - S(100), y, S(100), bh);
    Place(HW.bClose, W_ - m - S(100), y, S(100), bh);
    y += bh + S(8);
    int bottom = H - m - S(20);
    Place(HW.toc, m, y, tw, bottom - y);
    Place(HW.text, m + tw + S(8), y, W_ - 2 * m - tw - S(8), bottom - y);
    Place(HW.src, m, bottom + S(3), W_ - 2 * m, S(18));
}

LRESULT CALLBACK FindEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && wp == VK_RETURN) { FindNext(); return 0; }
    if (m == WM_CHAR && (wp == L'\r' || wp == L'\n')) return 0;
    return DefSubclassProc(h, m, wp, lp);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(640); mm->ptMinTrackSize.y = S(400); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == ID_TOC && code == LBN_SELCHANGE) {
                int i = (int)SendMessageW(HW.toc, LB_GETCURSEL, 0, 0);
                if (i >= 0 && i < (int)HW.secs.size()) ScrollTo(HW.secs[(size_t)i].pos);
            } else if (id == ID_BFIND) FindNext();
            else if (id == ID_BEDIT) EditHelp();
            else if (id == ID_BRELOAD) { Load(); GoTo(""); }
            else if (id == ID_BCLOSE) DestroyWindow(h);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            HW.h = nullptr;
            if (HW.hFont) { DeleteObject(HW.hFont); HW.hFont = nullptr; }
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void ShowHelpImpl(const std::string& topic) {
    if (HW.h && IsWindow(HW.h)) {
        if (IsIconic(HW.h)) ShowWindow(HW.h, SW_RESTORE);
        SetForegroundWindow(HW.h);
        if (!topic.empty()) GoTo(topic);
        return;
    }
    RegisterOnce(L"WRHelp", Proc);
    HW = HelpWnd();
    RECT wa{0, 0, 1280, 900};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = std::min<int>(S(980), wa.right - wa.left - S(40)), hgt = std::min<int>(S(720), wa.bottom - wa.top - S(40));
    int x = wa.left + ((wa.right - wa.left) - w) / 2, y = wa.top + ((wa.bottom - wa.top) - hgt) / 2;
    HW.h = CreateWindowExW(0, L"WRHelp", kAuthorMode ? L"Справка WinRepair — режим автора" : L"Справка WinRepair", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, hgt, g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!HW.h) return;
    HW.find = Mk(HW.h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_FIND, WS_EX_CLIENTEDGE);
    SendMessageW(HW.find, EM_SETCUEBANNER, TRUE, (LPARAM)TrW(L"Поиск по справке").c_str());
    SetWindowSubclass(HW.find, FindEditProc, 1, 0);
    HW.bFind = Mk(HW.h, L"BUTTON", L"Найти", BS_PUSHBUTTON | WS_TABSTOP, ID_BFIND);
    if (kAuthorMode) {
        HW.bEdit = Mk(HW.h, L"BUTTON", L"Изменить справку…", BS_PUSHBUTTON | WS_TABSTOP, ID_BEDIT);
        HW.bReload = Mk(HW.h, L"BUTTON", L"Обновить", BS_PUSHBUTTON | WS_TABSTOP, ID_BRELOAD);
    }
    HW.bClose = Mk(HW.h, L"BUTTON", L"Закрыть", BS_PUSHBUTTON | WS_TABSTOP, ID_BCLOSE);
    HW.toc = Mk(HW.h, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, ID_TOC, WS_EX_CLIENTEDGE);
    HW.rich = LoadLibraryW(L"msftedit.dll") != nullptr;
    DWORD ts = ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP;
    HW.text = HW.rich ? Mk(HW.h, L"RICHEDIT50W", L"", ts, ID_TEXT, WS_EX_CLIENTEDGE) : nullptr;
    if (!HW.text) { HW.rich = false; HW.text = Mk(HW.h, L"EDIT", L"", ts, ID_TEXT, WS_EX_CLIENTEDGE); }
    HW.src = Mk(HW.h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_SRC);
    // Текст справки — шрифтом покрупнее, чем элементы управления
    LOGFONTW lf{};
    GetObjectW(g_ui.font, sizeof lf, &lf);
    lf.lfHeight = -MulDiv(10, g_ui.dpi, 72);
    HW.hFont = CreateFontIndirectW(&lf);
    SendMessageW(HW.text, WM_SETFONT, (WPARAM)(HW.hFont ? HW.hFont : g_ui.font), TRUE);
    if (HW.rich) {
        SendMessageW(HW.text, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(255, 255, 255));
        SendMessageW(HW.text, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(10), S(10)));
    }
    Layout();
    Load();
    ShowWindow(HW.h, SW_SHOW);
    GoTo(topic);
    SetFocus(HW.text);
}

// Раздел справки для окна, в котором нажата F1 (по классу окна верхнего уровня).
std::string HelpTopicForWindowImpl(HWND w) {
    HWND top = w ? GetAncestor(w, GA_ROOT) : nullptr;
    if (!top) return "";
    wchar_t cn[64] = L"";
    GetClassNameW(top, cn, 64);
    static const struct { const wchar_t* cls; const char* topic; } map[] = {
        {L"WRWinPeBuild", "winpe"}, {L"WRWinRE", "winre"}, {L"WRRegistry", "registry"}, {L"WRRegFind", "registry"},
        {L"WRBcd", "bcd"}, {L"WRAutoruns", "autoruns"}, {L"WRApps", "apps"}, {L"WRExplorer", "explorer"},
        {L"WRFixes", "fixes"}, {L"WRStorage", "bitlocker"}, {L"WRTriage", "triage"}, {L"WRPlaybooks", "playbooks"},
        {L"WinRepairCatalog", "catalog"},
    };
    for (auto& e : map) if (!_wcsicmp(cn, e.cls)) return e.topic;
    return "";
}

}  // namespace ui

void ShowHelp(const std::string& topic) { ui::ShowHelpImpl(topic); }
std::string HelpTopicForWindow(HWND w) { return ui::HelpTopicForWindowImpl(w); }

}  // namespace wr
