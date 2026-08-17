@echo off
setlocal EnableDelayedExpansion
rem ============================================================================================
rem  win-build.bat - configure, build and test kvit-dictate in one command.
rem
rem    win-build.bat                 Debug
rem    win-build.bat Release         Release
rem    win-build.bat Debug clean     wipe the binary directory first
rem
rem  Echoes every step, because a build script that hides what it ran is a build script you
rem  cannot debug from a pasted log.
rem
rem  CMake is at "C:\Program Files\CMake\bin\cmake.exe" on this machine (plan.md section 5.3).
rem  Ninja is not on PATH, hence the Visual Studio generator, which the presets select.
rem ============================================================================================

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
if /I "%CONFIG%"=="debug" set "CONFIG=Debug"
if /I "%CONFIG%"=="release" set "CONFIG=Release"

if /I "%CONFIG%"=="Debug" (
    set "PRESET=windows-msvc-debug"
) else if /I "%CONFIG%"=="Release" (
    set "PRESET=windows-msvc-release"
) else (
    echo [win-build] unknown configuration "%CONFIG%" - expected Debug or Release
    exit /b 2
)

set "BUILDDIR=%REPO%\build-%PRESET%"

rem --- locate cmake -----------------------------------------------------------------------
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
if not exist "%CMAKE%" (
    for %%I in (cmake.exe) do set "CMAKE=%%~$PATH:I"
)
if not exist "%CMAKE%" (
    echo [win-build] cmake.exe not found at "C:\Program Files\CMake\bin\cmake.exe" nor on PATH.
    exit /b 3
)
set "CTEST=%CMAKE:cmake.exe=ctest.exe%"

echo [win-build] repo      : %REPO%
echo [win-build] cmake     : %CMAKE%
echo [win-build] preset    : %PRESET%
echo [win-build] config    : %CONFIG%
echo [win-build] build dir : %BUILDDIR%

if /I "%~2"=="clean" (
    echo [win-build] removing %BUILDDIR%
    if exist "%BUILDDIR%" rmdir /s /q "%BUILDDIR%"
)

rem --- configure --------------------------------------------------------------------------
echo.
echo [win-build] === configure ===
echo [win-build] "%CMAKE%" --preset %PRESET%
"%CMAKE%" --preset %PRESET%
if errorlevel 1 (
    echo [win-build] configure FAILED
    exit /b 1
)

rem --- build ------------------------------------------------------------------------------
echo.
echo [win-build] === build ===
echo [win-build] "%CMAKE%" --build "%BUILDDIR%" --config %CONFIG% -- -m
"%CMAKE%" --build "%BUILDDIR%" --config %CONFIG% -- -m
if errorlevel 1 (
    echo [win-build] build FAILED
    exit /b 1
)

rem --- test -------------------------------------------------------------------------------
rem  Disabled suites (see CMakeLists.txt) are reported as "Disabled" and do not fail the run;
rem  that is how the gap between planned and covered stays visible in every build.
echo.
echo [win-build] === ctest ===
echo [win-build] "%CTEST%" --test-dir "%BUILDDIR%" -C %CONFIG% --output-on-failure
"%CTEST%" --test-dir "%BUILDDIR%" -C %CONFIG% --output-on-failure
if errorlevel 1 (
    echo [win-build] tests FAILED
    exit /b 1
)

echo.
echo [win-build] OK - binaries in %BUILDDIR%\bin\%CONFIG%
endlocal
exit /b 0
