@echo off

where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo cl.exe is not in PATH, searching for vcvarsall.bat...

    set VCVARS=
    for /d %%i in ("C:\Program Files\Microsoft Visual Studio\2022\*") do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat
    )
    for /d %%i in ("C:\Program Files\Microsoft Visual Studio\2019\*") do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat
    )
    for /d %%i in ("C:\Program Files (x86)\Microsoft Visual Studio\2019\*") do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat
    )
    for /d %%i in ("C:\Program Files (x86)\Microsoft Visual Studio\2017\*") do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat
    )

    if defined VCVARS (
        echo Found: %VCVARS%
        call "%VCVARS%" x64
    ) else (
        echo ERROR: vcvarsall.bat not found
        echo Run build.bat in "Developer Command Prompt for VS".
        pause
        exit /b 1
    )
)

echo.
echo compiling resources...
rc.exe app.rc
if %ERRORLEVEL% neq 0 ( echo ERROR: rc.exe failed & pause & exit /b 1 )

echo compiling app...
cl.exe /EHsc /O2 /W3 /Fe:AirPing.exe main.cpp GoogleCalendar.cpp overlay.cpp app.res ^
    user32.lib gdi32.lib shell32.lib winhttp.lib msimg32.lib advapi32.lib ws2_32.lib ole32.lib windowscodecs.lib
if %ERRORLEVEL% neq 0 ( echo ERROR: compilation failed & pause & exit /b 1 )

echo.
echo CLEAN
del *.obj 2>nul
del app.res 2>nul

echo AirPing.exe built successfully!
pause

