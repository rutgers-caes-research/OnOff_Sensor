@echo off
setlocal
title OnOff Sensor Installer
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
set "result=%errorlevel%"
echo.
if not "%result%"=="0" echo Installation did not complete.
pause
exit /b %result%
