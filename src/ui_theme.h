// Visual theme: fonts, palette, colour-coded owner-draw buttons, header band with logo, legend.
// Colour = purpose of the command, so the technician sees at a glance what a button does.
#pragma once
#include <windows.h>
#include <string>

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
HBRUSH BgBrush();
HBRUSH LogBrush();
COLORREF Text();
COLORREF LogText();
COLORREF LogBg();
COLORREF Accent(Cat c);

void DrawButton(const DRAWITEMSTRUCT* d, Cat c, bool hover);
void AttachHover(HWND button);        // subclass: repaint on mouse enter/leave
bool IsHover(HWND button);
void PaintHeader(HDC dc, const RECT& rc, HICON icon, const std::wstring& title, const std::wstring& subtitle);
int PaintLegend(HDC dc, int x, int y, int maxWidth);   // returns used height

}  // namespace theme
}  // namespace wr
