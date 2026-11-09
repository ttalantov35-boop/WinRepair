@echo off
rem Static analysis build (MSVC /analyze) - no exe is produced. Run from a Native Tools prompt. Warnings go to build\analyze.log
setlocal EnableDelayedExpansion
if not exist build mkdir build
set SRC=
for /r src %%F in (*.cpp) do set SRC=!SRC! "%%F"
cl /nologo /std:c++17 /analyze /analyze:log build\analyze.sarif /W3 /EHsc /utf-8 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /c /Fo:build\ !SRC! > build\analyze.log 2>&1
findstr /c:"warning C6" /c:"warning C28" build\analyze.log
echo Full log: build\analyze.log
exit /b 0
