@echo off
rem Run from "x64 Native Tools Command Prompt for VS" (or x86 for a 32-bit build).
rem Result: build\WinRepair.exe - static CRT (/MT), no runtime redistributables.
setlocal
if not exist build mkdir build
rc /nologo /fo build\app.res res\app.rc || goto :err
cl /nologo /std:c++17 /O1 /Os /GL /Gy /Gw /Zc:inline /MT /EHsc /utf-8 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DNDEBUG ^
   src\main.cpp src\ui_common.cpp src\ui_registry.cpp src\ui_bcd.cpp src\ui_autoruns.cpp src\ui_fixes.cpp src\win\common.cpp src\win\lang.cpp src\win\integrity.cpp src\win\proc.cpp src\win\env.cpp src\win\app.cpp src\win\offreg.cpp src\modules\storage.cpp src\modules\system.cpp src\modules\events.cpp src\modules\dump.cpp src\modules\os.cpp src\modules\net.cpp src\modules\full.cpp src\modules\catalog.cpp src\modules\native_ps.cpp src\modules\bcd.cpp src\modules\autoruns.cpp src\modules\fixes.cpp src\modules\winre.cpp src\ui_winre.cpp src\ui_storage.cpp src\modules\access.cpp src\modules\triage.cpp src\ui_triage.cpp src\modules\playbook.cpp src\ui_playbooks.cpp build\app.res ^
   /Fo:build\ /Fe:build\WinRepair.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF /LTCG /INCREMENTAL:NO /DEBUG:NONE || goto :err
echo.
echo OK: build\WinRepair.exe
exit /b 0
:err
echo BUILD FAILED
exit /b 1
