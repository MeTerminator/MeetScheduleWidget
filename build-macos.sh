#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p dist
x86_64-w64-mingw32-g++ main.cpp -o dist/MeetScheduleWidget.exe \
  -std=c++17 -Os -s -municode -mwindows -static -static-libgcc -static-libstdc++ \
  -D_WIN32_WINNT=0x0A00 -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Wall -Wextra -lgdiplus -lwinhttp -lshell32 -lole32 -luser32 -lgdi32
cp vendor/LICENSE.json dist/THIRD_PARTY_NOTICES.txt
if [ -f meetics.txt ]; then cp meetics.txt dist/meetics.txt; fi
