@echo off
rem ============================================================================
rem  SRO PK2 Studio - Release build (x64, single static exe)
rem  Needs Visual Studio 2022 / 2026 with "Desktop development with C++".
rem  Output: bin\SroPk2Studio.exe  (the only file in bin\; objects stay in obj\)
rem  Usage : build.bat            (pauses at the end when double-clicked)
rem          build.bat /nopause   (for scripts / CI)
rem ============================================================================
setlocal EnableExtensions
title SRO PK2 Studio - Release build
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [HATA] Visual Studio bulunamadi ^(vswhere.exe yok^).
    echo        Visual Studio 2022/2026 ve "Desktop development with C++" is yukunu kurun.
    goto :fail
)

set "MSBUILD="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe`) do (
    if not defined MSBUILD set "MSBUILD=%%i"
)
if not defined MSBUILD (
    echo [HATA] MSBuild / MSVC C++ araclari bulunamadi.
    echo        Visual Studio Installer'da "Desktop development with C++" is yukunu ekleyin.
    goto :fail
)

echo MSBuild : %MSBUILD%
echo Proje   : SroPk2Studio.sln  ^(Release ^| x64^)
echo.
"%MSBUILD%" "SroPk2Studio.sln" /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64
if errorlevel 1 goto :fail
if not exist "bin\SroPk2Studio.exe" goto :fail

echo.
echo [OK] Derleme tamamlandi / Build succeeded:
echo      %~dp0bin\SroPk2Studio.exe
if /i not "%~1"=="/nopause" pause
exit /b 0

:fail
echo.
echo [HATA] Derleme basarisiz.
if /i not "%~1"=="/nopause" pause
exit /b 1
