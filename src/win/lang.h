// UI language (RU / EN). Source strings are Russian; English is produced at the output boundary from lang/en.txt (embedded + optional Lang\\en.txt).
#pragma once
#include <string>

namespace wr {

enum class Lang { RU, EN };
Lang CurrentLang();
void InitLanguage();            // command line /lang:xx > WinRepair.ini (next to exe) > Data\\settings.ini > system UI language
void SetLanguage(Lang l);       // persists the choice
std::string Tr(const std::string& ru);
std::wstring TrW(const std::wstring& ru);
const wchar_t* TrP(const wchar_t* ru);   // stable pointer (for literals passed to Win32)

}  // namespace wr
