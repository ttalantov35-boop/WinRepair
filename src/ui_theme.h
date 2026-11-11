// Visual theme: fonts, palette, colour-coded owner-draw buttons, header band with logo, legend.
// Colour = purpose of the command, so the technician sees at a glance what a button does.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace wr {
namespace theme {


enum class Cat {
    Diag,     // blue    - diagnostics, read-only
    Guide,    // teal    - wizards and playbooks (guided work)
    Repair,   // orange  - changes the system (always asks for confirmation)
    Protect,  // green   - protection: restore points, saving data/backups
    Info,     // grey    - service buttons, about
    Header,   // translucent white on the dark header
    Exit      // red     - leave the program
};

void Init(int dpi);
HFONT Body();      // controls
HFONT Semi();      // buttons, headings
HFONT Title();     // product name in the header
HFONT Small();     // captions
HFONT Big();       // target selector
HBRUSH BgBrush();
HBRUSH LogBrush();
HBRUSH TintBrush();   // light-blue fill of the highlighted "target OS" panel
COLORREF TintColor();
COLORREF LevelColor(int level, bool* bold);   // log line colour: 0 info, 1 warn, 2 error, 3 out, 4 ok, 5 heading
COLORREF Text();
COLORREF LogText();
COLORREF LogBg();
COLORREF Accent(Cat c);

void DrawButton(const DRAWITEMSTRUCT* d, Cat c, bool hover);
void AttachHover(HWND button);        // subclass: repaint on mouse enter/leave
bool IsHover(HWND button);
struct Chip { std::wstring text; int kind; };   // kind: 0 neutral, 1 good (green), 2 bad (red)
void PaintHeader(HDC dc, const RECT& rc, HICON icon, const std::wstring& title, const std::wstring& subtitle, const std::vector<Chip>& chips);
void PaintTargetPanel(HDC dc, const RECT& rc);
// Owner-drawn tab: the selected one is filled with its colour, the others show a thin colour line
void DrawTab(const DRAWITEMSTRUCT* d, const wchar_t* text, Cat c, bool selected);
// "What does this command do" card: bold title, category label on the right, description below
void DrawInfoCard(const DRAWITEMSTRUCT* d, const std::wstring& title, const std::wstring& text, Cat c, const std::wstring& catLabel);
int PaintLegend(HDC dc, int x, int y, int maxWidth);   // returns used height

}  // namespace theme
}  // namespace wr
