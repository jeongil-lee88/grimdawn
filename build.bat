@echo off
rem Builds both DLLs of the Grim Dawn Korean Voice Pack.
rem native-lorevo.dll: run inside "x64 Native Tools Command Prompt for VS 2022" (MSVC 14.44, /Brepro = no timestamps).
rem dinput8.dll:       TinyCC 0.9.27 x86_64-win32 (tcc.exe on PATH, or set TCC=path\to\tcc.exe).
setlocal
cd /d "%~dp0"
if not exist build mkdir build
cd build

set CFLAGS=/nologo /c /O2 /MD /W3 /GS- /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Brepro /I..\third_party\minhook\include
cl %CFLAGS% ..\third_party\minhook\src\buffer.c ..\third_party\minhook\src\hook.c ..\third_party\minhook\src\trampoline.c ..\third_party\minhook\src\hde\hde64.c || exit /b 1
cl %CFLAGS% ..\native-lorevo\native-lorevo.c || exit /b 1
link /nologo /DLL /MACHINE:X64 /OPT:REF /INCREMENTAL:NO /Brepro /OUT:native-lorevo.dll native-lorevo.obj buffer.obj hook.obj trampoline.obj hde64.obj kernel32.lib user32.lib shell32.lib shlwapi.lib || exit /b 1

if "%TCC%"=="" set TCC=tcc
copy /y ..\proxy\dinput8-proxy.c . >nul
"%TCC%" -shared -o dinput8.dll dinput8-proxy.c || exit /b 1

echo.
echo Built build\native-lorevo.dll and build\dinput8.dll
certutil -hashfile native-lorevo.dll SHA256 | findstr /v /c:"CertUtil"
certutil -hashfile dinput8.dll SHA256 | findstr /v /c:"CertUtil"
