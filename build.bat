@echo off
setlocal
cd /d "%~dp0"
where g++ >nul 2>nul
if %errorlevel%==0 goto :mingw
where cl >nul 2>nul
if %errorlevel%==0 goto :msvc
echo No compiler found.
echo Install MinGW-w64 (winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT)
echo or run this from a Visual Studio Developer Command Prompt.
pause
exit /b 1

:mingw
g++ -std=c++17 -O3 -s -mwindows -static -municode main.cpp -o SkysS2FishingMacro.exe -lgdi32 -ldwmapi -lwinmm -lcomctl32 -lgdiplus -lole32 -lwinhttp
goto :done

:msvc
cl /nologo /std:c++17 /O2 /EHsc /DUNICODE /D_UNICODE main.cpp /link /SUBSYSTEM:WINDOWS /OUT:SkysS2FishingMacro.exe user32.lib gdi32.lib dwmapi.lib winmm.lib comctl32.lib gdiplus.lib ole32.lib winhttp.lib

:done
if errorlevel 1 (
  echo Build failed.
  pause
  exit /b 1
)
echo Built SkysS2FishingMacro.exe
pause
