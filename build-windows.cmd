@echo off
setlocal
cd /d "%~dp0"
if not exist dist (
    mkdir dist
)
rem Run in an x64 Native Tools Command Prompt for Visual Studio.
cl /nologo /std:c++17 /O1 /MT /EHsc /utf-8 ^
    /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 ^
    main.cpp /Fe:dist\MeetScheduleWidget.exe ^
    /link /SUBSYSTEM:WINDOWS ^
    gdiplus.lib winhttp.lib shell32.lib ole32.lib user32.lib gdi32.lib
if errorlevel 1 (
    exit /b 1
)
copy /y vendor\LICENSE.json dist\THIRD_PARTY_NOTICES.txt >nul
if exist meetics.txt (
    copy /y meetics.txt dist\meetics.txt >nul
)
