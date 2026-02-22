@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%install.ps1"
if not exist "%PS_SCRIPT%" set "PS_SCRIPT=%SCRIPT_DIR%install-package-windows.ps1"

if not exist "%PS_SCRIPT%" (
  echo Could not find install PowerShell script next to this script.
  exit /b 1
)

powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %*
set "EXIT_CODE=%ERRORLEVEL%"
exit /b %EXIT_CODE%
