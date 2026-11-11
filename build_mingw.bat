@echo off
rem MinGW-w64 (MSYS2 ucrt64/mingw64/mingw32). Fully static executable.
setlocal
if not exist build mkdir build
windres -I res -i res\app.rc -O coff -o build\app.res.o || goto :err
g++ -std=c++17 -Os -ffunction-sections -fdata-sections -DNDEBUG -DUNICODE -D_UNICODE -finput-charset=UTF-8 -fexec-charset=UTF-8 -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s ^
  -municode -mwindows -o build\WinRepair.exe ^
  src\main.cpp src\ui_common.cpp src\ui_registry.cpp src\ui_bcd.cpp src\ui_autoruns.cpp src\ui_apps.cpp src\ui_explorer.cpp src\modules\apps.cpp src\ui_fixes.cpp src\win\common.cpp src\win\lang.cpp src\win\integrity.cpp src\win\backups.cpp src\ui_theme.cpp src\win\proc.cpp src\win\env.cpp src\win\app.cpp src\win\offreg.cpp src\modules\storage.cpp src\modules\system.cpp src\modules\events.cpp src\modules\dump.cpp src\modules\os.cpp src\modules\net.cpp src\modules\full.cpp src\modules\catalog.cpp src\modules\native_ps.cpp src\modules\bcd.cpp src\modules\autoruns.cpp src\modules\fixes.cpp src\modules\winre.cpp src\ui_winre.cpp src\ui_storage.cpp src\modules\access.cpp src\modules\triage.cpp src\ui_triage.cpp src\modules\playbook.cpp src\ui_playbooks.cpp build\app.res.o ^
  -lcomctl32 -lcomdlg32 -lsetupapi -lcfgmgr32 -lversion -liphlpapi -lws2_32 -lshell32 -ladvapi32 -lgdi32 -luser32 || goto :err
echo OK: build\WinRepair.exe
exit /b 0
:err
echo BUILD FAILED
exit /b 1
