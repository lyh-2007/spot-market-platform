@echo off
rem Spot Market Platform - web backend launcher
rem Prefer system Node.js; fall back to the runtime bundled with Kimi Desktop
chcp 65001 >nul
cd /d %~dp0
where node >nul 2>nul
if %errorlevel%==0 (
  node server.js
) else if exist "C:\Users\gs_ly\AppData\Local\Programs\kimi-desktop\resources\resources\runtime\node.exe" (
  "C:\Users\gs_ly\AppData\Local\Programs\kimi-desktop\resources\resources\runtime\node.exe" server.js
) else (
  echo.
  echo [ERROR] Node.js not found.
  echo Please install Node.js LTS from https://nodejs.org/ and run this script again.
  echo.
)
pause
