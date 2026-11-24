@echo off
rem ===========================================================================
rem  FastPeek - native Win32 build (static CRT, single self-contained exe)
rem  Output: FastPeek.exe  -- copy to a USB stick, double click, done.
rem ===========================================================================
setlocal
if defined VCINSTALLDIR goto :have_vc

set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto :call_vc
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto :call_vc
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto :call_vc

echo [x] cannot find vcvars64.bat, run inside a Developer Command Prompt.
exit /b 1

:call_vc
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

:have_vc
rc /nologo resource.rc || exit /b 1

cl /nologo /W4 /utf-8 /std:c11 /O2 /MT /GS- /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   src\main.c src\core.c src\scan.c src\image.c src\thumbcache.c src\video.cpp src\view.c src\chrome.c src\folders.c ^
   resource.res /Fe:FastPeek.exe ^
   /link /INCREMENTAL:NO /OPT:REF /OPT:ICF user32.lib gdi32.lib shell32.lib shlwapi.lib ^
   ole32.lib oleaut32.lib uuid.lib windowscodecs.lib mfplat.lib mfplay.lib mfuuid.lib ^
   dwmapi.lib comdlg32.lib advapi32.lib msimg32.lib
if errorlevel 1 goto :failed

del /q *.obj >nul 2>nul
echo.
echo [ok] FastPeek.exe
exit /b 0

:failed
echo.
echo [!] build failed
exit /b 1
