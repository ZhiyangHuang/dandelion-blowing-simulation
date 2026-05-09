@echo off
setlocal
cd /d "%~dp0"

set "MINGW_ROOT=C:\msys64\mingw64"
set "PATH=%MINGW_ROOT%\bin;%PATH%"

echo Building DandelionOS live runtime...
g++ -std=c++17 -Wall -Wextra -pedantic ^
  -I"%MINGW_ROOT%\include\SDL2" ^
  main.cpp scheduler.cpp simulation.cpp events.cpp bridge_io.cpp render_sdl.cpp ^
  -L"%MINGW_ROOT%\lib" -lmingw32 -lSDL2main -lSDL2 ^
  -o dandelion_live.exe
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Launching live runtime...
.\dandelion_live.exe
