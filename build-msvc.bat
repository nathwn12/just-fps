@echo off
REM justFPS MSVC Build Script

setlocal

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
for %%f in ("lhwm-wrapper.dll" "LibreHardwareMonitorLib.dll") do (
    if exist "libs\lhwm\%%~f" (
        copy /Y "libs\lhwm\%%~f" "build\" >nul
        echo   - %%~f copied
    )
)

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
echo   - lhwm-wrapper.dll (for LHWM support)
echo   - LibreHardwareMonitorLib.dll (for LHWM support)
echo.
echo Run as Administrator for full functionality.

endlocal
