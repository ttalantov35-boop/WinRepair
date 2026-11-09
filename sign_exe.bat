@echo off
rem Authenticode-signs an exe (SHA-256 + RFC 3161 timestamp).
rem Usage: sign_exe.bat <file.exe>   with environment: WR_PFX=<cert.pfx> [WR_PFX_PASS=<password>] [WR_TSA=<timestamp url>]
rem Or use a certificate from the store: set WR_CERT_SHA1=<thumbprint> instead of WR_PFX.
setlocal
if "%~1"=="" (echo Usage: sign_exe.bat file.exe & exit /b 1)
if "%WR_TSA%"=="" set WR_TSA=http://timestamp.digicert.com
where signtool >nul 2>&1 || (echo signtool not found - run from a VS Native Tools prompt / install the Windows SDK & exit /b 1)
if not "%WR_PFX%"=="" (
  signtool sign /fd SHA256 /tr %WR_TSA% /td SHA256 /f "%WR_PFX%" /p "%WR_PFX_PASS%" "%~1" || exit /b 1
) else if not "%WR_CERT_SHA1%"=="" (
  signtool sign /fd SHA256 /tr %WR_TSA% /td SHA256 /sha1 %WR_CERT_SHA1% "%~1" || exit /b 1
) else (
  echo Set WR_PFX or WR_CERT_SHA1 first. & exit /b 1
)
signtool verify /pa "%~1" || exit /b 1
echo Signed OK: %~1
exit /b 0
