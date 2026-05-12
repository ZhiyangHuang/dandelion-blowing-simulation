@echo off
setlocal
cd /d "%~dp0"

set "MINGW_ROOT=C:\msys64\mingw64"
set "ORIGINAL_PATH=%PATH%"
set "PATH=%MINGW_ROOT%\bin;%PATH%"

echo Building DandelionOS live runtime...
g++ -std=c++17 -Wall -Wextra -pedantic ^
  -I"%MINGW_ROOT%\include\SDL2" ^
  main.cpp scheduler.cpp system_tasks.cpp camera_task.cpp microphone_task.cpp particle_generation_task.cpp particle_tasks.cpp simulation.cpp events.cpp bridge_io.cpp render_sdl.cpp runtime_orchestration.cpp ^
  -L"%MINGW_ROOT%\lib" -lmingw32 -lSDL2main -lSDL2 ^
  -o dandelion_live.exe
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

set "PATH=%ORIGINAL_PATH%"
echo Launching live runtime...
.\dandelion_live.exe %*
