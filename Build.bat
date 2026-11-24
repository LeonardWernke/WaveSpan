@echo off
title WaveSpan
rem Double-click to open WaveSpan. Options still run the terminal builder.
setlocal
cd /d "%~dp0"
set "PY="
for %%V in (3.14 3.13 3.12 3) do (
  if not defined PY (
    py -%%V -c "import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)" >nul 2>nul && set "PY=py -%%V"
  )
)
if not defined PY (
  where python >nul 2>nul && (
    python -c "import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)" >nul 2>nul && set "PY=python"
  )
)
if not defined PY (
  echo Python 3.12 or newer is needed: https://www.python.org/downloads/
  echo Tick "Add python.exe to PATH" during setup, then run Build.bat again.
  pause
  exit /b 1
)
if "%~1"=="" goto launcher
%PY% -I "%~dp0make_game.py" %*
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (echo WaveSpan finished.) else (echo WaveSpan stopped. See the message above.)
pause
exit /b %RC%

:launcher
%PY% -I "%~dp0make_game.py" --launcher
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" pause
exit /b %RC%
