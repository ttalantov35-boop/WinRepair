@echo off
rem Builds WinRepair and assembles the USB / WinPE package in dist\WinRepair
rem Usage: make_package.bat [x64|x86|arm64]   (set SIGN=1 and WR_PFX/WR_CERT_SHA1 to sign - see sign_exe.bat)
rem   run it from the matching "Native Tools Command Prompt" (x64, x86 or ARM64); run it twice to get both builds into the same package.
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
call build_msvc.bat || goto :err
set OUT=dist\WinRepair
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\Playbooks" mkdir "%OUT%\Playbooks"
if not exist "%OUT%\Tools" mkdir "%OUT%\Tools"
copy /y build\WinRepair.exe "%OUT%\WinRepair_%ARCH%.exe" >nul || goto :err
if "%SIGN%"=="1" call sign_exe.bat "%OUT%\WinRepair_%ARCH%.exe" || goto :err
xcopy /y /q Playbooks\*.wrp "%OUT%\Playbooks\" >nul
copy /y docs\PLAYBOOKS.md "%OUT%\Playbooks\README_PLAYBOOKS.md" >nul
copy /y package\Run-WinRepair.cmd "%OUT%\" >nul
copy /y package\README_RU.txt "%OUT%\" >nul
copy /y package\README_EN.txt "%OUT%\" >nul
if not exist "%OUT%\Lang" mkdir "%OUT%\Lang"
copy /y lang\en.txt "%OUT%\Lang\en.txt" >nul
copy /y package\Tools_README.txt "%OUT%\Tools\README.txt" >nul
for %%D in (LICENSE.txt PRIVACY.md SECURITY.md THIRD_PARTY_NOTICES.md CHANGELOG.md) do copy /y %%D "%OUT%\" >nul
copy /y package\winpeshl.ini.example "%OUT%\" >nul
copy /y package\startnet.cmd.example "%OUT%\" >nul
rem MANIFEST.sha256 is checked by the program at startup (self-integrity). Built AFTER signing, over everything shipped.
if exist "%OUT%\MANIFEST.sha256" del "%OUT%\MANIFEST.sha256"
pushd "%OUT%"
for /r %%F in (*) do (
  if /i not "%%~nxF"=="MANIFEST.sha256" (
    call :hashone "%%F"
  )
)
popd
goto :sizes
:hashone
setlocal EnableDelayedExpansion
set "P=%~1"
set "R=!P:%CD%\=!"
set "H="
for /f "skip=1 delims=" %%L in ('certutil -hashfile "%~1" SHA256') do if not defined H set "H=%%L"
set "H=!H: =!"
echo !H! *!R!>>"%CD%\MANIFEST.sha256"
endlocal
exit /b 0
:sizes
for %%F in ("%OUT%\WinRepair_%ARCH%.exe") do echo.& echo Size of %%~nxF: %%~zF bytes
echo.
echo Package ready: %OUT%   (copy the whole folder to the USB drive root as \WinRepair\)
exit /b 0
:err
echo PACKAGE FAILED
exit /b 1
