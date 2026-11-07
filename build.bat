@echo off
rem Builds build\dinput8.dll (the mod) and build\hfblade-launcher.exe (x86) with the VS 2022 toolchain.
setlocal
set "VS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :fallback
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
:fallback
if not defined VS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community" set "VS=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VS (echo Visual Studio C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /EHsc /W4 /Fobuild\ src\dllmain.cpp ^
   /LD /Febuild\dinput8.dll ^
   /link /DEF:src\dinput8.def kernel32.lib user32.lib || exit /b 1
set "RCDEF="
if exist src\launcher\launcher.ico set "RCDEF=/dHAVE_ICON"
rc /nologo %RCDEF% /fo build\launcher.res src\launcher\launcher.rc || exit /b 1
cl /nologo /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /Fobuild\ src\launcher\launcher.cpp src\launcher\audio.cpp build\launcher.res ^
   /Febuild\hfblade-launcher.exe ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib shell32.lib gdiplus.lib uxtheme.lib dwmapi.lib ole32.lib xaudio2.lib mfplat.lib mfreadwrite.lib mfuuid.lib
exit /b %ERRORLEVEL%
