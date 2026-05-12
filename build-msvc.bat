@echo off
REM justFPS MSVC Build Script

setlocal

REM Default: don't launch after build. Pass --run to launch.
set "RUN_AFTER="
if /I "%~1"=="--run" set "RUN_AFTER=1"

REM Change to the directory where this script is located
pushd "%~dp0"

REM Find vcvars64.bat
set "VCVARS="
for %%p in (
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist "%%~p" set "VCVARS=%%~p"

if not defined VCVARS (
    echo ERROR: Could not find Visual Studio vcvars64.bat
    echo Please install Visual Studio Build Tools with C++ workload
    exit /b 1
)

echo Setting up Visual Studio environment...
call "%VCVARS%"

echo Cleaning build directory...
if exist "build\justFPS.exe" (
    del /F /Q "build\justFPS.exe" 2>nul
    if exist "build\justFPS.exe" (
        echo   - justFPS.exe locked, killing process...
        taskkill /f /im justFPS.exe 2>nul
        timeout /t 1 /nobreak >nul
        del /F /Q "build\justFPS.exe" 2>nul
        if exist "build\justFPS.exe" (
            echo   - still locked, will overwrite during build
        ) else (
            echo   - justFPS.exe removed
        )
    ) else (
        echo   - justFPS.exe removed
    )
)
if exist "build" (
    del /F /Q "build\*.*" 2>nul
    for /D %%d in ("build\*") do rmdir /S /Q "%%d" 2>nul
    echo   - build\ cleaned
)
echo.
echo Building justFPS (Release x64)...
echo.

msbuild justFPS.vcxproj /p:Configuration=Release /p:Platform=x64 /m /verbosity:minimal
if errorlevel 1 (
    echo.
    echo BUILD FAILED!
    exit /b 1
)

echo.
echo Build successful!
echo.

echo Copying required DLLs...
if not exist "libs\lhwm\lhwm-wrapper.dll" (
    echo ERROR: libs\lhwm\lhwm-wrapper.dll not found!
    popd
    endlocal
    exit /b 1
)
copy /Y "libs\lhwm\lhwm-wrapper.dll" "build\" >nul
echo   - lhwm-wrapper.dll copied

REM Clean up intermediate files
if exist "build\obj" (
    rmdir /S /Q "build\obj" >nul 2>&1
    echo   - obj folder removed
)

echo.
echo ========================================
echo   Build complete!
echo   Output: build\justFPS.exe
echo ========================================
echo.
echo Required files in build folder:
echo   - justFPS.exe
echo   - lhwm-wrapper.dll
echo.
echo Run as Administrator for full functionality.
echo.

if defined RUN_AFTER (
    start "" "build\justFPS.exe"
)

popd
endlocal
