# DandelionOS

DandelionOS is a C++ scheduling visualization project for an operating-systems
course. It combines:

- a scheduler with visible queue / lane behavior
- an SDL visualization layer
- a Python MediaPipe bridge for camera mouth detection
- a native Windows microphone bridge
- a particle world that reacts to camera and microphone input

`DESIGN.md` is the canonical architecture document.
`PROJECT_PROGRESS.txt` is the running implementation log and next-step tracker.

## Runtime Overview

The project models a human interaction loop as a scheduler-visible pipeline:

```text
Camera listener
-> CameraTask
-> mouth-open gate
-> Microphone listener
-> MicrophoneTask
-> GenerateParticleTask / BreezeTask
-> BatchParticleExecutionTask
-> particle move / fade drain
-> reseed next listener entry
```

The current scheduler has four physical queues:

- `P1_SYSTEM` -> `L0 System`
- `P2_REALTIME` -> `L1 Realtime`
- `P2_FUNCTIONAL` -> `L2 Interactive`
- `P3_PARTICLE` -> `L3 Throughput`

The realtime layer is split into two listener families:

- long listeners:
  - `CameraListenerServiceTask`
  - `MicrophoneListenerServiceTask`
- short listeners:
  - `CameraListenerBurstTask`
  - `MicrophoneListenerBurstTask`

Current mode mapping:

```text
1 thread  -> short + short + deterministic fixed chain
2 threads -> camera long + microphone short + general lane
3 threads -> camera long + microphone long + general lane
```

## Repository Layout

Core runtime:

- `main.cpp`
- `thread.h`
- `scheduler.cpp`
- `scheduler_task_support.h`

Task families:

- `system_tasks.cpp`
- `camera_task.cpp`
- `microphone_task.cpp`
- `particle_generation_task.cpp`
- `particle_tasks.cpp`

Bridge / rendering / simulation:

- `bridge_io.cpp`
- `render_sdl.cpp`
- `simulation.cpp`
- `runtime_orchestration.cpp`

Docs / verification:

- `DESIGN.md`
- `PROJECT_PROGRESS.txt`
- `queue_verification.cpp`
- `queue_verification_output.txt`

## Environment

The current repo is Windows-oriented.

Assumptions:

- Windows audio input through `waveIn`
- Python available on PATH
- MinGW from `C:\msys64\mingw64`
- SDL2 headers and libraries under that MinGW tree

Required runtime assets:

- `face_landmarker.task`
- `python_mediapipe_bridge.py`

Python packages:

```powershell
python -m pip install --upgrade pip
python -m pip install opencv-python mediapipe
```

## Build and Run

### Live Runtime

Use the provided batch script:

```powershell
.\run_live.bat
```

That script currently builds:

```text
main.cpp
scheduler.cpp
system_tasks.cpp
camera_task.cpp
microphone_task.cpp
particle_generation_task.cpp
particle_tasks.cpp
simulation.cpp
events.cpp
bridge_io.cpp
render_sdl.cpp
runtime_orchestration.cpp
```

and outputs `dandelion_live.exe`.

### Headless Verification

Build the verifier:

```powershell
$env:PATH='C:\msys64\mingw64\bin;' + $env:PATH
g++ -std=c++17 -Wall -Wextra -pedantic `
  -I"C:\msys64\mingw64\include\SDL2" `
  queue_verification.cpp scheduler.cpp system_tasks.cpp camera_task.cpp `
  microphone_task.cpp particle_generation_task.cpp particle_tasks.cpp `
  simulation.cpp events.cpp bridge_io.cpp render_sdl.cpp runtime_orchestration.cpp `
  -L"C:\msys64\mingw64\lib" -lmingw32 -lSDL2main -lSDL2 `
  -o queue_verification.exe
```

Run it:

```powershell
.\queue_verification.exe
```

To also store the output:

```powershell
.\queue_verification.exe | Tee-Object -FilePath queue_verification_output.txt
```

## Keyboard Controls

In the SDL window:

- `q` or `Esc`: quit
- `r`: reset
- `c`: change dandelion layout
- `1` / `2` / `3`: switch thread mode
- `x`: force breeze fallback

## What The Verifier Covers

Current verification includes:

- queue precedence across realtime / interactive / throughput
- single-thread burst-entry reseed
- single-thread full-cycle and fallback-cycle completion
- mode collapse preserving downstream phase order
- listener runtime snapshot bookkeeping
- two-thread camera-long / microphone-short behavior
- realtime slices cutting across multi-thread dispatch slots
- microphone overlap with active particle drain
- reset / exit preemption
- persistent-listener hard stop on mode collapse
- short microphone lease not auto-renewing after expiration in `2-thread`

## Source-of-Truth Notes

If documents disagree, use them in this order:

1. current code behavior
2. `DESIGN.md`
3. `PROJECT_PROGRESS.txt`

`ProjectGuideline.md` currently exists in the repo but is empty as of
`2026-05-11`, so it does not currently add extra constraints beyond the files
above.

## Preparing `v2` Branch Upload

This repo contains generated binaries, object files, bridge JSON artifacts, and
editor metadata during normal development. `.gitignore` is set up to keep those
out of the next upload-oriented branch.

Before pushing a clean `v2` branch, re-check:

- no generated `.exe` / `.o` / `.obj` files are staged
- no runtime bridge JSON snapshots are staged
- no local IDE folders are staged
- docs reflect the current scheduler semantics
- verifier output is refreshed if you want to include it intentionally
