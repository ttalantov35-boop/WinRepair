// WinRepair Toolkit - native Win32 GUI (no .NET / PowerShell / VC++ runtime required).
#include "win/integrity.h"
#include "win/backups.h"
#include "ui_theme.h"
#include <richedit.h>
#include <set>
#include "win/app.h"
#include "modules/catalog.h"
#include "modules/bcd.h"
#include "modules/winre.h"
#include "modules/playbook.h"
#include "ui_tools_common.h"
#include "ui_tools.h"

#include <cctype>
#include <cstring>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

using namespace wr;

namespace {

enum {
    WM_APP_LOG = WM_APP + 1, WM_APP_STATUS, WM_APP_DONE, WM_APP_ASK = WM_APP + 10, WM_APP_CONFIRM, WM_APP_OPENWIN,
    IDC_BANNER = 100, IDC_TARGET, IDC_REFRESH, IDC_DAYS, IDC_EXT, IDC_FORCE, IDC_HOST, IDC_PORT, IDC_SOURCE,
    IDC_TAB, IDC_LOG, IDC_CANCEL, IDC_OPENREP, IDC_OPENLOG, IDC_SAVELOG, IDC_CLEAR, IDC_COPYLOG, IDC_PROG, IDC_STATUS,
    IDC_L1, IDC_L2, IDC_L3, IDC_L4, IDC_L5, IDC_L6, IDC_DESC, IDC_ACTION0 = 1000,
    IDC_CAT_LIST = 2001, IDC_CAT_FILTER, IDC_CAT_RUN, IDC_CAT_CLOSE, IDC_CAT_DESC, IDC_CAT_E0 = 2010, IDC_CAT_L0 = 2020, IDC_CAT_FIND = 2030, IDC_LANG = 2040, IDC_EXIT = 2050
};

struct Action {
    const wchar_t* text;
    int tab;
    ModuleFn fn;
    const char* name;
    const wchar_t* confirm;  // non-null => mutating, ask first
};

void ShowAbout();
void OpenLastReport();
void OpenCatalog();
void ChangeLanguage();
void PutClipboard(const std::wstring& txt);
LRESULT CALLBACK LogSubProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
void QuickCmd(int no);
Target CurrentTarget();

const wchar_t* kTabs[] = {L"РћР±Р·РѕСЂ", L"РЎРёСЃС‚РµРјР°", L"Р”РёСЃРєРё", L"Р¦РµР»РѕСЃС‚РЅРѕСЃС‚СЊ", L"Р”СЂР°Р№РІРµСЂС‹ Рё BSOD", L"РЎРѕР±С‹С‚РёСЏ", L"РЎРµС‚СЊ", L"РРЅСЃС‚СЂСѓРјРµРЅС‚С‹ СЂРµРјРѕРЅС‚Р°"};

Action kActions[] = {
    {L"РњР°СЃС‚РµСЂ В«Р§С‚Рѕ СЃР»СѓС‡РёР»РѕСЃСЊ?В» (РґРёР°РіРЅРѕСЃС‚РёРєР° РїРѕ СЃРёРјРїС‚РѕРјСѓ)вЂ¦", 0, nullptr, "triage", nullptr},
    {L"РЎС†РµРЅР°СЂРёРё СЂРµРјРѕРЅС‚Р° (playbooks)вЂ¦", 0, nullptr, "playbooks", nullptr},
    {L"РџРѕР»РЅР°СЏ РґРёР°РіРЅРѕСЃС‚РёРєР° + HTML-РѕС‚С‡С‘С‚", 0, ModFull, "full", nullptr},
    {L"РћС‚РєСЂС‹С‚СЊ РїРѕСЃР»РµРґРЅРёР№ HTML-РѕС‚С‡С‘С‚", 0, nullptr, "open-report", nullptr},
    {L"РљР°С‚Р°Р»РѕРі РєРѕРјР°РЅРґвЂ¦", 0, nullptr, "catalog", nullptr},
    {L"Рћ РїСЂРѕРіСЂР°РјРјРµ", 0, nullptr, "about", nullptr},
    {L"РЎРЅРёРјРѕРє СЃРёСЃС‚РµРјС‹", 1, ModSystem, "system", nullptr},
    {L"РџР°РјСЏС‚СЊ (SMBIOS, РґРёР°РіРЅРѕСЃС‚РёРєР°)", 1, ModMemory, "memory", nullptr},
    {L"Р—Р°РіСЂСѓР·РєР° (BCD / GPT / WinRE)", 1, ModBoot, "boot", nullptr},
    {L"Р”РёСЃРєРё, SMART, РѕС€РёР±РєРё Р¤РЎ", 2, ModDisk, "disk", nullptr},
    {L"РџСЂРѕРІРµСЂРєР° С†РµР»РѕСЃС‚РЅРѕСЃС‚Рё (Р±РµР· РёР·РјРµРЅРµРЅРёР№)", 3, ModIntegrity, "integrity", nullptr},
    {L"РРЎРџР РђР’РРўР¬ С†РµР»РѕСЃС‚РЅРѕСЃС‚СЊ (DISM + SFC)", 3, ModRepair, "repair",
     L"Р‘СѓРґРµС‚ РІС‹РїРѕР»РЅРµРЅРѕ РР—РњР•РќРЇР®Р©Р•Р• РґРµР№СЃС‚РІРёРµ:\nDISM /RestoreHealth, Р·Р°С‚РµРј sfc /scannow РЅР° РІС‹Р±СЂР°РЅРЅРѕР№ РћРЎ.\n\n"
     L"РћРїРµСЂР°С†РёСЏ РјРѕР¶РµС‚ Р·Р°РЅСЏС‚СЊ РґРµСЃСЏС‚РєРё РјРёРЅСѓС‚. РџРµСЂРµРґ СЂРµРјРѕРЅС‚РѕРј СЂРµРєРѕРјРµРЅРґСѓРµС‚СЃСЏ С‚РѕС‡РєР° РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ Рё СЂРµР·РµСЂРІРЅР°СЏ РєРѕРїРёСЏ РґР°РЅРЅС‹С….\n\nРџСЂРѕРґРѕР»Р¶РёС‚СЊ?"},
    {L"РЎРѕР·РґР°С‚СЊ С‚РѕС‡РєСѓ РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ", 3, ModRestorePoint, "restorepoint",
     L"Р‘СѓРґРµС‚ СЃРѕР·РґР°РЅР° С‚РѕС‡РєР° РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ Windows (РёР·РјРµРЅСЏСЋС‰РµРµ РґРµР№СЃС‚РІРёРµ).\n\nРџСЂРѕРґРѕР»Р¶РёС‚СЊ?"},
    {L"Р”СЂР°Р№РІРµСЂС‹ Рё СѓСЃС‚СЂРѕР№СЃС‚РІР° СЃ РѕС€РёР±РєР°РјРё", 4, ModDrivers, "drivers", nullptr},
    {L"BSOD: СЃРѕР±С‹С‚РёСЏ Рё РґР°РјРїС‹", 4, ModBsod, "bsod", nullptr},
    {L"Р Р°Р·Р±РѕСЂ РґР°РјРїРѕРІ (Р·Р°РіРѕР»РѕРІРѕРє + cdb)", 4, ModDump, "dump", nullptr},
    {L"РћС€РёР±РєРё System / Application", 5, ModEvents, "events", nullptr},
    {L"РЎРµС‚РµРІР°СЏ РґРёР°РіРЅРѕСЃС‚РёРєР°", 6, ModNetwork, "network", nullptr},
    {L"Р РµРµСЃС‚СЂ РѕС„Р»Р°Р№РЅ-Windows (РїСЂРѕСЃРјРѕС‚СЂ Рё РїСЂР°РІРєР°)вЂ¦", 7, nullptr, "registry", nullptr},
    {L"BCD / Р—Р°РіСЂСѓР·РєР°: С…СЂР°РЅРёР»РёС‰Рµ, Р·Р°РїРёСЃРё, СЂРµР¶РёРјС‹вЂ¦", 7, nullptr, "bcd", nullptr},
    {L"РђРІС‚РѕР·Р°РіСЂСѓР·РєР° Рё СЃР»СѓР¶Р±С‹: С‚РµРєСѓС‰Р°СЏ Рё РѕС„Р»Р°Р№РЅ-Windows (Autoruns + msconfig)вЂ¦", 7, nullptr, "autoruns", nullptr},
    {L"РњР°СЃС‚РµСЂ РёСЃРїСЂР°РІР»РµРЅРёР№ РїРѕСЃР»Рµ Р·Р°СЂР°Р¶РµРЅРёР№вЂ¦", 7, nullptr, "fixes", nullptr},
    {L"Р’РѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёРµ СЃРёСЃС‚РµРјС‹: rstruiвЂ¦", 7, nullptr, "rstrui", nullptr},
    {L"РўРѕС‡РєРё РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ: СЃРѕСЃС‚РѕСЏРЅРёРµ", 7, nullptr, "restore-info", nullptr},
    {L"WinRE: РІС…РѕРґ РІ СЃСЂРµРґСѓ РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ, WinRE + WinRepairвЂ¦", 7, nullptr, "winre", nullptr},
    {L"Р’С‹Р№С‚Рё РІ С€С‚Р°С‚РЅРѕРµ РјРµРЅСЋ WinRE (RecEnv)вЂ¦", 7, nullptr, "winre-menu", nullptr},
    {L"РЎРѕС…СЂР°РЅРёС‚СЊ Р¶СѓСЂРЅР°Р»С‹ Рё РѕС‚С‡С‘С‚С‹ РЅР° РґРёСЃРєвЂ¦", 7, nullptr, "savedata", nullptr},
    {L"Р РµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё: РїСЂРѕСЃРјРѕС‚СЂ Рё РѕС‡РёСЃС‚РєР°вЂ¦", 7, nullptr, "backups", nullptr},
    {L"Secure Boot: СЃРµСЂС‚РёС„РёРєР°С‚С‹ 2023", 7, nullptr, "secureboot", nullptr},
    {L"BitLocker Рё РґСЂР°Р№РІРµСЂС‹ С…СЂР°РЅРёР»РёС‰Р° (РґРёСЃРє РЅРµ РІРёРґРµРЅ)вЂ¦", 7, nullptr, "storagefix", nullptr},
    {L"РЈСЃС‚Р°РЅРѕРІР»РµРЅРЅС‹Рµ РїСЂРёР»РѕР¶РµРЅРёСЏ: С‚РµРєСѓС‰Р°СЏ Рё РѕС„Р»Р°Р№РЅ-Windows (СѓРґР°Р»РёС‚СЊ РїСЂРѕРіСЂР°РјРјСѓ)вЂ¦", 7, nullptr, "apps", nullptr},
    {L"РћС„Р»Р°Р№РЅ-РїСЂРѕРІРѕРґРЅРёРє: С„Р°Р№Р»С‹ РІС‹Р±СЂР°РЅРЅРѕР№ WindowsвЂ¦", 7, nullptr, "explorer", nullptr},
};
const int kActionCount = (int)(sizeof kActions / sizeof kActions[0]);

HWND g_wnd, g_tab, g_log, g_target, g_days, g_ext, g_force, g_host, g_port, g_source, g_prog, g_status, g_banner;
HWND g_btn[32], g_cancelBtn, g_refreshBtn, g_lbl[6], g_hint;
HWND g_langBtn = nullptr, g_exitBtn = nullptr;
HICON g_icon = nullptr;
bool g_logRich = false;
RECT g_targetFrame{};
std::vector<theme::Chip> g_chips;
std::wstring g_hdrSub;
HWND g_cat = nullptr, g_catList, g_catFilter, g_catDesc, g_catRun, g_catClose, g_catEd[6], g_catLb[6], g_catFind;
std::vector<std::string> g_bcdPaths;
std::vector<int> g_catMap;
std::vector<std::string> g_catCats;
HFONT g_font, g_mono;
int g_dpi = 96;
std::vector<Target> g_targets;
Results g_results;
std::atomic<bool> g_cancel(false);
bool g_running = false;
std::string g_lastOut, g_jobName;
ULONGLONG g_jobStart = 0;

int S(int v) { return MulDiv(v, g_dpi, 96); }

// What each command does (shown in the card under the tabs when the pointer is on a button)
struct ActionDesc { const char* name; const wchar_t* text; };
const ActionDesc kDescs[] = {
    {"triage", L"Р’С‹Р±РµСЂРёС‚Рµ, С‡С‚Рѕ СЃР»СѓС‡РёР»РѕСЃСЊ (РЅРµ Р·Р°РіСЂСѓР¶Р°РµС‚СЃСЏ, СЃРёРЅРёР№ СЌРєСЂР°РЅ, РјРµРґР»РµРЅРЅРѕ СЂР°Р±РѕС‚Р°РµС‚, РЅРµС‚ СЃРµС‚Рё Рё С‚. Рґ.). РџСЂРѕРіСЂР°РјРјР° РІС‹РїРѕР»РЅРёС‚ РЅР°Р±РѕСЂ РїСЂРѕРІРµСЂРѕРє С‚РѕР»СЊРєРѕ РЅР° С‡С‚РµРЅРёРµ, РїРѕРєР°Р¶РµС‚ РЅР°Р№РґРµРЅРЅС‹Рµ РїСЂРѕР±Р»РµРјС‹ РїРѕ РІР°Р¶РЅРѕСЃС‚Рё Рё РїСЂРµРґР»РѕР¶РёС‚ РґРµР№СЃС‚РІРёРµ РґР»СЏ РєР°Р¶РґРѕР№."},
    {"playbooks", L"Р“РѕС‚РѕРІС‹Рµ РїРѕС€Р°РіРѕРІС‹Рµ СЃС†РµРЅР°СЂРёРё СЂРµРјРѕРЅС‚Р°: РѕС€РёР±РєР° 0x7B, Р·Р°С†РёРєР»РµРЅРЅРѕРµ РѕР±РЅРѕРІР»РµРЅРёРµ, СЂРµРјРѕРЅС‚ Р·Р°РіСЂСѓР·РєРё UEFI Рё РґСЂСѓРіРёРµ. РџРµСЂРµРґ Р·Р°РїСѓСЃРєРѕРј РїРѕРєР°Р·С‹РІР°РµС‚СЃСЏ РІРµСЃСЊ РїР»Р°РЅ, РєР°Р¶РґС‹Р№ С€Р°Рі, РјРµРЅСЏСЋС‰РёР№ СЃРёСЃС‚РµРјСѓ, РїРѕРґС‚РІРµСЂР¶РґР°РµС‚СЃСЏ РѕС‚РґРµР»СЊРЅРѕ."},
    {"full", L"Р—Р°РїСѓСЃРєР°РµС‚ 9 РґРёР°РіРЅРѕСЃС‚РёС‡РµСЃРєРёС… РјРѕРґСѓР»РµР№ РїРѕРґСЂСЏРґ, РІС‹СЃС‚Р°РІР»СЏРµС‚ РѕС†РµРЅРєСѓ СЃРѕСЃС‚РѕСЏРЅРёСЏ СЃРёСЃС‚РµРјС‹ Рё СЃРѕС…СЂР°РЅСЏРµС‚ HTML-РѕС‚С‡С‘С‚ РІ РїР°РїРєСѓ РѕС‚С‡С‘С‚РѕРІ. РќРёС‡РµРіРѕ РЅРµ РёР·РјРµРЅСЏРµС‚."},
    {"open-report", L"РћС‚РєСЂС‹РІР°РµС‚ РІ Р±СЂР°СѓР·РµСЂРµ РїРѕСЃР»РµРґРЅРёР№ СЃРѕС…СЂР°РЅС‘РЅРЅС‹Р№ HTML-РѕС‚С‡С‘С‚ РїРѕР»РЅРѕР№ РґРёР°РіРЅРѕСЃС‚РёРєРё."},
    {"catalog", L"РљР°С‚Р°Р»РѕРі РёР· 83 РєРѕРјР°РЅРґ Windows (winver, systeminfo, sfc, DISM, bcdedit, diskpart, netsh Рё РґСЂ.) СЃ РѕРїРёСЃР°РЅРёРµРј, СѓСЂРѕРІРЅРµРј СЂРёСЃРєР° Рё С‚РѕС‡РЅРѕР№ РєРѕРјР°РЅРґРЅРѕР№ СЃС‚СЂРѕРєРѕР№. РљР°Р¶РґС‹Р№ Р·Р°РїСѓСЃРє РїРѕРґС‚РІРµСЂР¶РґР°РµС‚СЃСЏ РѕС‚РґРµР»СЊРЅРѕ."},
    {"about", L"Р’РµСЂСЃРёСЏ РїСЂРѕРіСЂР°РјРјС‹, СЃСЂРµРґР° Р·Р°РїСѓСЃРєР°, РїСЂР°РІР°, СЂР°СЃРїРѕР»РѕР¶РµРЅРёРµ Р¶СѓСЂРЅР°Р»РѕРІ Рё РѕС‚С‡С‘С‚РѕРІ, СЂРµР·СѓР»СЊС‚Р°С‚ РїСЂРѕРІРµСЂРєРё С†РµР»РѕСЃС‚РЅРѕСЃС‚Рё."},
    {"system", L"РЎРЅРёРјРѕРє СЃРёСЃС‚РµРјС‹: РІРµСЂСЃРёСЏ Рё СЃР±РѕСЂРєР° Windows, РёРјСЏ Рё СЃРѕСЃС‚РѕСЏРЅРёРµ РєРѕРјРїСЊСЋС‚РµСЂР°, РїСЂРѕС†РµСЃСЃРѕСЂ, РїР°РјСЏС‚СЊ, BIOS Рё РїР»Р°С‚Р°, TPM, Secure Boot, РґРёСЃРєРё. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"memory", L"РњРѕРґСѓР»Рё РїР°РјСЏС‚Рё РїРѕ РґР°РЅРЅС‹Рј SMBIOS (РѕР±СЉС‘Рј, С‚РёРї, СЃРєРѕСЂРѕСЃС‚СЊ, СЃР»РѕС‚С‹) Рё СЂРµР·СѓР»СЊС‚Р°С‚С‹ РґРёР°РіРЅРѕСЃС‚РёРєРё РїР°РјСЏС‚Рё Windows, РµСЃР»Рё РѕРЅР° Р·Р°РїСѓСЃРєР°Р»Р°СЃСЊ. Р”Р°С‘С‚ СЂРµРєРѕРјРµРЅРґР°С†РёРё РїСЂРё РїРѕРґРѕР·СЂРµРЅРёРё РЅР° РЅРµРёСЃРїСЂР°РІРЅРѕРµ РћР—РЈ. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"boot", L"РЎРѕСЃС‚РѕСЏРЅРёРµ Р·Р°РіСЂСѓР·РєРё РІС‹Р±СЂР°РЅРЅРѕР№ РћРЎ: С…СЂР°РЅРёР»РёС‰Рµ BCD, Р·Р°РїРёСЃРё Р·Р°РіСЂСѓР·РєРё, СЃС…РµРјР° РґРёСЃРєРѕРІ (GPT/MBR), СЃРёСЃС‚РµРјРЅС‹Р№ СЂР°Р·РґРµР» ESP Рё СЃСЂРµРґР° WinRE. РџРѕРєР°Р·С‹РІР°РµС‚ Р·Р°РјРµС‡Р°РЅРёСЏ, РЅР°РїСЂРёРјРµСЂ РѕС‚СЃСѓС‚СЃС‚РІСѓСЋС‰СѓСЋ Р·Р°РїРёСЃСЊ. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"disk", L"РЎРѕСЃС‚РѕСЏРЅРёРµ РґРёСЃРєРѕРІ: SMART Рё Р·РґРѕСЂРѕРІСЊРµ, С‚РѕРјР° Рё СЃРІРѕР±РѕРґРЅРѕРµ РјРµСЃС‚Рѕ, РѕС€РёР±РєРё С„Р°Р№Р»РѕРІРѕР№ СЃРёСЃС‚РµРјС‹ РёР· Р¶СѓСЂРЅР°Р»Р° СЃРѕР±С‹С‚РёР№ Р·Р° РІС‹Р±СЂР°РЅРЅС‹Р№ РїРµСЂРёРѕРґ. Р’ СЂР°СЃС€РёСЂРµРЅРЅРѕРј СЂРµР¶РёРјРµ РґРѕР±Р°РІР»СЏРµС‚СЃСЏ chkdsk /scan. РќРёС‡РµРіРѕ РЅРµ РјРµРЅСЏРµС‚."},
    {"integrity", L"РџСЂРѕРІРµСЂСЏРµС‚ С†РµР»РѕСЃС‚РЅРѕСЃС‚СЊ СЃРёСЃС‚РµРјРЅС‹С… С„Р°Р№Р»РѕРІ Рё С…СЂР°РЅРёР»РёС‰Р° РєРѕРјРїРѕРЅРµРЅС‚РѕРІ Windows (DISM, SFC) С‚РѕР»СЊРєРѕ РІ СЂРµР¶РёРјРµ РїСЂРѕРІРµСЂРєРё: РЅРёС‡РµРіРѕ РЅРµ РёСЃРїСЂР°РІР»СЏРµС‚, РїРѕРєР°Р·С‹РІР°РµС‚, РµСЃС‚СЊ Р»Рё РЅР°СЂСѓС€РµРЅРёСЏ."},
    {"repair", L"РРЎРџР РђР’Р›РЇР•Рў С†РµР»РѕСЃС‚РЅРѕСЃС‚СЊ: DISM /RestoreHealth, Р·Р°С‚РµРј sfc /scannow РЅР° РІС‹Р±СЂР°РЅРЅРѕР№ РћРЎ. РњРѕР¶РµС‚ Р·Р°РЅСЏС‚СЊ РґРµСЃСЏС‚РєРё РјРёРЅСѓС‚. РџРµСЂРµРґ Р·Р°РїСѓСЃРєРѕРј СЃРѕР·РґР°Р№С‚Рµ С‚РѕС‡РєСѓ РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ Рё СЃРѕС…СЂР°РЅРёС‚Рµ РґР°РЅРЅС‹Рµ. РџРѕС‚СЂРµР±СѓРµС‚СЃСЏ РїРѕРґС‚РІРµСЂР¶РґРµРЅРёРµ."},
    {"restorepoint", L"РЎРѕР·РґР°С‘С‚ С‚РѕС‡РєСѓ РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ Windows, С‡С‚РѕР±С‹ РјРѕР¶РЅРѕ Р±С‹Р»Рѕ РѕС‚РєР°С‚РёС‚СЊ РёР·РјРµРЅРµРЅРёСЏ. РћР±С‹С‡РЅРѕ СЃРёСЃС‚РµРјР° СЂР°Р·СЂРµС€Р°РµС‚ РЅРµ С‡Р°С‰Рµ РѕРґРЅРѕР№ С‚РѕС‡РєРё РІ 24 С‡Р°СЃР°: РіР°Р»РєР° СЂСЏРґРѕРј СЃРЅРёРјР°РµС‚ СЌС‚Рѕ РѕРіСЂР°РЅРёС‡РµРЅРёРµ."},
    {"drivers", L"РЈСЃС‚СЂРѕР№СЃС‚РІР° СЃ РѕС€РёР±РєР°РјРё РґСЂР°Р№РІРµСЂРѕРІ (РєРѕРґ РїСЂРѕР±Р»РµРјС‹, РР” РѕР±РѕСЂСѓРґРѕРІР°РЅРёСЏ) Рё СЃРїРёСЃРѕРє СѓСЃС‚Р°РЅРѕРІР»РµРЅРЅС‹С… РїР°РєРµС‚РѕРІ РґСЂР°Р№РІРµСЂРѕРІ. РџРѕРјРѕРіР°РµС‚ РЅР°Р№С‚Рё СѓСЃС‚СЂРѕР№СЃС‚РІРѕ Р±РµР· РґСЂР°Р№РІРµСЂР°. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"bsod", L"РЎРѕР±С‹С‚РёСЏ BugCheck Р·Р° РІС‹Р±СЂР°РЅРЅС‹Р№ РїРµСЂРёРѕРґ Рё РЅР°Р№РґРµРЅРЅС‹Рµ С„Р°Р№Р»С‹ РґР°РјРїРѕРІ (Minidump, MEMORY.DMP). РљРѕРґ СЃРёРЅРµРіРѕ СЌРєСЂР°РЅР° вЂ” РєР°С‚РµРіРѕСЂРёСЏ, Р° РЅРµ РґРёР°РіРЅРѕР·. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"dump", L"Р Р°Р·Р±РѕСЂ С„Р°Р№Р»РѕРІ РґР°РјРїРѕРІ: Р·Р°РіРѕР»РѕРІРѕРє Рё РєРѕРґС‹ РѕСЃС‚Р°РЅРѕРІРєРё; РµСЃР»Рё РІ РїР°РїРєРµ Tools РµСЃС‚СЊ cdb.exe, РІС‹РїРѕР»РЅСЏРµС‚СЃСЏ РїРѕРґСЂРѕР±РЅС‹Р№ Р°РЅР°Р»РёР·. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"events", L"РћС€РёР±РєРё Рё РєСЂРёС‚РёС‡РµСЃРєРёРµ СЃРѕР±С‹С‚РёСЏ РёР· Р¶СѓСЂРЅР°Р»РѕРІ System Рё Application Р·Р° РІС‹Р±СЂР°РЅРЅС‹Р№ РїРµСЂРёРѕРґ. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"network", L"РЎРµС‚РµРІР°СЏ РґРёР°РіРЅРѕСЃС‚РёРєР°: Р°РґР°РїС‚РµСЂС‹, РЅР°СЃС‚СЂРѕР№РєРё IP, С€Р»СЋР·, DNS, РїСЂРѕРІРµСЂРєР° СЃРІСЏР·Рё СЃ СѓРєР°Р·Р°РЅРЅС‹Рј С…РѕСЃС‚РѕРј Рё РїРѕСЂС‚РѕРј. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"registry", L"РћС„Р»Р°Р№РЅ-СЂРµРґР°РєС‚РѕСЂ СЂРµРµСЃС‚СЂР° РІС‹Р±СЂР°РЅРЅРѕР№ Windows: РїСЂРѕСЃРјРѕС‚СЂ Рё РїСЂР°РІРєР° РєСѓСЃС‚РѕРІ (SYSTEM, SOFTWARE, РїСЂРѕС„РёР»Рё РїРѕР»СЊР·РѕРІР°С‚РµР»РµР№). РџРµСЂРµРґ РёР·РјРµРЅРµРЅРёРµРј СЃРѕС…СЂР°РЅСЏРµС‚СЃСЏ РєРѕРїРёСЏ РІРµС‚РєРё; РїСЂР°РІРєРё РїРѕРґС‚РІРµСЂР¶РґР°СЋС‚СЃСЏ Рё РїРёС€СѓС‚СЃСЏ РІ Р¶СѓСЂРЅР°Р»."},
    {"bcd", L"РњРµРЅРµРґР¶РµСЂ Р·Р°РіСЂСѓР·РєРё: РїСЂРѕСЃРјРѕС‚СЂ Рё РїСЂР°РІРєР° Р·Р°РїРёСЃРµР№ BCD, СЂРµР¶РёРјС‹ Р±РµР·РѕРїР°СЃРЅРѕР№ Р·Р°РіСЂСѓР·РєРё РїСЂРё СЃР»РµРґСѓСЋС‰РµРј Р·Р°РїСѓСЃРєРµ, СЃРѕР·РґР°РЅРёРµ Р·Р°РіСЂСѓР·РѕС‡РЅС‹С… С„Р°Р№Р»РѕРІ (bcdboot). РџРµСЂРµРґ РёР·РјРµРЅРµРЅРёРµРј РґРµР»Р°РµС‚СЃСЏ РєРѕРїРёСЏ С…СЂР°РЅРёР»РёС‰Р°."},
    {"apps", L"РЎРїРёСЃРѕРє РїСЂРѕРіСЂР°РјРј РѕС„Р»Р°Р№РЅ-Windows РёР· СЂРµРµСЃС‚СЂР° (РјР°С€РёРЅР° Рё РїРѕР»СЊР·РѕРІР°С‚РµР»Рё) СЃ РІС‹РґРµР»РµРЅРёРµРј Р°РЅС‚РёРІРёСЂСѓСЃРѕРІ Рё СЃСЂРµРґСЃС‚РІ Р·Р°С‰РёС‚С‹. Р”Р»СЏ РїСЂРѕРіСЂР°РјРјС‹, РёР·-Р·Р° РєРѕС‚РѕСЂРѕР№ Windows РЅРµ Р·Р°РїСѓСЃРєР°РµС‚СЃСЏ: РѕС‚РєР»СЋС‡РёС‚СЊ РµС‘ СЃР»СѓР¶Р±С‹, РґСЂР°Р№РІРµСЂС‹ Рё Р°РІС‚РѕР·Р°РїСѓСЃРє, РїРµСЂРµРјРµСЃС‚РёС‚СЊ РїР°РїРєСѓ РІ РєР°СЂР°РЅС‚РёРЅ (РѕР±СЂР°С‚РёРјРѕ) Рё СѓР±СЂР°С‚СЊ Р·Р°РїРёСЃСЊ РёР· СЃРїРёСЃРєР°. Р”РµРёРЅСЃС‚Р°Р»Р»СЏС‚РѕСЂ Р±РµР· СЂР°Р±РѕС‚Р°СЋС‰РµР№ Windows РЅРµ Р·Р°РїСѓСЃС‚РёС‚СЊ вЂ” СЌС‚Рѕ РїСЂРёРЅСѓРґРёС‚РµР»СЊРЅРѕРµ СѓРґР°Р»РµРЅРёРµ."},
    {"explorer", L"РђРІС‚РѕРЅРѕРјРЅС‹Р№ РїСЂРѕРІРѕРґРЅРёРє РґР»СЏ С„Р°Р№Р»РѕРІ РІС‹Р±СЂР°РЅРЅРѕР№ (РІ С‚РѕРј С‡РёСЃР»Рµ РЅРµСЂР°Р±РѕС‚Р°СЋС‰РµР№) Windows: РґРµСЂРµРІРѕ РґРёСЃРєРѕРІ, РєРѕРїРёСЂРѕРІР°РЅРёРµ, РїРµСЂРµРјРµС‰РµРЅРёРµ, РїРµСЂРµРёРјРµРЅРѕРІР°РЅРёРµ, РїРѕРёСЃРє. РЈРґР°Р»РµРЅРёРµ РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ вЂ” РІ РєР°СЂР°РЅС‚РёРЅ WinRepair_Removed (РѕР±СЂР°С‚РёРјРѕ), РµСЃС‚СЊ РѕРєРѕРЅС‡Р°С‚РµР»СЊРЅРѕРµ СѓРґР°Р»РµРЅРёРµ Рё РїРѕР»СѓС‡РµРЅРёРµ РїСЂР°РІ РІР»Р°РґРµР»СЊС†Р°, РµСЃР»Рё РґРѕСЃС‚СѓРї Р·Р°РїСЂРµС‰С‘РЅ."},
    {"autoruns", L"РђРІС‚РѕР·Р°РіСЂСѓР·РєР° Рё СЃР»СѓР¶Р±С‹ РІС‹Р±СЂР°РЅРЅРѕР№ (РІ С‚РѕРј С‡РёСЃР»Рµ РѕС„Р»Р°Р№РЅ) Windows: РєР»СЋС‡Рё Run, Р·Р°РґР°С‡Рё, СЃР»СѓР¶Р±С‹, РґСЂР°Р№РІРµСЂС‹. РџРѕРґРѕР·СЂРёС‚РµР»СЊРЅС‹Рµ Р·Р°РїРёСЃРё РјРѕР¶РЅРѕ РѕС‚РєР»СЋС‡РёС‚СЊ, СЂРµР¶РёРј Р·Р°РїСѓСЃРєР° СЃР»СѓР¶Р± РјРµРЅСЏС‚СЊ (Р°РЅР°Р»РѕРі msconfig). РћС‚РєР»СЋС‡РµРЅРёРµ РѕР±СЂР°С‚РёРјРѕ."},
    {"fixes", L"РћР±СЂР°С‚РёРјС‹Рµ РёСЃРїСЂР°РІР»РµРЅРёСЏ РїРѕСЃР»Рµ Р·Р°СЂР°Р¶РµРЅРёСЏ: Winlogon Shell/Userinit, IFEO, AppInit_DLLs, РїРѕР»РёС‚РёРєРё, РїСЂРѕРєСЃРё, С„Р°Р№Р» hosts Рё РґСЂ. РљР°Р¶РґРѕРµ РёСЃРїСЂР°РІР»РµРЅРёРµ РѕРїРёСЃР°РЅРѕ, РїРѕРґС‚РІРµСЂР¶РґР°РµС‚СЃСЏ Рё Р·Р°РїРёСЃС‹РІР°РµС‚СЃСЏ РІ Р¶СѓСЂРЅР°Р»."},
    {"rstrui", L"Р—Р°РїСѓСЃРєР°РµС‚ РјР°СЃС‚РµСЂ РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ СЃРёСЃС‚РµРјС‹ (rstrui.exe) РґР»СЏ РѕС‚РєР°С‚Р° Рє С‚РѕС‡РєРµ РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ, РµСЃР»Рё РѕРЅР° РµСЃС‚СЊ. РњРµРЅСЏРµС‚ СЃРёСЃС‚РµРјСѓ, РїРѕС‚СЂРµР±СѓРµС‚СЃСЏ РїРѕРґС‚РІРµСЂР¶РґРµРЅРёРµ."},
    {"restore-info", L"РџРѕРєР°Р·С‹РІР°РµС‚, РєР°РєРёРµ С‚РѕС‡РєРё РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ РµСЃС‚СЊ РІ СЃРёСЃС‚РµРјРµ, РёС… РґР°С‚Сѓ Рё РѕРїРёСЃР°РЅРёРµ. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"winre", L"РЎСЂРµРґР° РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ Windows (WinRE): СЃРѕСЃС‚РѕСЏРЅРёРµ, РІС…РѕРґ РІ РЅРµС‘ РїСЂРё СЃР»РµРґСѓСЋС‰РµР№ Р·Р°РіСЂСѓР·РєРµ РёР»Рё СЃСЂР°Р·Сѓ, СЃРѕР·РґР°РЅРёРµ РѕР±СЂР°Р·Р° WinRE СЃ СЌС‚РѕР№ СѓС‚РёР»РёС‚РѕР№ Рё РµРіРѕ СѓРґР°Р»РµРЅРёРµ."},
    {"winre-menu", L"РР· WinRE РІС‹С…РѕРґРёС‚ РІ С€С‚Р°С‚РЅРѕРµ РјРµРЅСЋ СЃСЂРµРґС‹ РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ (РџРѕРёСЃРє Рё СѓСЃС‚СЂР°РЅРµРЅРёРµ РЅРµРёСЃРїСЂР°РІРЅРѕСЃС‚РµР№, РљРѕРјР°РЅРґРЅР°СЏ СЃС‚СЂРѕРєР° Рё РґСЂ.). Р Р°Р±РѕС‚Р°РµС‚ С‚РѕР»СЊРєРѕ РІРЅСѓС‚СЂРё WinRE."},
    {"savedata", L"РљРѕРїРёСЂСѓРµС‚ Р¶СѓСЂРЅР°Р»С‹ Рё РѕС‚С‡С‘С‚С‹ РЅР° РІС‹Р±СЂР°РЅРЅС‹Р№ РґРёСЃРє: РѕРїРµСЂР°С‚РёРІРЅС‹Р№ РґРёСЃРє WinPE/WinRE РѕС‡РёС‰Р°РµС‚СЃСЏ РїСЂРё РїРµСЂРµР·Р°РіСЂСѓР·РєРµ, Рё СЂРµР·СѓР»СЊС‚Р°С‚С‹ РёРЅР°С‡Рµ РїРѕС‚РµСЂСЏСЋС‚СЃСЏ."},
    {"backups", L"РџРѕРєР°Р·С‹РІР°РµС‚ СЂРµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё РєСѓСЃС‚РѕРІ СЂРµРµСЃС‚СЂР°, BCD Рё С„Р°Р№Р»РѕРІ, СЃРґРµР»Р°РЅРЅС‹Рµ РїРµСЂРµРґ РёР·РјРµРЅРµРЅРёСЏРјРё: СЂР°Р·РјРµСЂ Рё СЂР°СЃРїРѕР»РѕР¶РµРЅРёРµ. РњРѕР¶РЅРѕ СѓРґР°Р»РёС‚СЊ РІСЃРµ РєРѕРїРёРё РёР»Рё РѕС‚РєСЂС‹С‚СЊ РїР°РїРєСѓ. РҐСЂР°РЅРёС‚СЃСЏ РѕРґРЅР° РєРѕРїРёСЏ РЅР° СЃРёСЃС‚РµРјСѓ."},
    {"secureboot", L"РџСЂРѕРІРµСЂРєР° Secure Boot: РІРєР»СЋС‡С‘РЅ Р»Рё РѕРЅ Рё РµСЃС‚СЊ Р»Рё РІ KEK Рё db СЃРµСЂС‚РёС„РёРєР°С‚С‹ 2023 РіРѕРґР° РІР·Р°РјРµРЅ РёСЃС‚РµРєР°СЋС‰РёС… РІ 2026. РўРѕР»СЊРєРѕ С‡С‚РµРЅРёРµ."},
    {"storagefix", L"Р•СЃР»Рё РґРёСЃРє РЅРµ РІРёРґРµРЅ РёР»Рё СЃРёРЅРёР№ СЌРєСЂР°РЅ 0x7B: РїРѕРёСЃРє С‚РѕРјРѕРІ, Р·Р°Р±Р»РѕРєРёСЂРѕРІР°РЅРЅС‹С… BitLocker (СЂР°Р·Р±Р»РѕРєРёСЂРѕРІРєР° РєР»СЋС‡РѕРј), СѓСЃС‚СЂРѕР№СЃС‚РІ Р±РµР· РґСЂР°Р№РІРµСЂР° С…СЂР°РЅРёР»РёС‰Р° (Intel VMD/RST, NVMe, RAID) Рё Р·Р°РіСЂСѓР·РєР° РґСЂР°Р№РІРµСЂР°."},
};

const wchar_t* DescOf(const char* name) {
    for (const auto& d : kDescs) if (!strcmp(d.name, name)) return d.text;
    return L"";
}

// Purpose of a command -> colour (see ui_theme.h)
theme::Cat CatOfAction(const char* n) {
    static const char* guide[] = {"triage", "playbooks", "catalog"};
    static const char* repair[] = {"repair", "fixes", "registry", "bcd", "autoruns", "winre", "winre-menu", "storagefix", "rstrui", "apps", "explorer"};
    static const char* protect[] = {"restorepoint", "savedata", "backups"};
    for (auto g : guide) if (!strcmp(n, g)) return theme::Cat::Guide;
    for (auto g : repair) if (!strcmp(n, g)) return theme::Cat::Repair;
    for (auto g : protect) if (!strcmp(n, g)) return theme::Cat::Protect;
    if (!strcmp(n, "about")) return theme::Cat::Info;
    return theme::Cat::Diag;
}

std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

// Log line colour by level tag: "2026-... [WARN ] text"
int LevelOf(const std::string& line) {
    if (line.find("в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ") != std::string::npos) return 5;
    size_t p = line.find('[');
    if (p != std::string::npos && p < 30 && line.size() > p + 6) {
        if (!line.compare(p, 5, "[WARN")) return 1;
        if (!line.compare(p, 6, "[ERROR")) return 2;
        if (!line.compare(p, 4, "[OUT")) return 3;
        if (!line.compare(p, 3, "[OK")) return 4;
    }
    return 0;
}

void AppendLog(const std::string& utf8) {
    std::wstring w = W(utf8) + L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    if (len > 3000000) {  // keep the control responsive: drop the oldest ~1/3
        SendMessageW(g_log, EM_SETSEL, 0, 1000000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
    SendMessageW(g_log, EM_SETSEL, (WPARAM)-1, -1);   // end of text (works for EDIT and RichEdit)
    if (g_logRich) {
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_COLOR | CFM_BOLD;
        bool bold = false;
        cf.crTextColor = theme::LevelColor(LevelOf(utf8), &bold);
        cf.dwEffects = bold ? CFE_BOLD : 0;
        SendMessageW(g_log, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
    SendMessageW(g_log, WM_VSCROLL, SB_BOTTOM, 0);
}

void OpenPath(const std::string& p) {
    HINSTANCE r = ShellExecuteW(g_wnd, L"open", W(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        r = ShellExecuteW(g_wnd, nullptr, L"notepad.exe", W(p).c_str(), nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) Logger::I().log(Lvl::Warn, "РќРµ СѓРґР°Р»РѕСЃСЊ РѕС‚РєСЂС‹С‚СЊ Р°РІС‚РѕРјР°С‚РёС‡РµСЃРєРё (РЅРµС‚ РїСЂРѕРІРѕРґРЅРёРєР°/Р±СЂР°СѓР·РµСЂР°). РџСѓС‚СЊ: " + p);
    }
}

void FillTargets() {
    SendMessageW(g_target, CB_RESETCONTENT, 0, 0);
    g_targets = FindTargets();
    for (auto& t : g_targets) SendMessageW(g_target, CB_ADDSTRING, 0, (LPARAM)W(t.label).c_str());
    if (g_targets.empty()) SendMessageW(g_target, CB_ADDSTRING, 0, (LPARAM)TrP(L"(СѓСЃС‚Р°РЅРѕРІР»РµРЅРЅР°СЏ Windows РЅРµ РЅР°Р№РґРµРЅР°)"));
    SendMessageW(g_target, CB_SETCURSEL, 0, 0);
}

void Layout();
void ShowTabButtons() {
    int sel = TabCtrl_GetCurSel(g_tab);
    for (int i = 0; i < kActionCount; ++i) ShowWindow(g_btn[i], kActions[i].tab == sel ? SW_SHOW : SW_HIDE);
    Layout();   // options (period, DISM source, host...) follow the selected tab
}

// Options live inside the tab they belong to, right under that tab's buttons (tab: 2 Р”РёСЃРєРё, 3 Р¦РµР»РѕСЃС‚РЅРѕСЃС‚СЊ, 5 РЎРѕР±С‹С‚РёСЏ, 6 РЎРµС‚СЊ).
int OptionsStripH(int tab) {
    switch (tab) {
        case 2: case 5: case 6: return S(70);
        case 3: return S(104);
        default: return 0;
    }
}

void PlaceOptions(const RECT& tr, const int* cnt, int cols, int bh, int gap) {
    int sel = TabCtrl_GetCurSel(g_tab);
    HWND all[] = {g_lbl[1], g_days, g_ext, g_force, g_lbl[2], g_host, g_lbl[3], g_port, g_lbl[4], g_source, g_hint};
    for (HWND o : all) ShowWindow(o, SW_HIDE);
    if (sel < 0 || sel > 15 || !OptionsStripH(sel)) return;
    int rowsB = (cnt[sel] + cols - 1) / cols;
    int ox = tr.left + S(10), oy = tr.top + S(6) + rowsB * (bh + gap) + S(6), ow = (int)(tr.right - tr.left) - S(20);
    int rowH = S(26), step = S(32);
    auto put = [&](HWND o, int x, int yy, int w) { MoveWindow(o, x, yy, w, rowH, TRUE); ShowWindow(o, SW_SHOW); };
    auto label = [&](HWND o, int x, int yy, int w) { MoveWindow(o, x, yy + S(4), w, rowH - S(4), TRUE); ShowWindow(o, SW_SHOW); };
    const wchar_t* hint = L"";
    int rows = 1;
    if (sel == 2) {
        label(g_lbl[1], ox, oy, S(100));
        put(g_days, ox + S(104), oy, S(52));
        SetWindowTextW(g_ext, L"Р Р°СЃС€РёСЂРµРЅРЅС‹Р№ СЂРµР¶РёРј: chkdsk /scan (РґРѕР»СЊС€Рµ)");
        put(g_ext, ox + S(176), oy, ow - S(176));
        hint = L"РџРµСЂРёРѕРґ вЂ” Р·Р° СЃРєРѕР»СЊРєРѕ РїРѕСЃР»РµРґРЅРёС… РґРЅРµР№ РёСЃРєР°С‚СЊ РѕС€РёР±РєРё РґРёСЃРєРѕРІ Рё С„Р°Р№Р»РѕРІРѕР№ СЃРёСЃС‚РµРјС‹ РІ Р¶СѓСЂРЅР°Р»Рµ СЃРѕР±С‹С‚РёР№. Р Р°СЃС€РёСЂРµРЅРЅС‹Р№ СЂРµР¶РёРј РґРѕР±Р°РІР»СЏРµС‚ РїСЂРѕРІРµСЂРєСѓ chkdsk /scan (Р·Р°РЅРёРјР°РµС‚ Р±РѕР»СЊС€Рµ РІСЂРµРјРµРЅРё).";
    } else if (sel == 3) {
        SetWindowTextW(g_ext, L"Р Р°СЃС€РёСЂРµРЅРЅС‹Р№ СЂРµР¶РёРј: DISM /ScanHealth (РґРѕР»СЊС€Рµ)");
        put(g_ext, ox, oy, S(360));
        put(g_force, ox + S(370), oy, ow - S(370));
        label(g_lbl[4], ox, oy + step, S(170));
        put(g_source, ox + S(176), oy + step, ow - S(176));
        rows = 2;
        hint = L"РСЃС‚РѕС‡РЅРёРє DISM (/Source) РЅСѓР¶РµРЅ, РµСЃР»Рё Windows Update РЅРµРґРѕСЃС‚СѓРїРµРЅ: СѓРєР°Р¶РёС‚Рµ РїР°РїРєСѓ СЃ С„Р°Р№Р»Р°РјРё, РЅР°РїСЂРёРјРµСЂ D:\\sources\\sxs, РёР»Рё WIM:D:\\sources\\install.wim:1. РџСѓСЃС‚Рѕ вЂ” РѕР±С‹С‡РЅС‹Р№ РёСЃС‚РѕС‡РЅРёРє. РўРѕС‡РєР° РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ РѕР±С‹С‡РЅРѕ СЃРѕР·РґР°С‘С‚СЃСЏ РЅРµ С‡Р°С‰Рµ СЂР°Р·Р° РІ 24 С‡Р°СЃР°; РіР°Р»РєР° РѕС‚РєР»СЋС‡Р°РµС‚ СЌС‚Рѕ РѕРіСЂР°РЅРёС‡РµРЅРёРµ.";
    } else if (sel == 5) {
        label(g_lbl[1], ox, oy, S(100));
        put(g_days, ox + S(104), oy, S(52));
        hint = L"РџРµСЂРёРѕРґ вЂ” Р·Р° СЃРєРѕР»СЊРєРѕ РїРѕСЃР»РµРґРЅРёС… РґРЅРµР№ РїРѕРєР°Р·С‹РІР°С‚СЊ РѕС€РёР±РєРё Рё РїСЂРµРґСѓРїСЂРµР¶РґРµРЅРёСЏ РёР· Р¶СѓСЂРЅР°Р»РѕРІ System Рё Application.";
    } else if (sel == 6) {
        label(g_lbl[2], ox, oy, S(140));
        put(g_host, ox + S(144), oy, S(220));
        label(g_lbl[3], ox + S(380), oy, S(50));
        put(g_port, ox + S(432), oy, S(70));
        hint = L"РҐРѕСЃС‚ Рё РїРѕСЂС‚ РґР»СЏ РїСЂРѕРІРµСЂРєРё СЃРІСЏР·Рё: РїРёРЅРі, СЂР°Р·СЂРµС€РµРЅРёРµ РёРјРµРЅРё (DNS) Рё РїРѕРґРєР»СЋС‡РµРЅРёРµ Рє TCP-РїРѕСЂС‚Сѓ (443 вЂ” HTTPS, 80 вЂ” HTTP). РџРѕРґРѕР№РґС‘С‚ Р»СЋР±РѕР№ РґРѕСЃС‚СѓРїРЅС‹Р№ СЃР°Р№С‚ РёР»Рё Р°РґСЂРµСЃ.";
    }
    SetWindowTextW(g_hint, hint);
    MoveWindow(g_hint, ox, oy + rows * step, ow, S(36), TRUE);
    ShowWindow(g_hint, SW_SHOW);
}

void Layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8);
    int hdrH = S(72);
    MoveWindow(g_banner, 0, 0, 1, 1, TRUE);   // hidden: the header band is painted in WM_PAINT
    MoveWindow(g_exitBtn, W_ - m - S(104), S(20), S(104), S(32), TRUE);
    MoveWindow(g_langBtn, W_ - m - S(104) - S(8) - S(96), S(20), S(96), S(32), TRUE);
    int y = hdrH + S(32);
    int rh = S(24);
    y += S(2);
    g_targetFrame = {m, y, W_ - m, y + S(42)};
    MoveWindow(g_lbl[0], m + S(14), y + S(11), S(104), rh, TRUE);
    MoveWindow(g_target, m + S(122), y + S(7), W_ - 2 * m - S(122) - S(124), S(300), TRUE);
    MoveWindow(g_refreshBtn, W_ - m - S(112), y + S(6), S(100), S(30), TRUE);
    y += S(52);
    int gap = S(8), bh = S(44);
    int innerW = W_ - 2 * m - S(16);
    int cols = std::max(1, (innerW + gap) / (S(235) + gap));
    int cnt[16] = {0}, maxRows = 1;
    for (int i = 0; i < kActionCount; ++i) cnt[kActions[i].tab]++;
    for (int t = 0; t < 16; ++t) maxRows = std::max(maxRows, (cnt[t] + cols - 1) / cols);
    int need = 0;   // tallest tab: its button rows + its options strip
    for (int t = 0; t < 16; ++t) {
        int rowsT = (cnt[t] + cols - 1) / cols;
        need = std::max(need, rowsT * (bh + gap) + OptionsStripH(t));
    }
    (void)maxRows;
    int tabHdr = S(42);
    int tabH = tabHdr + need + S(8);
    MoveWindow(g_tab, m, y, W_ - 2 * m, tabH, TRUE);
    {   // the tab strip spans the whole width: every tab gets an equal share up to the right edge
        RECT tc;
        GetClientRect(g_tab, &tc);
        int n = (int)(sizeof kTabs / sizeof kTabs[0]);
        TabCtrl_SetItemSize(g_tab, std::max(S(60), (int)(tc.right - tc.left - S(6)) / n), S(36));   // a few pixels spare: no scroll arrows, every tab visible
    }
    RECT tr = {m, y, W_ - m, y + tabH};
    TabCtrl_AdjustRect(g_tab, FALSE, &tr);
    int bw = std::max(S(150), ((int)(tr.right - tr.left) - S(12) - gap * (cols - 1)) / cols);
    int idxInTab[16] = {0};
    for (int i = 0; i < kActionCount; ++i) {
        int t = kActions[i].tab, k = idxInTab[t]++;
        MoveWindow(g_btn[i], tr.left + S(6) + (k % cols) * (bw + gap), tr.top + S(6) + (k / cols) * (bh + gap), bw, bh, TRUE);
    }
    PlaceOptions(tr, cnt, cols, bh, gap);
    y += tabH + S(6);
    y += m;
    int barH = S(34);
    MoveWindow(g_log, m, y, W_ - 2 * m, std::max(S(60), H - y - barH - 2 * m - S(22)), TRUE);
    int by = H - barH - S(22) - m + S(0);
    by = y + std::max(S(60), H - y - barH - 2 * m - S(22)) + m;
    int x = m;
    int ws[] = {S(110), S(200), S(170), S(180), S(170), S(100)};
    HWND hs[] = {g_cancelBtn, GetDlgItem(g_wnd, IDC_OPENREP), GetDlgItem(g_wnd, IDC_OPENLOG), GetDlgItem(g_wnd, IDC_SAVELOG), GetDlgItem(g_wnd, IDC_COPYLOG), GetDlgItem(g_wnd, IDC_CLEAR)};
    for (int i = 0; i < 6; ++i) { MoveWindow(hs[i], x, by, ws[i], barH - S(4), TRUE); x += ws[i] + gap; }
    MoveWindow(g_prog, x + gap, by + S(6), std::max(S(60), W_ - x - m - gap), S(18), TRUE);
    MoveWindow(g_status, m, H - S(22) - m / 2, W_ - 2 * m, S(20), TRUE);
}

void SetRunning(bool run) {
    g_running = run;
    for (int i = 0; i < kActionCount; ++i) EnableWindow(g_btn[i], !run);
    EnableWindow(g_refreshBtn, !run);
    EnableWindow(g_cancelBtn, run);
    if (g_cat) EnableWindow(g_catRun, !run);
    SendMessageW(g_prog, PBM_SETMARQUEE, run, 30);
    ShowWindow(g_prog, run ? SW_SHOW : SW_HIDE);
}

struct JobData { Ctx ctx; std::function<void(Ctx&)> fn; std::string name; };

DWORD WINAPI JobThread(LPVOID p) {
    std::unique_ptr<JobData> d((JobData*)p);
    try { d->fn(d->ctx); }
    catch (const std::exception& e) { Logger::I().log(Lvl::Error, std::string("РќРµРѕР±СЂР°Р±РѕС‚Р°РЅРЅРѕРµ РёСЃРєР»СЋС‡РµРЅРёРµ: ") + e.what()); }
    catch (...) { Logger::I().log(Lvl::Error, "РќРµРѕР±СЂР°Р±РѕС‚Р°РЅРЅРѕРµ РёСЃРєР»СЋС‡РµРЅРёРµ"); }
    PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

JobData* MakeJob(const std::string& name, std::function<void(Ctx&)> fn) {
    auto* d = new JobData();
    int ti = (int)SendMessageW(g_target, CB_GETCURSEL, 0, 0);
    if (ti >= 0 && ti < (int)g_targets.size()) d->ctx.target = g_targets[ti];
    d->ctx.opt.days = std::max(1, std::min(90, _wtoi(GetText(g_days).c_str())));
    d->ctx.opt.extended = SendMessageW(g_ext, BM_GETCHECK, 0, 0) == BST_CHECKED;
    d->ctx.opt.force = SendMessageW(g_force, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::string host = trim(U(GetText(g_host)));
    if (!host.empty()) d->ctx.opt.host = host;
    int port = _wtoi(GetText(g_port).c_str());
    d->ctx.opt.port = (port > 0 && port < 65536) ? port : 443;
    d->ctx.opt.source = trim(U(GetText(g_source)));
    d->ctx.outDir = DataDir() + "Reports\\" + Stamp() + "_" + name + "\\";
    MakeDirs(d->ctx.outDir);
    g_lastOut = d->ctx.outDir;
    d->ctx.cancel = &g_cancel;
    d->ctx.res = &g_results;
    d->fn = fn;
    d->name = name;
    g_jobName = name;
    return d;
}

void RunJob(JobData* d, const std::string& title) {
    g_cancel = false;
    g_jobStart = GetTickCount64();
    SetRunning(true);
    Logger::I().log(Lvl::Info, "в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ Р—РђРџРЈРЎРљ: " + title + " в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ");
    Logger::I().log(Lvl::Info, "Р¦РµР»РµРІР°СЏ РћРЎ: " + (d->ctx.target.valid ? d->ctx.target.label : std::string("РЅРµ РІС‹Р±СЂР°РЅР°")) + "; РїР°РїРєР° РѕС‚С‡С‘С‚Р°: " + d->ctx.outDir);
    SetWindowTextW(g_status, L"Р’С‹РїРѕР»РЅСЏРµС‚СЃСЏвЂ¦");
    HANDLE h = CreateThread(nullptr, 0, JobThread, d, 0, nullptr);
    if (h) CloseHandle(h);
    else { delete d; SetRunning(false); }
}

theme::Cat TabCat(int tab) {
    switch (tab) {
        case 0: return theme::Cat::Guide;
        case 3: case 7: return theme::Cat::Repair;
        default: return theme::Cat::Diag;
    }
}

const wchar_t* CatLabel(theme::Cat c) {
    switch (c) {
        case theme::Cat::Diag: return L"Р”РёР°РіРЅРѕСЃС‚РёРєР° вЂ” С‚РѕР»СЊРєРѕ С‡С‚РµРЅРёРµ";
        case theme::Cat::Guide: return L"РњР°СЃС‚РµСЂР° Рё СЃС†РµРЅР°СЂРёРё";
        case theme::Cat::Repair: return L"РР·РјРµРЅСЏРµС‚ СЃРёСЃС‚РµРјСѓ";
        case theme::Cat::Protect: return L"Р—Р°С‰РёС‚Р° Рё СЃРѕС…СЂР°РЅРµРЅРёРµ";
        default: return L"РЎР»СѓР¶РµР±РЅРѕРµ";
    }
}

// Hover tooltip for every command button: title + what the command does.
HWND g_tip = nullptr;
std::wstring g_tipText;

void CreateTooltips() {
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_tip) return;
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, S(420));
    SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
    SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
    for (int i = 0; i < kActionCount; ++i) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = g_wnd;
        ti.uId = (UINT_PTR)g_btn[i];
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

// TTN_GETDISPINFO: title and text for the button under the pointer
void TooltipNeedText(NMTTDISPINFOW* di) {
    int id = GetDlgCtrlID((HWND)di->hdr.idFrom);
    if (id < IDC_ACTION0 || id >= IDC_ACTION0 + kActionCount) return;
    const Action& a = kActions[id - IDC_ACTION0];
    std::wstring t = W(U(std::wstring(a.text)));
    while (!t.empty() && (t.back() == L'вЂ¦' || t.back() == L'.')) t.pop_back();
    g_tipText = W(U(std::wstring(DescOf(a.name))));
    if (g_tipText.empty()) g_tipText = t;
    SendMessageW(g_tip, TTM_SETTITLEW, TTI_INFO, (LPARAM)t.c_str());
    di->lpszText = const_cast<wchar_t*>(g_tipText.c_str());
}

void ManageBackups() {
    std::vector<BackupItem> bk = ListBackups();
    if (bk.empty()) { MessageBoxW(g_wnd, L"Р РµР·РµСЂРІРЅС‹С… РєРѕРїРёР№ РЅРµС‚: РїР°РїРєР° Data\\Backups РїСѓСЃС‚Р°.", L"Р РµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё", MB_OK | MB_ICONINFORMATION); return; }
    std::wstring msg = W(fmt("Р РµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё С…СЂР°РЅСЏС‚СЃСЏ РїРѕ РѕРґРЅРѕР№ РЅР° СЃРёСЃС‚РµРјСѓ Рё РЅРµ РЅР°РєР°РїР»РёРІР°СЋС‚СЃСЏ.\n\n%s\nР”Р° вЂ” СѓРґР°Р»РёС‚СЊ РІСЃРµ РєРѕРїРёРё.\nРќРµС‚ вЂ” РѕС‚РєСЂС‹С‚СЊ РїР°РїРєСѓ СЃ РєРѕРїРёСЏРјРё.\nРћС‚РјРµРЅР° вЂ” Р·Р°РєСЂС‹С‚СЊ РѕРєРЅРѕ.", BackupsSummary(bk).c_str()));
    int r = MessageBoxW(g_wnd, msg.c_str(), L"Р РµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
    if (r == IDYES) {
        if (MessageBoxW(g_wnd, L"РЈРґР°Р»РёС‚СЊ РІСЃРµ СЂРµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё? Р’РµСЂРЅСѓС‚СЊ РёР·РјРµРЅС‘РЅРЅС‹Рµ СЃРёСЃС‚РµРјРЅС‹Рµ С„Р°Р№Р»С‹ РёР· РЅРёС… РїРѕСЃР»Рµ СЌС‚РѕРіРѕ Р±СѓРґРµС‚ РЅРµР»СЊР·СЏ.", L"РџРѕРґС‚РІРµСЂР¶РґРµРЅРёРµ", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            int n = DeleteBackups(bk);
            Logger::I().log(Lvl::Ok, fmt("РЈРґР°Р»РµРЅРѕ СЂРµР·РµСЂРІРЅС‹С… РєРѕРїРёР№: %d", n));
        }
    } else if (r == IDNO) OpenPath(DataDir() + "Backups");
}

// Copies left by other computers (the Data folder travels on the USB drive) and old-format folders are offered for deletion at start.
void CheckForeignBackups() {
    std::vector<BackupItem> bk = ListBackups();
    if (bk.empty()) return;
    std::set<std::string> mine;
    mine.insert(VolumeId(""));
    for (auto& t : g_targets) mine.insert(VolumeId(t.root));
    std::vector<BackupItem> foreign;
    for (auto& i : bk) if (i.legacy || !mine.count(i.name)) foreign.push_back(i);
    if (foreign.empty()) return;
    std::wstring msg = W(fmt("Р’ РїР°РїРєРµ Data\\Backups РЅР°Р№РґРµРЅС‹ СЂРµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё РѕС‚ РґСЂСѓРіРёС… СЃРёСЃС‚РµРј РёР»Рё СЃС‚Р°СЂРѕРіРѕ С„РѕСЂРјР°С‚Р°.\nРЎРµР№С‡Р°СЃ Р·РґРµСЃСЊ РґРѕР»Р¶РЅС‹ Р±С‹С‚СЊ С‚РѕР»СЊРєРѕ РєРѕРїРёРё РїСЂРѕРІРµСЂСЏРµРјС‹С… РІР°РјРё СЃРёСЃС‚РµРј.\n\n%s\nРЈРґР°Р»РёС‚СЊ СЌС‚Рё РєРѕРїРёРё?", BackupsSummary(foreign).c_str()));
    if (MessageBoxW(g_wnd, msg.c_str(), L"Р РµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё РґСЂСѓРіРёС… СЃРёСЃС‚РµРј", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        int n = DeleteBackups(foreign);
        Logger::I().log(Lvl::Ok, fmt("РЈРґР°Р»РµРЅРѕ СЂРµР·РµСЂРІРЅС‹С… РєРѕРїРёР№ РґСЂСѓРіРёС… СЃРёСЃС‚РµРј: %d", n));
    } else Logger::I().log(Lvl::Info, "Р РµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё РґСЂСѓРіРёС… СЃРёСЃС‚РµРј РѕСЃС‚Р°РІР»РµРЅС‹ РїРѕ РІС‹Р±РѕСЂСѓ РїРѕР»СЊР·РѕРІР°С‚РµР»СЏ.");
}

void StartAction(int idx) {
    const Action& a = kActions[idx];
    if (!a.fn) {
        if (!strcmp(a.name, "open-report")) OpenLastReport();
        else if (!strcmp(a.name, "catalog")) OpenCatalog();
        else if (!strcmp(a.name, "triage")) OpenTriage();
        else if (!strcmp(a.name, "playbooks")) OpenPlaybooks();
        else if (!strcmp(a.name, "registry")) OpenRegistryEditor();
        else if (!strcmp(a.name, "bcd")) OpenBcdManager();
        else if (!strcmp(a.name, "autoruns")) OpenAutoruns();
        else if (!strcmp(a.name, "apps")) OpenInstalledApps();
        else if (!strcmp(a.name, "explorer")) OpenOfflineExplorer();
        else if (!strcmp(a.name, "fixes")) OpenFixWizard();
        else if (!strcmp(a.name, "rstrui")) { Target t = CurrentTarget(); QuickCmd(t.valid && t.online ? 56 : 57); }
        else if (!strcmp(a.name, "restore-info")) QuickCmd(59);
        else if (!strcmp(a.name, "winre")) OpenWinReManager();
        else if (!strcmp(a.name, "savedata")) OpenSaveData();
        else if (!strcmp(a.name, "backups")) ManageBackups();
        else if (!strcmp(a.name, "secureboot")) QuickCmd(80);
        else if (!strcmp(a.name, "storagefix")) OpenStorageAccess();
        else if (!strcmp(a.name, "winre-menu")) {
            const Env& e = GetEnv();
            if (e.winreShell) {
                if (MessageBoxW(g_wnd, L"Р—Р°РєСЂС‹С‚СЊ WinRepair Рё РѕС‚РєСЂС‹С‚СЊ С€С‚Р°С‚РЅРѕРµ РјРµРЅСЋ WinRE (Р’С‹Р±РѕСЂ РґРµР№СЃС‚РІРёСЏ)?\n\nРџРѕСЃР»Рµ РІС‹С…РѕРґР° РёР· РјРµРЅСЋ РјРѕР¶РЅРѕ РІРµСЂРЅСѓС‚СЊСЃСЏ С‡РµСЂРµР· В«РљРѕРјР°РЅРґРЅР°СЏ СЃС‚СЂРѕРєР°В» в†’ WinRepair.exe.", L"WinRE", MB_YESNO | MB_ICONQUESTION) == IDYES) DestroyWindow(g_wnd);
            } else if (e.isWinRE) {
                std::string rec = PathExists(e.sysDrive + "\\sources\\recovery\\RecEnv.exe") ? e.sysDrive + "\\sources\\recovery\\RecEnv.exe" : e.winDir + "\\System32\\Recovery\\RecEnv.exe";
                ShellExecuteW(g_wnd, L"open", W(rec).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else MessageBoxW(g_wnd, L"Р­С‚Р° С„СѓРЅРєС†РёСЏ СЂР°Р±РѕС‚Р°РµС‚ С‚РѕР»СЊРєРѕ РІРЅСѓС‚СЂРё WinRE. РћС‚РєСЂРѕР№С‚Рµ В«WinRE: РІС…РѕРґ РІ СЃСЂРµРґСѓ РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏвЂ¦В», С‡С‚РѕР±С‹ Р·Р°РіСЂСѓР·РёС‚СЊСЃСЏ РІ WinRE.", L"WinRE", MB_OK | MB_ICONINFORMATION);
        }
        else ShowAbout();
        return;
    }
    if (a.confirm && MessageBoxW(g_wnd, a.confirm, L"РџРѕРґС‚РІРµСЂР¶РґРµРЅРёРµ РёР·РјРµРЅСЏСЋС‰РµРіРѕ РґРµР№СЃС‚РІРёСЏ", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    if (*DescOf(a.name)) Logger::I().log(Lvl::Info, "РќР°Р·РЅР°С‡РµРЅРёРµ: " + U(std::wstring(DescOf(a.name))));
    RunJob(MakeJob(a.name, a.fn), U(a.text));
}

void StartCommand(const CmdItem& it, const CmdParams& p) {
    JobData* d = MakeJob(fmt("cmd%02d", it.no), ModCommand);
    d->ctx.opt.cmdNo = it.no;
    d->ctx.opt.cp = p;
    RunJob(d, fmt("РљРѕРјР°РЅРґР° в„–%d: %s", it.no, it.title));
}

void ChangeLanguage() {
    if (g_running) { MessageBoxW(g_wnd, L"Р”РѕР¶РґРёС‚РµСЃСЊ Р·Р°РІРµСЂС€РµРЅРёСЏ С‚РµРєСѓС‰РµР№ РѕРїРµСЂР°С†РёРё.", L"Р—Р°РЅСЏС‚Рѕ", MB_OK | MB_ICONINFORMATION); return; }
    bool toEn = CurrentLang() == Lang::RU;
    const wchar_t* q = toEn ? L"Switch the interface to English? The program will restart." : L"РЎРјРµРЅРёС‚СЊ СЏР·С‹Рє РёРЅС‚РµСЂС„РµР№СЃР° РЅР° СЂСѓСЃСЃРєРёР№? РџСЂРѕРіСЂР°РјРјР° РїРµСЂРµР·Р°РїСѓСЃС‚РёС‚СЃСЏ.";
    if (MessageBoxW(g_wnd, q, L"Language / РЇР·С‹Рє", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    SetLanguage(toEn ? Lang::EN : Lang::RU);
    wchar_t exe[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr, SW_SHOWNORMAL);
    DestroyWindow(g_wnd);
}

void OpenLastReport() {
    if (!g_results.lastReportHtml.empty() && PathExists(g_results.lastReportHtml)) OpenPath(g_results.lastReportHtml);
    else Logger::I().log(Lvl::Warn, "HTML-РѕС‚С‡С‘С‚Р° РµС‰С‘ РЅРµС‚ вЂ” СЃРЅР°С‡Р°Р»Р° Р·Р°РїСѓСЃС‚РёС‚Рµ В«РџРѕР»РЅСѓСЋ РґРёР°РіРЅРѕСЃС‚РёРєСѓВ».");
}

void ShowAbout() {
    const Env& e = GetEnv();
    std::string t = fmt("%s %s\n\nРќР°С‚РёРІРЅРѕРµ РїСЂРёР»РѕР¶РµРЅРёРµ Win32 (C++17), Р±РµР· Р·Р°РІРёСЃРёРјРѕСЃС‚РµР№ РѕС‚ .NET, PowerShell Рё VC++ Runtime.\n\n"
                        "РЎСЂРµРґР°: %s, %s%s\nРђРґРјРёРЅРёСЃС‚СЂР°С‚РѕСЂ: %s\nР–СѓСЂРЅР°Р»С‹ Рё РѕС‚С‡С‘С‚С‹: %s",
                        kAppName, kAppVersion, e.osName.c_str(), e.arch.c_str(), e.isWinPE ? " (WinPE)" : "", e.isAdmin ? "РґР°" : "РЅРµС‚", DataDir().c_str());
    MessageBoxW(g_wnd, W(t).c_str(), L"Рћ РїСЂРѕРіСЂР°РјРјРµ", MB_OK | MB_ICONINFORMATION);
}

void CopyLog() {   // whole log to the clipboard (handy for pasting into a ticket or a message)
    std::wstring w = GetText(g_log), n;
    for (size_t i = 0; i < w.size(); ++i) {
        n += w[i];
        if (w[i] == L'\r' && (i + 1 >= w.size() || w[i + 1] != L'\n')) n += L'\n';
    }
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (n.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), n.c_str(), (n.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
    SetWindowTextW(g_status, L"Р–СѓСЂРЅР°Р» СЃРєРѕРїРёСЂРѕРІР°РЅ РІ Р±СѓС„РµСЂ РѕР±РјРµРЅР°.");
}

void SaveLogAs() {
    wchar_t file[MAX_PATH] = L"WinRepair_log.txt";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = g_wnd;
    o.lpstrFilter = L"РўРµРєСЃС‚ (*.txt)\0*.txt\0Р’СЃРµ С„Р°Р№Р»С‹\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH;
    o.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&o)) return;
    std::wstring w = GetText(g_log), n;
    for (size_t i = 0; i < w.size(); ++i) {   // RichEdit stores line breaks as a lone CR
        n += w[i];
        if (w[i] == L'\r' && (i + 1 >= w.size() || w[i + 1] != L'\n')) n += L'\n';
    }
    if (WriteText(U(file), U(n))) Logger::I().log(Lvl::Ok, "Р–СѓСЂРЅР°Р» СЃРѕС…СЂР°РЅС‘РЅ: " + U(file));
}

// ---- command catalogue window ---------------------------------------------------------------------------
Target CurrentTarget() {
    int ti = (int)SendMessageW(g_target, CB_GETCURSEL, 0, 0);
    return (ti >= 0 && ti < (int)g_targets.size()) ? g_targets[ti] : Target();
}

CmdParams ReadParams() {
    CmdParams p;
    p.vol = trim(U(GetText(g_catEd[0])));
    p.src = trim(U(GetText(g_catEd[1])));
    p.host = trim(U(GetText(g_catEd[2])));
    p.win = trim(U(GetText(g_catEd[3])));
    p.efi = trim(U(GetText(g_catEd[4])));
    p.bcd = trim(U(GetText(g_catEd[5])));
    return p;
}

const CmdItem* CatSelected() {
    int s = (int)SendMessageW(g_catList, LB_GETCURSEL, 0, 0);
    if (s < 0 || s >= (int)g_catMap.size()) return nullptr;
    return &Catalog()[(size_t)g_catMap[(size_t)s]];
}

void CatUpdate() {
    const CmdItem* it = CatSelected();
    unsigned mask = it ? it->params : 0;
    for (int i = 0; i < 6; ++i) { EnableWindow(g_catEd[i], (mask & (1u << i)) != 0); EnableWindow(g_catLb[i], (mask & (1u << i)) != 0); }
    EnableWindow(g_catFind, (mask & P_BCD) != 0);
    EnableWindow(g_catRun, it != nullptr && !g_running);
    if (!it) { SetWindowTextW(g_catDesc, L"Р’С‹Р±РµСЂРёС‚Рµ РєРѕРјР°РЅРґСѓ РІ СЃРїРёСЃРєРµ."); return; }
    std::string sw = ScopeWarning(*it, CurrentTarget());
    std::string t = fmt("РљРѕРјР°РЅРґР° в„–%d\r\nРљР°С‚РµРіРѕСЂРёСЏ: %s\r\nРўРёРї: %s\r\nРЈСЂРѕРІРµРЅСЊ: %s\r\n\r\n%s\r\n%s\r\nР‘СѓРґРµС‚ РІС‹РїРѕР»РЅРµРЅРѕ:\r\n%s", it->no, it->cat, KindText(it->kind),
                        RiskText(it->risk), it->note, sw.empty() ? "" : ("\r\n" + sw + "\r\n").c_str(), CmdBuild(*it, ReadParams(), CurrentTarget(), "<РїР°РїРєР° РѕС‚С‡С‘С‚Р°>\\scratch", "<РІСЂРµРјРµРЅРЅС‹Р№ С„Р°Р№Р»>").c_str());
    SetWindowTextW(g_catDesc, W(t).c_str());
}

void CatFillList() {
    int fi = (int)SendMessageW(g_catFilter, CB_GETCURSEL, 0, 0);
    std::string cat = (fi > 0 && fi <= (int)g_catCats.size()) ? g_catCats[(size_t)fi - 1] : "";
    SendMessageW(g_catList, LB_RESETCONTENT, 0, 0);
    g_catMap.clear();
    const auto& all = Catalog();
    for (size_t i = 0; i < all.size(); ++i) {
        if (!cat.empty() && cat != all[i].cat) continue;
        std::string line = fmt("%2d. [%s] %s", all[i].no, Tr(all[i].cat).c_str(), Tr(all[i].title).c_str());
        SendMessageW(g_catList, LB_ADDSTRING, 0, (LPARAM)W(line).c_str());
        g_catMap.push_back((int)i);
    }
    SendMessageW(g_catList, LB_SETCURSEL, 0, 0);
}

bool ConfirmStart(HWND owner, const CmdItem& it, const CmdParams& p) {
    std::string err;
    if (!CmdValidate(it, p, err)) { MessageBoxW(owner, W(err).c_str(), L"РџСЂРѕРІРµСЂСЊС‚Рµ РїР°СЂР°РјРµС‚СЂС‹", MB_OK | MB_ICONWARNING); return false; }
    Target t = CurrentTarget();
    std::string sw = ScopeWarning(it, t);
    std::string msg = fmt("РљРѕРјР°РЅРґР° в„–%d [%s]\n\n%s\n\nР¦РµР»РµРІР°СЏ РћРЎ: %s\nРўРёРї: %s\nРЈСЂРѕРІРµРЅСЊ: %s\n\n", it.no, it.cat,
                          CmdBuild(it, p, t, "<РїР°РїРєР° РѕС‚С‡С‘С‚Р°>\\scratch", "<РІСЂРµРјРµРЅРЅС‹Р№ С„Р°Р№Р»>").c_str(), t.valid ? t.label.c_str() : "РЅРµ РІС‹Р±СЂР°РЅР°", KindText(it.kind), RiskText(it.risk));
    if (!sw.empty()) msg += sw + "\n\n";
    msg += it.kind == CKind::Manual ? "Р‘СѓРґРµС‚ РїРѕРєР°Р·Р°РЅР° РёРЅСЃС‚СЂСѓРєС†РёСЏ. РќРёС‡РµРіРѕ РЅРµ РІС‹РїРѕР»РЅСЏРµС‚СЃСЏ.\n\nРџРѕРєР°Р·Р°С‚СЊ?" : "Р’С‹РїРѕР»РЅРёС‚СЊ СЌС‚Сѓ РєРѕРјР°РЅРґСѓ?";
    UINT flags = MB_YESNO | (it.risk == Risk::Safe ? MB_ICONQUESTION : (MB_ICONWARNING | MB_DEFBUTTON2));
    if (MessageBoxW(owner, W(msg).c_str(), L"РџРѕРґС‚РІРµСЂР¶РґРµРЅРёРµ РєРѕРјР°РЅРґС‹", flags) != IDYES) return false;
    if (it.risk == Risk::Danger &&
        MessageBoxW(owner, L"РћРџРђРЎРќРђРЇ РѕРїРµСЂР°С†РёСЏ: РІРѕР·РјРѕР¶РЅР° РїРѕС‚РµСЂСЏ Р·Р°РіСЂСѓР·РєРё РёР»Рё РґР°РЅРЅС‹С….\n\nРЈ РІР°СЃ РµСЃС‚СЊ СЂРµР·РµСЂРІРЅР°СЏ РєРѕРїРёСЏ, Рё РІС‹ РїРѕРЅРёРјР°РµС‚Рµ РїРѕСЃР»РµРґСЃС‚РІРёСЏ?\nРџРѕРґС‚РІРµСЂРґРёС‚Рµ РїРѕРІС‚РѕСЂРЅРѕ.",
                    L"РџРѕРІС‚РѕСЂРЅРѕРµ РїРѕРґС‚РІРµСЂР¶РґРµРЅРёРµ", MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) != IDYES)
        return false;
    return true;
}

void CatRun() {
    if (g_running) { MessageBoxW(g_cat, L"Р”РѕР¶РґРёС‚РµСЃСЊ Р·Р°РІРµСЂС€РµРЅРёСЏ С‚РµРєСѓС‰РµР№ РѕРїРµСЂР°С†РёРё.", L"Р—Р°РЅСЏС‚Рѕ", MB_OK | MB_ICONINFORMATION); return; }
    const CmdItem* it = CatSelected();
    if (!it) { MessageBoxW(g_cat, L"Р’С‹Р±РµСЂРёС‚Рµ РєРѕРјР°РЅРґСѓ.", L"РљР°С‚Р°Р»РѕРі РєРѕРјР°РЅРґ", MB_OK | MB_ICONINFORMATION); return; }
    CmdParams p = ReadParams();
    if (ConfirmStart(g_cat, *it, p)) StartCommand(*it, p);
}

void QuickCmd(int no) {
    if (g_running) { MessageBoxW(g_wnd, L"Р”РѕР¶РґРёС‚РµСЃСЊ Р·Р°РІРµСЂС€РµРЅРёСЏ С‚РµРєСѓС‰РµР№ РѕРїРµСЂР°С†РёРё.", L"Р—Р°РЅСЏС‚Рѕ", MB_OK | MB_ICONINFORMATION); return; }
    const CmdItem* it = FindCmd(no);
    if (!it) return;
    Target t = CurrentTarget();
    CmdParams p;
    p.vol = t.valid ? std::string(1, t.letter()) + ":" : "C:";
    p.win = t.valid ? t.winDir : "C:\\Windows";
    p.host = trim(U(GetText(g_host)));
    p.src = trim(U(GetText(g_source)));
    if (ConfirmStart(g_wnd, *it, p)) StartCommand(*it, p);
}

void CatFindBcd() {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    auto st = FindBcdStores();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    SendMessageW(g_catEd[5], CB_RESETCONTENT, 0, 0);
    g_bcdPaths.clear();
    for (auto& s : st) { if (s.path.empty()) continue; SendMessageW(g_catEd[5], CB_ADDSTRING, 0, (LPARAM)W(s.label).c_str()); g_bcdPaths.push_back(s.path); }
    if (g_bcdPaths.empty()) MessageBoxW(g_cat, L"РҐСЂР°РЅРёР»РёС‰Р° BCD РЅРµ РЅР°Р№РґРµРЅС‹.", L"BCD", MB_OK | MB_ICONINFORMATION);
    else { SendMessageW(g_catEd[5], CB_SHOWDROPDOWN, TRUE, 0); }
}

HWND MkC(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    bool push = !wcscmp(cls, L"BUTTON") && (style & 0xF) <= BS_DEFPUSHBUTTON;   // command buttons of tool windows are bold
    SendMessageW(h, WM_SETFONT, (WPARAM)(push ? theme::Semi() : g_font), TRUE);
    return h;
}

void CatLayout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8);
    int leftW = std::max(S(380), W_ * 56 / 100);
    MoveWindow(g_catFilter, m, m, leftW, S(300), TRUE);
    MoveWindow(g_catList, m, m + S(32), leftW, H - 2 * m - S(32), TRUE);
    int x = leftW + 2 * m, rw = W_ - x - m, y = m;
    int descH = std::max(S(120), H - S(6 * 50) - S(60) - 2 * m);
    MoveWindow(g_catDesc, x, y, rw, descH, TRUE);
    y += descH + m;
    for (int i = 0; i < 6; ++i) {
        MoveWindow(g_catLb[i], x, y, i == 5 ? rw - S(110) : rw, S(18), TRUE);
        MoveWindow(g_catEd[i], x, y + S(18), rw, i == 5 ? S(300) : S(24), TRUE);
        if (i == 5) MoveWindow(g_catFind, x + rw - S(106), y - S(2), S(106), S(20), TRUE);
        y += S(50);
    }
    MoveWindow(g_catRun, x, H - m - S(32), S(150), S(32), TRUE);
    MoveWindow(g_catClose, x + S(160), H - m - S(32), S(110), S(32), TRUE);
}

void CatContextMenu(HWND h, HWND src, LPARAM lp) {
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) { RECT r; GetWindowRect(src, &r); pt = {r.left + S(20), r.top + S(20)}; }
    else if (src == g_catList) {   // right-click selects the item under the pointer
        POINT c = pt;
        ScreenToClient(src, &c);
        DWORD r = (DWORD)SendMessageW(src, LB_ITEMFROMPOINT, 0, MAKELPARAM(c.x, c.y));
        if (HIWORD(r) == 0 && LOWORD(r) != 0xFFFF) { SendMessageW(src, LB_SETCURSEL, LOWORD(r), 0); CatUpdate(); }
    }
    const CmdItem* it = CatSelected();
    enum { K_RUN = 1, K_NAME, K_CMD, K_CLOSE };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (it && !g_running ? 0 : MF_GRAYED), K_RUN, TrW(L"Р’С‹РїРѕР»РЅРёС‚СЊ РІС‹Р±СЂР°РЅРЅСѓСЋ РєРѕРјР°РЅРґСѓ").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), K_NAME, TrW(L"РљРѕРїРёСЂРѕРІР°С‚СЊ РЅР°Р·РІР°РЅРёРµ").c_str());
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), K_CMD, TrW(L"РљРѕРїРёСЂРѕРІР°С‚СЊ С‚РµРєСЃС‚ РєРѕРјР°РЅРґС‹").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, K_CLOSE, TrW(L"Р—Р°РєСЂС‹С‚СЊ").c_str());
    SetForegroundWindow(h);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
    DestroyMenu(m);
    if (c == K_RUN) CatRun();
    else if (c == K_NAME && it) PutClipboard(W(fmt("%d  %s", it->no, it->title)));
    else if (c == K_CMD && it) PutClipboard(W(CmdBuild(*it, ReadParams(), CurrentTarget(), "<РїР°РїРєР° РѕС‚С‡С‘С‚Р°>\\scratch", "<РІСЂРµРјРµРЅРЅС‹Р№ С„Р°Р№Р»>")));
    else if (c == K_CLOSE) DestroyWindow(h);
}

LRESULT CALLBACK CatProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CONTEXTMENU: CatContextMenu(h, (HWND)wp, lp); return 0;
        case WM_SIZE: if (g_catClose) CatLayout(h); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(820); mm->ptMinTrackSize.y = S(560); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_CAT_LIST && code == LBN_SELCHANGE) CatUpdate();
            else if (id == IDC_CAT_LIST && code == LBN_DBLCLK) CatRun();
            else if (id == IDC_CAT_FILTER && code == CBN_SELCHANGE) { CatFillList(); CatUpdate(); }
            else if (id >= IDC_CAT_E0 && id < IDC_CAT_E0 + 5 && code == EN_CHANGE) CatUpdate();
            else if (id == IDC_CAT_E0 + 5 && code == CBN_SELCHANGE) { int si = (int)SendMessageW(g_catEd[5], CB_GETCURSEL, 0, 0); if (si >= 0 && si < (int)g_bcdPaths.size()) { SetWindowTextW(g_catEd[5], W(g_bcdPaths[(size_t)si]).c_str()); CatUpdate(); } }
            else if (id == IDC_CAT_E0 + 5 && code == CBN_EDITCHANGE) CatUpdate();
            else if (id == IDC_CAT_FIND) CatFindBcd();
            else if (id == IDC_CAT_RUN) CatRun();
            else if (id == IDC_CAT_CLOSE) DestroyWindow(h);
            return 0;
        }
        case WM_DESTROY: g_cat = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void OpenCatalog() {
    if (g_cat) { ShowWindow(g_cat, SW_RESTORE); SetForegroundWindow(g_cat); return; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = CatProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hbrBackground = theme::BgBrush();
        wc.lpszClassName = L"WinRepairCatalog";
        RegisterClassExW(&wc);
        reg = true;
    }
    g_cat = CreateWindowExW(0, L"WinRepairCatalog", L"РљР°С‚Р°Р»РѕРі РєРѕРјР°РЅРґ вЂ” РєР°Р¶РґРѕРµ РґРµР№СЃС‚РІРёРµ С‚СЂРµР±СѓРµС‚ РїРѕРґС‚РІРµСЂР¶РґРµРЅРёСЏ", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(1060), S(680), g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = g_cat;
    g_catFilter = MkC(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CAT_FILTER);
    SendMessageW(g_catFilter, CB_ADDSTRING, 0, (LPARAM)TrP(L"Р’СЃРµ РєР°С‚РµРіРѕСЂРёРё"));
    g_catCats.clear();
    for (const auto& it : Catalog())
        if (std::find(g_catCats.begin(), g_catCats.end(), std::string(it.cat)) == g_catCats.end()) g_catCats.push_back(it.cat);
    for (const auto& c : g_catCats) SendMessageW(g_catFilter, CB_ADDSTRING, 0, (LPARAM)W(c).c_str());
    SendMessageW(g_catFilter, CB_SETCURSEL, 0, 0);
    g_catList = MkC(h, L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, IDC_CAT_LIST, WS_EX_CLIENTEDGE);
    g_catDesc = MkC(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, IDC_CAT_DESC, WS_EX_CLIENTEDGE);
    static const wchar_t* lbl[6] = {L"РўРѕРј (РЅР°РїСЂРёРјРµСЂ C:)", L"РСЃС‚РѕС‡РЅРёРє (/Source: РїР°РїРєР° РёР»Рё wim:С„Р°Р№Р»:РёРЅРґРµРєСЃ)", L"РЈР·РµР» / РёРјСЏ (host, IP)", L"РџСѓС‚СЊ Windows (РЅР°РїСЂРёРјРµСЂ C:\\Windows)", L"EFI-СЂР°Р·РґРµР» (Р±СѓРєРІР°, РЅР°РїСЂРёРјРµСЂ S:)", L"BCD-С…СЂР°РЅРёР»РёС‰Рµ (С„Р°Р№Р»; РїСѓСЃС‚Рѕ = С…СЂР°РЅРёР»РёС‰Рµ С‚РµРєСѓС‰РµР№ СЃСЂРµРґС‹)"};
    Target t = CurrentTarget();
    std::string def[6] = {t.valid ? std::string(1, t.letter()) + ":" : "C:", trim(U(GetText(g_source))), trim(U(GetText(g_host))), t.valid ? t.winDir : "C:\\Windows", "", ""};
    for (int i = 0; i < 6; ++i) {
        g_catLb[i] = MkC(h, L"STATIC", lbl[i], SS_LEFT, IDC_CAT_L0 + i);
        if (i == 5) g_catEd[i] = MkC(h, L"COMBOBOX", W(def[i]).c_str(), CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, IDC_CAT_E0 + i);
        else g_catEd[i] = MkC(h, L"EDIT", W(def[i]).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IDC_CAT_E0 + i, WS_EX_CLIENTEDGE);
    }
    g_catFind = MkC(h, L"BUTTON", L"РќР°Р№С‚Рё BCD", BS_PUSHBUTTON | WS_TABSTOP, IDC_CAT_FIND);
    g_catRun = MkC(h, L"BUTTON", L"Р’С‹РїРѕР»РЅРёС‚СЊвЂ¦", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_CAT_RUN);
    g_catClose = MkC(h, L"BUTTON", L"Р—Р°РєСЂС‹С‚СЊ", BS_PUSHBUTTON | WS_TABSTOP, IDC_CAT_CLOSE);
    SendMessageW(g_catDesc, WM_SETFONT, (WPARAM)g_mono, TRUE);
    CatLayout(h);
    CatFillList();
    CatUpdate();
    ShowWindow(h, SW_SHOW);
}

HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

void CreateControls() {
    const Env& e = GetEnv();
    g_banner = Mk(L"STATIC", L"", SS_LEFT, IDC_BANNER);
    std::string b = fmt("РЎСЂРµРґР°: %s, %s%s  |  РђРґРјРёРЅРёСЃС‚СЂР°С‚РѕСЂ: %s  |  Р’РµСЂСЃРёСЏ %s", e.osName.c_str(), e.arch.c_str(), e.isWinRE ? "  [WinRE]" : (e.isWinPE ? "  [WinPE]" : ""), e.isAdmin ? "РґР°" : "РќР•Рў", kAppVersion);
    SetWindowTextW(g_banner, W(b).c_str());
    ShowWindow(g_banner, SW_HIDE);
    g_hdrSub = W(e.osName);
    g_chips.push_back({W(e.arch), 0});
    if (e.isWinRE) g_chips.push_back({L"WinRE", 0}); else if (e.isWinPE) g_chips.push_back({L"WinPE", 0});
    g_chips.push_back({e.isAdmin ? W("РђРґРјРёРЅРёСЃС‚СЂР°С‚РѕСЂ: РґР°") : W("Р‘Р•Р— РїСЂР°РІ Р°РґРјРёРЅРёСЃС‚СЂР°С‚РѕСЂР°"), e.isAdmin ? 1 : 2});
    g_chips.push_back({W(std::string("v") + kAppVersion), 0});
    g_langBtn = Mk(L"BUTTON", CurrentLang() == Lang::RU ? L"English" : L"Р СѓСЃСЃРєРёР№", BS_OWNERDRAW | WS_TABSTOP, IDC_LANG);
    g_exitBtn = Mk(L"BUTTON", L"Р’С‹С…РѕРґ", BS_OWNERDRAW | WS_TABSTOP, IDC_EXIT);
    g_lbl[0] = Mk(L"STATIC", L"Р¦РµР»РµРІР°СЏ РћРЎ:", SS_LEFT, IDC_L1);
    g_target = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_TARGET);
    g_refreshBtn = Mk(L"BUTTON", L"РћР±РЅРѕРІРёС‚СЊ", BS_OWNERDRAW | WS_TABSTOP, IDC_REFRESH);
    g_lbl[1] = Mk(L"STATIC", L"РџРµСЂРёРѕРґ, РґРЅРµР№:", SS_LEFT, IDC_L2);
    g_hint = Mk(L"STATIC", L"", SS_LEFT, IDC_L6);
    g_days = Mk(L"EDIT", L"3", ES_NUMBER | WS_TABSTOP, IDC_DAYS, WS_EX_CLIENTEDGE);
    g_ext = Mk(L"BUTTON", L"Р Р°СЃС€РёСЂРµРЅРЅС‹Р№ СЂРµР¶РёРј (ScanHealth, chkdsk /scan)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_EXT);
    g_force = Mk(L"BUTTON", L"РўРѕС‡РєР° РІРѕСЃСЃС‚Р°РЅРѕРІР»РµРЅРёСЏ: РёРіРЅРѕСЂРёСЂРѕРІР°С‚СЊ Р»РёРјРёС‚ 24 С‡", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FORCE);
    g_lbl[2] = Mk(L"STATIC", L"РҐРѕСЃС‚ РґР»СЏ РїСЂРѕРІРµСЂРєРё:", SS_LEFT, IDC_L3);
    g_host = Mk(L"EDIT", L"www.microsoft.com", ES_AUTOHSCROLL | WS_TABSTOP, IDC_HOST, WS_EX_CLIENTEDGE);
    g_lbl[3] = Mk(L"STATIC", L"РџРѕСЂС‚:", SS_LEFT, IDC_L4);
    g_port = Mk(L"EDIT", L"443", ES_NUMBER | WS_TABSTOP, IDC_PORT, WS_EX_CLIENTEDGE);
    g_lbl[4] = Mk(L"STATIC", L"РСЃС‚РѕС‡РЅРёРє DISM (/Source):", SS_LEFT, IDC_L5);
    g_source = Mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SOURCE, WS_EX_CLIENTEDGE);
    g_tab = Mk(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED | TCS_FIXEDWIDTH | TCS_MULTILINE, IDC_TAB);
    for (int i = 0; i < (int)(sizeof kTabs / sizeof kTabs[0]); ++i) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = (LPWSTR)kTabs[i];
        TabCtrl_InsertItem(g_tab, i, &it);
    }
    for (int i = 0; i < kActionCount; ++i) {
        g_btn[i] = Mk(L"BUTTON", kActions[i].text, BS_OWNERDRAW | WS_TABSTOP, IDC_ACTION0 + i);
        SetParent(g_btn[i], g_wnd);
        SetWindowPos(g_btn[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    {   // RichEdit (msftedit.dll) gives per-level colours; fall back to a plain EDIT where it is missing (some WinPE images)
        g_logRich = LoadLibraryW(L"msftedit.dll") != nullptr;
        g_log = Mk(g_logRich ? L"RICHEDIT50W" : L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL, IDC_LOG, WS_EX_CLIENTEDGE);
        if (!g_log && g_logRich) {
            g_logRich = false;
            g_log = Mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL, IDC_LOG, WS_EX_CLIENTEDGE);
        }
        if (g_logRich) SendMessageW(g_log, EM_SETBKGNDCOLOR, 0, (LPARAM)theme::LogBg());
    }
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_mono, TRUE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
    g_cancelBtn = Mk(L"BUTTON", L"РћС‚РјРµРЅР°", BS_OWNERDRAW | WS_TABSTOP, IDC_CANCEL);
    Mk(L"BUTTON", L"РћС‚РєСЂС‹С‚СЊ РїР°РїРєСѓ РѕС‚С‡С‘С‚РѕРІ", BS_OWNERDRAW | WS_TABSTOP, IDC_OPENREP);
    Mk(L"BUTTON", L"РћС‚РєСЂС‹С‚СЊ Р»РѕРі-С„Р°Р№Р»", BS_OWNERDRAW | WS_TABSTOP, IDC_OPENLOG);
    Mk(L"BUTTON", L"РЎРѕС…СЂР°РЅРёС‚СЊ Р¶СѓСЂРЅР°Р»вЂ¦", BS_OWNERDRAW | WS_TABSTOP, IDC_SAVELOG);
    Mk(L"BUTTON", L"РљРѕРїРёСЂРѕРІР°С‚СЊ Р¶СѓСЂРЅР°Р»", BS_OWNERDRAW | WS_TABSTOP, IDC_COPYLOG);
    Mk(L"BUTTON", L"РћС‡РёСЃС‚РёС‚СЊ", BS_OWNERDRAW | WS_TABSTOP, IDC_CLEAR);
    g_prog = Mk(PROGRESS_CLASSW, L"", PBS_MARQUEE, IDC_PROG);
    g_status = Mk(L"STATIC", L"Р“РѕС‚РѕРІРѕ", SS_LEFT, IDC_STATUS);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)theme::Semi(), TRUE);
    SendMessageW(g_target, WM_SETFONT, (WPARAM)theme::Big(), TRUE);
    SendMessageW(g_target, CB_SETITEMHEIGHT, (WPARAM)-1, S(22));
    SendMessageW(g_lbl[0], WM_SETFONT, (WPARAM)theme::Big(), TRUE);
    SendMessageW(g_hint, WM_SETFONT, (WPARAM)theme::Small(), TRUE);
    EnableWindow(g_cancelBtn, FALSE);
    ShowWindow(g_prog, SW_HIDE);
    for (HWND hb : {g_langBtn, g_exitBtn, g_refreshBtn, g_cancelBtn, GetDlgItem(g_wnd, IDC_OPENREP), GetDlgItem(g_wnd, IDC_OPENLOG), GetDlgItem(g_wnd, IDC_SAVELOG), GetDlgItem(g_wnd, IDC_COPYLOG), GetDlgItem(g_wnd, IDC_CLEAR)}) theme::AttachHover(hb);
    for (int i = 0; i < kActionCount; ++i) theme::AttachHover(g_btn[i]);
    FillTargets();
    ShowTabButtons();
    SetWindowSubclass(g_log, LogSubProc, 7, 0);
    CreateTooltips();
}

// ---- right-click menus -----------------------------------------------------------------------------------
std::wstring Tm(const wchar_t* ru) { return TrW(ru); }

std::wstring StripAmp(std::wstring t) {
    t.erase(std::remove(t.begin(), t.end(), L'&'), t.end());
    return t;
}

void PutClipboard(const std::wstring& txt) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (txt.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), txt.c_str(), (txt.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

POINT MenuPoint(HWND src, LPARAM lp) {
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) {   // keyboard (Shift+F10 / menu key): below the control
        RECT r;
        GetWindowRect(src, &r);
        pt = {r.left + S(12), r.top + S(12)};
    }
    return pt;
}

enum { CM_RUN = 1, CM_ABOUT, CM_COPYNAME, CM_COPYDESC, CM_COPYSEL, CM_SELALL, CM_COPYALL, CM_SAVE, CM_OPENLOG, CM_CLEAR, CM_OPENREP,
       CM_REFRESH, CM_LANG, CM_EXIT, CM_TAB0 = 100 };

// Menu for a command button: run it, read what it does, copy its name or description.
void CommandMenu(int idx, POINT pt) {
    const Action& a = kActions[idx];
    std::wstring name = W(U(std::wstring(a.text)));
    while (!name.empty() && (name.back() == L'\u2026' || name.back() == L'.')) name.pop_back();
    std::wstring desc = W(U(std::wstring(DescOf(a.name))));
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_running ? MF_GRAYED : 0), CM_RUN, (Tm(L"Р—Р°РїСѓСЃС‚РёС‚СЊ: ") + name).c_str());
    AppendMenuW(m, MF_STRING | (desc.empty() ? MF_GRAYED : 0), CM_ABOUT, Tm(L"Р§С‚Рѕ РґРµР»Р°РµС‚ СЌС‚Р° РєРѕРјР°РЅРґР°вЂ¦").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CM_COPYNAME, Tm(L"РљРѕРїРёСЂРѕРІР°С‚СЊ РЅР°Р·РІР°РЅРёРµ").c_str());
    AppendMenuW(m, MF_STRING | (desc.empty() ? MF_GRAYED : 0), CM_COPYDESC, Tm(L"РљРѕРїРёСЂРѕРІР°С‚СЊ РѕРїРёСЃР°РЅРёРµ").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CM_OPENREP, Tm(L"РћС‚РєСЂС‹С‚СЊ РїР°РїРєСѓ РѕС‚С‡С‘С‚РѕРІ").c_str());
    SetForegroundWindow(g_wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    switch (c) {
        case CM_RUN: if (!g_running) StartAction(idx); break;
        case CM_ABOUT: MessageBoxW(g_wnd, desc.c_str(), name.c_str(), MB_OK | MB_ICONINFORMATION); break;
        case CM_COPYNAME: PutClipboard(name); break;
        case CM_COPYDESC: PutClipboard(desc); break;
        case CM_OPENREP: SendMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(IDC_OPENREP, 0), 0); break;
    }
}

// Menu for the log pane.
void LogMenu(POINT pt) {
    DWORD s0 = 0, s1 = 0;
    SendMessageW(g_log, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    bool hasSel = s1 > s0;
    bool empty = GetWindowTextLengthW(g_log) == 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (hasSel ? 0 : MF_GRAYED), CM_COPYSEL, Tm(L"РљРѕРїРёСЂРѕРІР°С‚СЊ РІС‹РґРµР»РµРЅРЅРѕРµ").c_str());
    AppendMenuW(m, MF_STRING | (empty ? MF_GRAYED : 0), CM_SELALL, Tm(L"Р’С‹РґРµР»РёС‚СЊ РІСЃС‘").c_str());
    AppendMenuW(m, MF_STRING | (empty ? MF_GRAYED : 0), CM_COPYALL, Tm(L"РљРѕРїРёСЂРѕРІР°С‚СЊ РІРµСЃСЊ Р¶СѓСЂРЅР°Р»").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (empty ? MF_GRAYED : 0), CM_SAVE, Tm(L"РЎРѕС…СЂР°РЅРёС‚СЊ Р¶СѓСЂРЅР°Р»вЂ¦").c_str());
    AppendMenuW(m, MF_STRING, CM_OPENLOG, Tm(L"РћС‚РєСЂС‹С‚СЊ С„Р°Р№Р» Р¶СѓСЂРЅР°Р»Р°").c_str());
    AppendMenuW(m, MF_STRING, CM_OPENREP, Tm(L"РћС‚РєСЂС‹С‚СЊ РїР°РїРєСѓ РѕС‚С‡С‘С‚РѕРІ").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (empty ? MF_GRAYED : 0), CM_CLEAR, Tm(L"РћС‡РёСЃС‚РёС‚СЊ").c_str());
    SetForegroundWindow(g_wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    switch (c) {
        case CM_COPYSEL: SendMessageW(g_log, WM_COPY, 0, 0); break;
        case CM_SELALL: SendMessageW(g_log, EM_SETSEL, 0, -1); break;
        case CM_COPYALL: CopyLog(); break;
        case CM_SAVE: SaveLogAs(); break;
        case CM_OPENLOG: SendMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(IDC_OPENLOG, 0), 0); break;
        case CM_OPENREP: SendMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(IDC_OPENREP, 0), 0); break;
        case CM_CLEAR: SetWindowTextW(g_log, L""); break;
    }
}

// Tab strip menu: jump to a tab.
void GeneralMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    int cur = TabCtrl_GetCurSel(g_tab);
    for (int i = 0; i < (int)(sizeof kTabs / sizeof kTabs[0]); ++i)
        AppendMenuW(m, MF_STRING | (i == cur ? MF_CHECKED : 0), CM_TAB0 + i, TrW(kTabs[i]).c_str());
    SetForegroundWindow(g_wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (c >= CM_TAB0 && c < CM_TAB0 + 16) { TabCtrl_SetCurSel(g_tab, c - CM_TAB0); ShowTabButtons(); }
}

void OnContextMenu(HWND src, LPARAM lp) {
    POINT pt = MenuPoint(src, lp);
    if (src == g_log) { LogMenu(pt); return; }
    int id = GetDlgCtrlID(src);
    if (id >= IDC_ACTION0 && id < IDC_ACTION0 + kActionCount) { CommandMenu(id - IDC_ACTION0, pt); return; }
    if (src == g_tab) GeneralMenu(pt);   // tab strip: go to a tab; other places have no menu of their own
}

// the log (RichEdit) has its own built-in menu; route it to ours so Save / Clear / Open are there too
LRESULT CALLBACK LogSubProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_CONTEXTMENU) { OnContextMenu(h, lp); return 0; }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, LogSubProc, 7);
    return DefSubclassProc(h, m, wp, lp);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: g_wnd = h; return 0;
        case WM_CONTEXTMENU: OnContextMenu((HWND)wp, lp); return 0;
        case WM_SIZE: if (g_tab) Layout(); InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            RECT hr = rc;
            hr.bottom = S(72);
            theme::PaintHeader(dc, hr, g_icon, W(kAppName), g_hdrSub, g_chips);
            theme::PaintLegend(dc, S(10), hr.bottom + S(7), rc.right - S(20));
            theme::PaintTargetPanel(dc, g_targetFrame);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* di = (DRAWITEMSTRUCT*)lp;
            if (di->CtlType == ODT_TAB) {
                wchar_t buf[128] = {0};
                TCITEMW ti{};
                ti.mask = TCIF_TEXT;
                ti.pszText = buf;
                ti.cchTextMax = 127;
                TabCtrl_GetItem(g_tab, di->itemID, &ti);
                theme::DrawTab(di, buf, TabCat((int)di->itemID), (di->itemState & ODS_SELECTED) != 0);
                return TRUE;
            }
            if (di->CtlType != ODT_BUTTON) break;
            int id = (int)di->CtlID;
            theme::Cat c = theme::Cat::Info;
            if (id >= IDC_ACTION0 && id < IDC_ACTION0 + kActionCount) c = CatOfAction(kActions[id - IDC_ACTION0].name);
            else if (id == IDC_EXIT) c = theme::Cat::Exit;
            else if (id == IDC_LANG) c = theme::Cat::Header;
            else if (id == IDC_CANCEL) c = theme::Cat::Repair;
            else if (id == IDC_REFRESH) c = theme::Cat::Diag;
            theme::DrawButton(di, c, theme::IsHover(di->hwndItem));
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            if ((HWND)lp == g_log) {
                SetTextColor(dc, theme::LogText());
                SetBkColor(dc, theme::LogBg());
                return (LRESULT)theme::LogBrush();
            }
            if ((HWND)lp == g_lbl[0]) {   // "Target OS" label sits on the highlighted panel
                SetBkColor(dc, theme::TintColor());
                SetTextColor(dc, RGB(16, 78, 168));
                return (LRESULT)theme::TintBrush();
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, (HWND)lp == g_hint ? RGB(88, 100, 122) : theme::Text());
            return (LRESULT)theme::BgBrush();
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = S(920);
            mm->ptMinTrackSize.y = S(720);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->code == TTN_GETDISPINFOW && nh->hwndFrom == g_tip) { TooltipNeedText((NMTTDISPINFOW*)lp); return 0; }
            if (nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) ShowTabButtons();
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id >= IDC_ACTION0 && id < IDC_ACTION0 + kActionCount) { if (!g_running) StartAction(id - IDC_ACTION0); }
            else if (id == IDC_CANCEL) { g_cancel = true; Logger::I().log(Lvl::Warn, "Р—Р°РїСЂРѕС€РµРЅР° РѕС‚РјРµРЅР°вЂ¦"); }
            else if (id == IDC_REFRESH) { FillTargets(); Logger::I().log(Lvl::Info, fmt("РќР°Р№РґРµРЅРѕ С†РµР»РµРІС‹С… РћРЎ: %zu", g_targets.size())); }
            else if (id == IDC_CLEAR) SetWindowTextW(g_log, L"");
            else if (id == IDC_LANG) ChangeLanguage();
            else if (id == IDC_EXIT) SendMessageW(h, WM_CLOSE, 0, 0);
            else if (id == IDC_OPENLOG) OpenPath(Logger::I().path());
            else if (id == IDC_OPENREP) OpenPath(g_lastOut.empty() ? DataDir() + "Reports" : g_lastOut);
            else if (id == IDC_SAVELOG) SaveLogAs();
            else if (id == IDC_COPYLOG) CopyLog();
            return 0;
        }
        case WM_APP_ASK: {
            struct AskData { std::wstring title, prompt, def; std::string out; bool ok; };
            auto* d = (AskData*)lp;
            std::vector<wr::ui::FormField> f(1);
            f[0].label = d->prompt;
            f[0].value = d->def;
            d->ok = wr::ui::FormDialog(h, d->title, f);
            if (d->ok) d->out = U(f[0].value);
            return 0;
        }
        case WM_APP_CONFIRM: {
            struct ConfData { std::wstring text; int level; bool yes; };
            auto* d = (ConfData*)lp;
            UINT fl = MB_YESNO | (d->level >= 2 ? MB_ICONSTOP : d->level == 1 ? MB_ICONWARNING : MB_ICONQUESTION) | (d->level > 0 ? MB_DEFBUTTON2 : 0);
            d->yes = MessageBoxW(h, d->text.c_str(), L"РџРѕРґС‚РІРµСЂР¶РґРµРЅРёРµ С€Р°РіР° СЃС†РµРЅР°СЂРёСЏ", fl) == IDYES;
            return 0;
        }
        case WM_APP_OPENWIN: {
            std::unique_ptr<std::string> n((std::string*)lp);
            if (*n == "storage") wr::OpenStorageAccess();
            else if (*n == "bcd") wr::OpenBcdManager();
            else if (*n == "autoruns") wr::OpenAutoruns();
            else if (*n == "apps") wr::OpenInstalledApps();
            else if (*n == "explorer") wr::OpenOfflineExplorer();
            else if (*n == "registry") wr::OpenRegistryEditor();
            else if (*n == "winre") wr::OpenWinReManager();
            else if (*n == "fixes") wr::OpenFixWizard();
            else if (*n == "triage") wr::OpenTriage();
            else if (*n == "playbooks") wr::OpenPlaybooks();
            else if (*n == "catalog") OpenCatalog();
            return 0;
        }
        case WM_APP_LOG: {
            std::unique_ptr<std::string> s((std::string*)lp);
            AppendLog(*s);
            return 0;
        }
        case WM_APP_STATUS: {
            std::unique_ptr<std::string> s((std::string*)lp);
            SetWindowTextW(g_status, s->empty() ? L"Р’С‹РїРѕР»РЅСЏРµС‚СЃСЏвЂ¦" : W(*s).c_str());
            return 0;
        }
        case WM_APP_DONE: {
            double sec = (double)(GetTickCount64() - g_jobStart) / 1000.0;
            SetRunning(false);
            Logger::I().log(g_cancel ? Lvl::Warn : Lvl::Ok, fmt("в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ Р—РђР’Р•Р РЁР•РќРћ Р·Р° %.1f СЃ%s. РћС‚С‡С‘С‚: %s", sec, g_cancel ? " (РѕС‚РјРµРЅРµРЅРѕ)" : "", g_lastOut.c_str()));
            SetWindowTextW(g_status, W(fmt("Р“РѕС‚РѕРІРѕ (%.1f СЃ). РћС‚С‡С‘С‚: %s", sec, g_lastOut.c_str())).c_str());
            if (!g_results.lastReportHtml.empty() && g_lastOut.find("_full") != std::string::npos && !g_cancel) OpenLastReport();
            if (g_jobName == "drvload" || g_jobName == "bl-unlock") FillTargets();
            UiJobDone();
            return 0;
        }
        case WM_CLOSE:
            if (GetEnv().winreShell && !g_running) {
                int r = MessageBoxW(h, L"Р’С‹ РІ WinRE СЃ WinRepair.\n\nР”Р° вЂ” РІС‹Р№С‚Рё РёР· WinRepair Рё РѕС‚РєСЂС‹С‚СЊ С€С‚Р°С‚РЅРѕРµ РјРµРЅСЋ WinRE.\nРќРµС‚ вЂ” РїРµСЂРµР·Р°РіСЂСѓР·РёС‚СЊ РєРѕРјРїСЊСЋС‚РµСЂ.\nРћС‚РјРµРЅР° вЂ” РѕСЃС‚Р°С‚СЊСЃСЏ РІ WinRepair.", L"Р’С‹С…РѕРґ РёР· WinRepair", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (r == IDCANCEL) return 0;
                if (r == IDNO) { RunProcess(RebootCommand(), "", nullptr, nullptr, 20); return 0; }
            }
            if (g_running && MessageBoxW(h, L"РћРїРµСЂР°С†РёСЏ РІС‹РїРѕР»РЅСЏРµС‚СЃСЏ. РџСЂРµСЂРІР°С‚СЊ Рё РІС‹Р№С‚Рё?", L"Р’С‹С…РѕРґ", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            g_cancel = true;
            {
                std::vector<BackupItem> bk = ListBackups();
                if (!bk.empty()) {
                    std::wstring msg = W(fmt("РќР° РґРёСЃРєРµ РѕСЃС‚Р°Р»РёСЃСЊ СЂРµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё СЃРѕСЃС‚РѕСЏРЅРёСЏ РїСЂРѕРІРµСЂРµРЅРЅС‹С… СЃРёСЃС‚РµРј (РєСѓСЃС‚С‹ СЂРµРµСЃС‚СЂР°, BCD).\n\n%s\nР”Р° вЂ” СѓРґР°Р»РёС‚СЊ РєРѕРїРёРё СЃРµР№С‡Р°СЃ.\nРќРµС‚ вЂ” РѕСЃС‚Р°РІРёС‚СЊ (СѓРґР°Р»РёС‚СЊ РјРѕР¶РЅРѕ РїРѕР·Р¶Рµ РєРЅРѕРїРєРѕР№ В«Р РµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРёвЂ¦В»; РїСЂРё Р·Р°РїСѓСЃРєРµ РЅР° РґСЂСѓРіРѕРј РџРљ РєРѕРїРёРё С‡СѓР¶РёС… СЃРёСЃС‚РµРј Р±СѓРґСѓС‚ РїСЂРµРґР»РѕР¶РµРЅС‹ Рє СѓРґР°Р»РµРЅРёСЋ).\nРћС‚РјРµРЅР° вЂ” РЅРµ РІС‹С…РѕРґРёС‚СЊ.", BackupsSummary(bk).c_str()));
                    int r = MessageBoxW(h, msg.c_str(), L"Р РµР·РµСЂРІРЅС‹Рµ РєРѕРїРёРё", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
                    if (r == IDCANCEL) { g_cancel = false; return 0; }
                    if (r == IDYES) Logger::I().log(Lvl::Ok, fmt("РЈРґР°Р»РµРЅРѕ СЂРµР·РµСЂРІРЅС‹С… РєРѕРїРёР№: %d", DeleteBackups(bk)));
                }
            }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

namespace wr {
Target UiCurrentTarget() { return CurrentTarget(); }
std::vector<Target> UiTargets() { return g_targets; }
bool UiRunning() { return g_running; }
void UiRefreshTargets() { FillTargets(); }
void UiSelectTarget(const Target& t) {
    for (size_t i = 0; i < g_targets.size(); ++i)
        if (g_targets[i].winDir == t.winDir) { SendMessageW(g_target, CB_SETCURSEL, (WPARAM)i, 0); return; }
}
void UiRunCatalog(int no) { QuickCmd(no); }
bool UiConfirm(const std::wstring& text, int level) {
    if (!g_wnd) return false;  // headless: risky steps are never confirmed automatically
    struct ConfData { std::wstring text; int level; bool yes; } d{text, level, false};
    SendMessageW(g_wnd, WM_APP_CONFIRM, 0, (LPARAM)&d);
    return d.yes;
}
bool UiAsk(const std::wstring& title, const std::wstring& prompt, const std::wstring& def, std::string& out) {
    if (!g_wnd) { out = U(def); return true; }
    struct AskData { std::wstring title, prompt, def; std::string out; bool ok; } d{title, prompt, def, "", false};
    SendMessageW(g_wnd, WM_APP_ASK, 0, (LPARAM)&d);
    out = d.out;
    return d.ok;
}
void UiOpenWindow(const std::string& name) {
    if (g_wnd) PostMessageW(g_wnd, WM_APP_OPENWIN, 0, (LPARAM) new std::string(name));
}
void UiRunModule(const std::string& name) {
    for (int i = 0; i < kActionCount; ++i)
        if (!strcmp(kActions[i].name, name.c_str())) { StartAction(i); return; }
}
void UiStartJob(const std::string& name, const std::string& title, std::function<void(Ctx&)> fn, const Target* target) {
    if (g_running) { MessageBoxW(g_wnd, L"Р”РѕР¶РґРёС‚РµСЃСЊ Р·Р°РІРµСЂС€РµРЅРёСЏ С‚РµРєСѓС‰РµР№ РѕРїРµСЂР°С†РёРё.", L"Р—Р°РЅСЏС‚Рѕ", MB_OK | MB_ICONINFORMATION); return; }
    JobData* d = MakeJob(name, fn);
    if (target) d->ctx.target = *target;
    RunJob(d, title);
}
}  // namespace wr

namespace {

int RunHeadless() {
    // wrapper: WinRepair.exe /full [/out:<dir>]  -> full diagnostic without UI; exit code 0 = ok, 2 = poor health
    std::string out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.rfind(L"/out:", 0) == 0) out = U(a.substr(5));
    }
    if (argv) LocalFree(argv);
    if (out.empty()) out = DataDir() + "Reports\\" + Stamp() + "_full\\";
    if (out.back() != '\\') out += '\\';
    MakeDirs(out);
    Results res;
    Ctx c;
    auto t = FindTargets();
    if (!t.empty()) c.target = t[0];
    c.outDir = out;
    c.res = &res;
    ModFull(c);
    return res.lastReportHtml.empty() ? 1 : 0;
}

IntegrityReport g_integrity;

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    InitEnvironment();
    InitLanguage();
    Logger::I().init(DataDir() + "Logs\\");
    ReloadPlaybooks();
    Logger::I().log(Lvl::Info, fmt("%s %s Р·Р°РїСѓС‰РµРЅ. РћРЎ: %s, %s%s, Р°РґРјРёРЅ=%s", kAppName, kAppVersion, GetEnv().osName.c_str(), GetEnv().arch.c_str(),
                                   GetEnv().isWinPE ? " (WinPE)" : "", GetEnv().isAdmin ? "РґР°" : "РЅРµС‚"));
    g_integrity = RunIntegrityCheck();
    Logger::I().log(g_integrity.tampered() ? Lvl::Error : Lvl::Info, IntegritySummary(g_integrity));
    for (auto& f : g_integrity.mismatched) Logger::I().log(Lvl::Error, "Р¤Р°Р№Р» РёР·РјРµРЅС‘РЅ РѕС‚РЅРѕСЃРёС‚РµР»СЊРЅРѕ MANIFEST.sha256: " + f);
    std::wstring cl = GetCommandLineW();
    if (g_integrity.tampered() && cl.find(L"/full") == std::wstring::npos)
        MessageBoxW(nullptr, W("РћР±РЅР°СЂСѓР¶РµРЅРѕ РЅР°СЂСѓС€РµРЅРёРµ С†РµР»РѕСЃС‚РЅРѕСЃС‚Рё РїСЂРѕРіСЂР°РјРјС‹ (СЃРј. Р¶СѓСЂРЅР°Р»).\nР¤Р°Р№Р»С‹ РјРѕРіР»Рё Р±С‹С‚СЊ РёР·РјРµРЅРµРЅС‹ РёР»Рё РїРѕРІСЂРµР¶РґРµРЅС‹. РСЃРїРѕР»СЊР·СѓР№С‚Рµ РїСЂРѕРіСЂР°РјРјСѓ С‚РѕР»СЊРєРѕ РµСЃР»Рё РґРѕРІРµСЂСЏРµС‚Рµ РёСЃС‚РѕС‡РЅРёРєСѓ.").c_str(),
                    W("WinRepair вЂ” С†РµР»РѕСЃС‚РЅРѕСЃС‚СЊ").c_str(), MB_ICONSTOP | MB_OK);
    if (cl.find(L"/full") != std::wstring::npos) return RunHeadless();

    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    theme::Init(g_dpi);
    g_font = theme::Body();
    g_mono = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WinRepairMain";
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_icon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48), S(48), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, (W(kAppName) + L" вЂ” РґРёР°РіРЅРѕСЃС‚РёРєР° Рё СЂРµРјРѕРЅС‚ Windows").c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(1120), S(840), nullptr, nullptr, hi, nullptr);
    g_wnd = w;
    g_ui.main = w; g_ui.font = g_font; g_ui.mono = g_mono; g_ui.dpi = g_dpi;
    CreateControls();
    Logger::I().attachUi(w, WM_APP_LOG, WM_APP_STATUS);
    Layout();
    ShowWindow(w, show);
    UpdateWindow(w);
    CheckForeignBackups();
    AppendLog("Р¤Р°Р№Р» Р¶СѓСЂРЅР°Р»Р°: " + Logger::I().path());
    if (!GetEnv().isAdmin) Logger::I().log(Lvl::Error, "РџСЂРѕРіСЂР°РјРјР° Р·Р°РїСѓС‰РµРЅР° Р‘Р•Р— РїСЂР°РІ Р°РґРјРёРЅРёСЃС‚СЂР°С‚РѕСЂР° вЂ” Р±РѕР»СЊС€РёРЅСЃС‚РІРѕ С„СѓРЅРєС†РёР№ Р±СѓРґРµС‚ РЅРµРґРѕСЃС‚СѓРїРЅРѕ.");
    if (GetEnv().isWinPE) Logger::I().log(Lvl::Info, "РћР±РЅР°СЂСѓР¶РµРЅР° WinPE: РІС‹Р±РµСЂРёС‚Рµ СѓСЃС‚Р°РЅРѕРІР»РµРЅРЅСѓСЋ Windows РІ СЃРїРёСЃРєРµ В«Р¦РµР»РµРІР°СЏ РћРЎВ» (СЂРµР¶РёРј РѕС„Р»Р°Р№РЅ).");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_cat && IsDialogMessageW(g_cat, &msg)) continue;
        if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return (int)msg.wParam;
}
