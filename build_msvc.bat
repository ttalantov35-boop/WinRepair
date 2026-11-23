@echo off
rem Run from "x64 Native Tools Command Prompt for VS" (or x86 for a 32-bit build).
rem Result: build\WinRepair.exe - static CRT (/MT), no runtime redistributables.
rem "build_msvc.bat edit" - author build: help can be edited from Help\help.ru.txt next to the exe (do not publish it).
setlocal
set WRDEF=
rem Baseline CPU: SSE2 only. Without this the newer MSVC (14.52 on GitHub runners) emitted AVX (vpxor) in application code
rem and the exe crashed with 0xC000001D on CPUs without AVX (e.g. Core 2). ARM64 does not take /arch:SSE2.
set WRARCH=
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" set WRARCH=/arch:SSE2
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" set WRARCH=/arch:SSE2
if /i "%~1"=="edit" set WRDEF=/DWR_HELP_EDIT
if /i "%~1"=="edit" echo AUTHOR BUILD: help editing enabled (WR_HELP_EDIT)
if not exist build mkdir build
rc /nologo /fo build\app.res res\app.rc || goto :err
cl /nologo /std:c++17 /O1 /Os /GL /Gy /Gw /Zc:inline /MT /EHsc /utf-8 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DNDEBUG %WRARCH% %WRDEF% ^
   src\main.cpp src\ui_common.cpp src\ui_registry.cpp src\ui_bcd.cpp src\ui_autoruns.cpp src\ui_apps.cpp src\ui_explorer.cpp src\modules\apps.cpp src\ui_fixes.cpp src\win\common.cpp src\win\lang.cpp src\win\integrity.cpp src\win\backups.cpp src\ui_theme.cpp src\win\proc.cpp src\win\env.cpp src\win\app.cpp src\win\offreg.cpp src\modules\storage.cpp src\modules\system.cpp src\modules\events.cpp src\modules\dump.cpp src\modules\os.cpp src\modules\net.cpp src\modules\full.cpp src\modules\catalog.cpp src\modules\native_ps.cpp src\modules\bcd.cpp src\modules\autoruns.cpp src\modules\fixes.cpp src\modules\winre.cpp src\ui_winre.cpp src\ui_storage.cpp src\modules\access.cpp src\modules\triage.cpp src\ui_triage.cpp src\modules\playbook.cpp src\modules\undo.cpp src\ui_playbooks.cpp src\ui_help.cpp src\ui_bsod.cpp src\ui_password.cpp src\modules\sampwd.cpp src\ui_partition.cpp src\modules\partition.cpp build\app.res ^
   /Fo:build\ /Fe:build\WinRepair.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF /LTCG /INCREMENTAL:NO /DEBUG:NONE || goto :err
echo.
echo OK: build\WinRepair.exe
exit /b 0
:err
echo BUILD FAILED
exit /b 1
