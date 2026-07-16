@echo off
setlocal

set "VSPATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSPATH%" (
    set "VSPATH=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VSPATH%" (
    set "VSPATH=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VSPATH%" (
    set "VSPATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VSPATH%" (
    set "VSPATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VSPATH%" (
    echo ERROR: Cannot find vcvars64.bat
    exit /b 1
)

call "%VSPATH%" >nul 2>&1

set "MYLANG=build\mylang.exe"
set "SRC=examples\sdl3_window\sdl3_window.my"
set "OUT=build\sdl3_window"

if not exist %OUT% mkdir %OUT%

"%MYLANG%" "%SRC%" "%OUT%\sdl3_window.c"
if %ERRORLEVEL% neq 0 (
    echo ERROR: mylang compilation failed
    exit /b 1
)

cl /nologo /std:c11 /W3 /I src /I "%OUT%" /I "C:\vcpkg\installed\x64-windows\include" /Fe:%OUT%\sdl3_window.exe "%OUT%\sdl3_window.c" src\runtime.c examples\sdl3_window\sdl3_native.c /link "C:\vcpkg\installed\x64-windows\lib\SDL3.lib"
if %ERRORLEVEL% neq 0 (
    echo ERROR: C compilation failed
    exit /b 1
)

copy /y "C:\vcpkg\installed\x64-windows\bin\SDL3.dll" %OUT% >nul 2>&1

echo Built: %OUT%\sdl3_window.exe
