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

set "CFLAGS=/nologo /std:c11 /fsanitize=address /Zi /W3"
set "SRCS=src\token.c src\ast.c src\lexer.c src\symtab.c src\parser.c src\codegen.c src\main.c"

if not exist build mkdir build

echo Building mylang-compiler...
cl %CFLAGS% /Fe:build\mylang.exe /Fo:build\ %SRCS%
if %ERRORLEVEL% neq 0 (
    echo ERROR: Compilation failed
    exit /b 1
)

echo Copying ASan DLL for runtime...
for /f "delims=" %%i in ('where clang_rt.asan_dynamic-x86_64.dll 2^>nul') do copy /y "%%i" build\ >nul 2>&1

echo Done.
