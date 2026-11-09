@echo off
rem Starts the right WinRepair build (x64 / x86). Works from any drive letter (USB letters differ in WinPE/WinRE).
setlocal
set ARCH=x86
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set ARCH=x64
if /i "%PROCESSOR_ARCHITEW6432%"=="AMD64" set ARCH=x64
if exist "%~dp0WinRepair_%ARCH%.exe" (
  start "" "%~dp0WinRepair_%ARCH%.exe"
  goto :eof
)
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
  if exist "%%D:\WinRepair\WinRepair_%ARCH%.exe" (
    start "" "%%D:\WinRepair\WinRepair_%ARCH%.exe"
    goto :eof
  )
)
echo WinRepair_%ARCH%.exe was not found. Copy the WinRepair folder to the root of a drive.
pause
