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
set "SRC=examples\fstring_basics\fstring_basics.my"
set "OUT=build\fstring_basics"

if not exist %OUT% mkdir %OUT%

"%MYLANG%" "%SRC%" "%OUT%\fstring_basics.c"
if %ERRORLEVEL% neq 0 (
    echo ERROR: mylang compilation failed
    exit /b 1
)

cl /nologo /std:c11 /W3 /I src /I "%OUT%" /Fe:%OUT%\fstring_basics.exe "%OUT%\fstring_basics.c" src\runtime.c
if %ERRORLEVEL% neq 0 (
    echo ERROR: C compilation failed
    exit /b 1
)

echo Built: %OUT%\fstring_basics.exe
