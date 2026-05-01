# Dandelion Blowing Simulation

## Remote Deployment

Use this section first if you want to move the project to another Windows machine.

### Target environment

Recommended target:

- Windows
- MSYS2 MinGW64
- SDL2 available through MSYS2
- OpenCV available through MSYS2
- webcam available
- microphone available

### Files to copy

Copy the whole project folder, including:

- `main.cpp`
- `main_logic_test.cpp`
- `main_runtime_test.cpp`
- `thread.h`
- `scheduler.cpp`
- `events.cpp`
- `simulation.cpp`
- `render_sdl.cpp`
- `visualization_stub.cpp`
- `README.md`
- `DESIGN.md`

### Dependency setup on another machine

1. Install MSYS2.
2. Open `MSYS2 MinGW64`.
3. Install the required packages:

```bash
pacman -Syu
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-opencv
```

If `pacman -Syu` asks you to restart the shell, close the window, reopen `MSYS2 MinGW64`, and run the next command.

### Build on another machine

Interactive app:

```bash
C:\msys64\mingw64\bin\g++.exe -std=c++17 -Wall -Wextra -pedantic main.cpp scheduler.cpp events.cpp simulation.cpp render_sdl.cpp -IC:\msys64\mingw64\include\SDL2 -IC:\msys64\mingw64\include\opencv4 -LC:\msys64\mingw64\lib -lmingw32 -lSDL2main -lSDL2 -lopencv_videoio -lopencv_objdetect -lopencv_imgproc -lopencv_imgcodecs -lopencv_core -o blowing_main.exe
```

### Run on another machine

If `C:\msys64\mingw64\bin` is already in `PATH`:

```bash
.\blowing_main.exe
```

### Remote deployment checklist

- build succeeds on the target machine
- `dandelion_logic_test.exe` prints `logic test passed`
- `dandelion_runtime_test.exe` prints `runtime smoke test passed`
- webcam background opens in `blowing_main.exe`
- `MIC DEV` shows a valid input device name
- switching `1T / 2T / 3T` still works

### Common remote deployment issues

- If `MIC RMS` stays `0`, the target machine may be using the wrong default microphone.
- If webcam does not appear, the target machine may not grant camera permission.
- If `SDL2.dll` or OpenCV runtime errors appear, make sure the app is launched from an environment where `C:\msys64\mingw64\bin` is available.
- If OpenCV prints a GStreamer warning but the app still runs, that warning alone does not mean the runtime failed.

## Overview

This project keeps the original operating-systems goal intact:

- user-level threads
- priority scheduling
- cooperative execution
- event-driven wakeup
- latest-value buffer
- Round Robin particle workers

On top of that core, it adds a real SDL2 shell with:

- webcam background
- microphone capture
- dandelion particle visualization
- thread-mode controls
- HUD-based runtime inspection

The project is still an OS-style runtime first. The UI is a shell around that runtime.

## Current Runtime Model

The runtime now defaults to `1T`.

- `1T`: camera, mic, and particle phases alternate through one scheduling flow
- `2T`: camera and mic share input flow while render stays separate
- `3T`: camera, mic, and render can remain independently active

Actual created user-level thread counts now match the selected mode:

- `1T` creates `1` thread
- `2T` creates `2` threads
- `3T` creates `3` threads

Priority policy in the current code:

- `HIGH`: mic
- `MEDIUM`: render
- `LOW`: camera and particle workers

This matches the current implementation rather than the earlier draft design.

## Project Files

- [main.cpp]: normal interactive application entry
- [main_logic_test.cpp]: core logic smoke test
- [main_runtime_test.cpp]: real runtime startup smoke test
- [thread.h]: shared enums, structs, and APIs
- [scheduler.cpp]: scheduler, queues, blocking, wakeup, timer, event loop
- [events.cpp]: event queue and latest-value buffer storage
- [simulation.cpp]: camera, mic, render, and particle task logic
- [render_sdl.cpp]: SDL2 window, webcam background, HUD, and particle visualization
- [visualization_stub.cpp]: visualization stub used by logic-only test

## Current Visual Behavior

The visual shell is now tied to runtime task state:

- the mouth box and guide line appear only while the mouth-open task is active
- the blue wind ribbons appear only while the mic task is active
- the dandelion sway animation appears only while particle processing is active
- particles stop individually when each one reaches the border

The webcam is used as the background layer. The old separate camera preview panel has been removed.

## Current Input Behavior

Camera:

- current implementation uses OpenCV Haar cascades
- face detection: `haarcascade_frontalface_default.xml`
- mouth-open approximation: `haarcascade_smile.xml`

Microphone:

- current implementation uses SDL audio capture
- SDL opens the system default recording device
- the HUD shows:
  - `MIC RMS`
  - `MIC TH`
  - `MIC DEV`

This helps verify whether the microphone path is actually active.

## Seed Model

The backend seed pool is now `100`.

- `seeds_total = 100`
- `seeds_left = 100`
- `max_spawn_budget = 100`

The center dandelion does not draw all 100 seeds literally.
Instead:

- the center visualization maps the backend pool to at most `27` visible seeds
- above half, seeds are distributed around a full ring
- below half, seeds collapse into the current half-ring style

## Test Plan

### 1. Interactive test

Use `blowing_main.exe`.

Checks:

- default thread mode is `1T`
- `1T / 2T / 3T` buttons work
- keys `1 / 2 / 3` switch runtime modes
- webcam background is visible
- camera highlight appears only while the camera task is in its input phase
- mouth box appears only during mouth-open detection
- wind ribbons appear only while mic is active
- dandelion sway appears only while particle work is active
- particles stop individually at borders
- `MIC RMS`, `MIC TH`, and `MIC DEV` update in HUD

## Controls

- `1 / 2 / 3`: switch thread mode
- `C`: camera event
- `M`: mic start
- `N`: mic end
- `R`: reset
- `B`: toggle webcam background
- `V`: toggle mouth cursor
- `Esc`: close app

## Limitations

- MediaPipe Face Mesh is not integrated yet
- mouth-open detection still uses OpenCV Haar cascades
- audio capture depends on the system default microphone device selected by SDL
- text rendering is still minimal and custom
- preemption is still simulated, not OS-native

## Next Steps

- replace Haar mouth-open detection with MediaPipe Face Mesh
- improve microphone device selection
- tune mic thresholds per machine
- refine particle physics and burst timing
- expand automated runtime validation
