@echo off
rem Fetch Lua 5.4.7 source into _deps\lua so the workshop plugin can build
rem on Windows. Run once after cloning the repo. Idempotent.

setlocal
set HERE=%~dp0
set DEPS=%HERE%_deps
set LUA_VERSION=5.4.7
set LUA_TARBALL=lua-%LUA_VERSION%.tar.gz
set LUA_URL=https://www.lua.org/ftp/%LUA_TARBALL%

if exist "%DEPS%\lua\src\lua.h" (
    echo Lua %LUA_VERSION% already present at %DEPS%\lua
    exit /b 0
)

if not exist "%DEPS%" mkdir "%DEPS%"
pushd "%DEPS%" >nul

echo Downloading %LUA_URL% ...
powershell -NoProfile -Command "Invoke-WebRequest -Uri '%LUA_URL%' -OutFile '%LUA_TARBALL%'"
if errorlevel 1 (
    echo Download failed. Need PowerShell with internet access. 1>&2
    popd >nul
    exit /b 1
)

rem tar.exe ships with Windows 10/11 build 17063+.
tar -xf "%LUA_TARBALL%"
if errorlevel 1 (
    echo Extract failed. Need tar.exe (Windows 10 1803+). 1>&2
    popd >nul
    exit /b 1
)

if exist lua rmdir /s /q lua
ren "lua-%LUA_VERSION%" lua
del "%LUA_TARBALL%"

popd >nul
echo Lua %LUA_VERSION% extracted to %DEPS%\lua
endlocal
