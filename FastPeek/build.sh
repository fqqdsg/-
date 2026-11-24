#!/bin/sh
# builds FastPeek.exe with the MSVC toolchain, without needing vcvars (the
# sandbox blocks some registry access, so INCLUDE/LIB are passed explicitly).
set -e
cd "$(dirname "$0")"

VCTOOLS="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
SDK="/c/Program Files (x86)/Windows Kits/10"
SDKVER="10.0.26100.0"

export INCLUDE="C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Tools\\MSVC\\14.51.36231\\include;C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt;C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\shared;C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\um;C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\winrt"
export LIB="C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Tools\\MSVC\\14.51.36231\\lib\\x64;C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.26100.0\\ucrt\\x64;C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.26100.0\\um\\x64"

export MSYS2_ARG_CONV_EXCL='*'

"$SDK/bin/$SDKVER/x64/rc.exe" /nologo /fo resource.res resource.rc

"$VCTOOLS/bin/Hostx64/x64/cl.exe" /nologo /W4 /utf-8 /std:c11 /O2 /MT /GS- \
  /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS \
  src/main.c src/core.c src/scan.c src/image.c src/thumbcache.c src/video.cpp src/view.c src/chrome.c src/folders.c \
  resource.res /Fe:FastPeek.exe \
  /link /INCREMENTAL:NO /OPT:REF /OPT:ICF \
  user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib uuid.lib \
  windowscodecs.lib mfuuid.lib dwmapi.lib comdlg32.lib advapi32.lib msimg32.lib

rm -f *.obj
echo "[ok] FastPeek.exe"
