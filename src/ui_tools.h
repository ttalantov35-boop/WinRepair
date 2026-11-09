// Service-engineer tool windows: offline registry editor, BCD manager, autoruns/msconfig, repair wizard.
#pragma once
#include "win/app.h"

namespace wr {

struct UiHost { HWND main = nullptr; HFONT font = nullptr, mono = nullptr; int dpi = 96; };
extern UiHost g_ui;

// implemented by main.cpp
Target UiCurrentTarget();
std::vector<Target> UiTargets();
bool UiRunning();
void UiStartJob(const std::string& name, const std::string& title, std::function<void(Ctx&)> fn, const Target* target = nullptr);

// implemented by ui_tools.cpp
void OpenRegistryEditor();
void OpenBcdManager();
void OpenAutoruns();
void OpenFixWizard();
void OpenWinReManager();
void OpenStorageAccess();
void UiRefreshTargets();  // implemented by main.cpp
void UiSelectTarget(const Target& t);             // select the target in the main window (implemented by main.cpp)
void UiRunCatalog(int no);                         // confirm + run a catalogue command (main.cpp)
void UiRunModule(const std::string& actionName);   // run a main-window action by its name (main.cpp)
void OpenTriage();
void OpenPlaybooks();
void UiRunPlaybook(const std::string& id);   // confirm the plan and run (ui_playbooks.cpp)
// thread-safe hooks used by playbook steps (implemented by main.cpp; they marshal to the UI thread)
bool UiConfirm(const std::wstring& text, int level);   // level 0 info, 1 warning, 2 danger
bool UiAsk(const std::wstring& title, const std::wstring& prompt, const std::wstring& def, std::string& out);
void UiOpenWindow(const std::string& name);
void OpenSaveData();
void UiJobDone();   // refresh open windows
void UiShutdown();  // unload hives, unmount temporary volumes

}  // namespace wr
