@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
setlocal EnableExtensions
if not defined MINGWDIR set "MINGWDIR=C:\ProgramData\mingw64\mingw64\bin"
if exist "%MINGWDIR%\gcc.exe" set "PATH=%MINGWDIR%;%PATH%"
where mingw32-make >nul 2>&1 && goto :use_mingw32
where make >nul 2>&1 && goto :use_make
echo ERROR: no make found. Set MINGWDIR to your MinGW-w64 bin directory.
exit /b 1

:use_mingw32
mingw32-make %*
exit /b %errorlevel%

:use_make
make %*
exit /b %errorlevel%
