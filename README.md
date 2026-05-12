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

The live runtime is now launch-fixed rather than runtime-switchable.
Choose the mode once at startup with `--single`, `--dual`, or `--triple`.

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
- `demo_io_loop_runtime_log.txt`
- `demo_single_mode_log.txt`
- `demo_dual_mode_log.txt`
- `demo_triple_mode_log.txt`

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

Or launch a fixed evaluation mode explicitly:

```powershell
.\run_live.bat --single
.\run_live.bat --dual
.\run_live.bat --triple
.\run_live.bat --triple --demo-camera
.\run_live.bat --triple --demo-io-loop
.\run_live.bat --triple --demo-io-loop --auto-exit-ms 5000 --log-file demo_io_loop_runtime_log.txt
```

Mode-specialized runnable demo wrappers:

```powershell
.\run_demo_single_mode.bat
.\run_demo_dual_mode.bat
.\run_demo_triple_mode.bat
```

Generated runtime logs:

```text
demo_single_mode_log.txt
demo_dual_mode_log.txt
demo_triple_mode_log.txt
```

Mode intent:

- `--single`: deterministic sequential runtime
- `--dual`: camera-prioritized constrained parallelism
- `--triple`: fully partitioned listener + throughput runtime
- `--demo-camera`: replace the live camera bridge with a stable manual demo source
- `--demo-io-loop`: replace both live camera and live microphone sources with auto-looping simulated I/O
- `--auto-exit-ms N`: stop automatically after `N` milliseconds
- `--log-file PATH`: mirror runtime note output into `PATH`

Mode-specific scheduling signatures in the logs:

- `demo_single_mode_log.txt`: one worker thread repeatedly advances the fixed chain, so camera, microphone, generate, and batch drain work appear as a mostly sequential trace.
- `demo_dual_mode_log.txt`: two worker threads split interactive work from throughput drain, so camera detection and particle batch requeue activity overlap across `Thread 1` and `Thread 2`.
- `demo_triple_mode_log.txt`: three worker threads separate camera, microphone, and particle throughput lanes, so the trace shows distinct ownership across `Thread 1`, `Thread 2`, and `Thread 3`.

Recommended final-demo fallback when the live camera stack is flaky:

- launch `.\run_live.bat --triple --demo-camera`
- press `o` in the SDL window to inject a mouth-open pulse
- keep the real microphone path live so the scheduler still demonstrates the camera-to-microphone handoff and particle pipeline

Recommended fully self-running fallback when both live listener stacks are unstable:

- launch `.\run_live.bat --triple --demo-io-loop`
- the runtime will continuously cycle simulated camera-open and microphone-blow samples
- this keeps the existing listener tasks, queues, and thread partitioning alive instead of replacing the scheduler with a one-shot scripted animation

Recommended log-producing final-demo capture:

- launch `.\run_live.bat --triple --demo-io-loop --auto-exit-ms 5000 --log-file demo_io_loop_runtime_log.txt`
- the runtime will loop automatically, stop after five seconds, and leave a text log on disk
- `demo_io_loop_runtime_log.txt` in this repo is an example capture generated from that path

Three specialized scheduling-simulation versions for the course demo:

- `run_demo_single_mode.bat`
  - launches `--single --demo-io-loop`
  - emphasizes deterministic sequential scheduling
  - generated log: `demo_single_mode_log.txt`
- `run_demo_dual_mode.bat`
  - launches `--dual --demo-io-loop`
  - emphasizes camera-prioritized constrained parallelism
  - generated log: `demo_dual_mode_log.txt`
- `run_demo_triple_mode.bat`
  - launches `--triple --demo-io-loop`
  - emphasizes fully partitioned listener + throughput execution
  - generated log: `demo_triple_mode_log.txt`

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
- `x`: force breeze fallback
- `o`: inject a mouth-open pulse when `--demo-camera` is enabled

## What The Verifier Covers

Current verification includes:

- queue precedence across realtime / interactive / throughput
- single-thread burst-entry reseed
- single-thread full-cycle and fallback-cycle completion
- watchdog recovery after stale listener heartbeat timeout
- scripted replay of `mouth open -> valid blow -> overlapping input -> reset`
- fairness sanity: realtime, interactive, and throughput all make progress
- mode collapse preserving downstream phase order
- listener runtime snapshot bookkeeping
- two-thread camera-long / microphone-short behavior
- realtime slices cutting across multi-thread dispatch slots
- microphone overlap with active particle drain
- reset / exit preemption
- persistent-listener hard stop on mode collapse
- short microphone lease not auto-renewing after expiration in `2-thread`

The verifier also records report-friendly counters such as:

- camera reseed count
- microphone reseed count
- watchdog recovery count
- realtime slices per frame
- average particle drain ticks

## Verification Matrix

| Invariant | Evidence |
| --- | --- |
| `L0` preemption outranks listener and business work | verifier checks reset / exit stop peer-lane dispatch and reseed correctly |
| `L1` listener residency is mode-correct | verifier checks `single=short+short`, `dual=camera long + mic short`, `triple=long+long` |
| queue ordering follows delay budgets | verifier checks realtime precedence over interactive and interactive handoff into throughput |
| overlap is intentional rather than accidental | verifier checks microphone handoff can overlap active `P3` drain in multi-thread mode |
| stalled listeners recover instead of wedging the runtime | verifier checks watchdog timeout clears stale listener state and reseeds the correct listener form |
| throughput eventually drains | verifier checks full-cycle drain and fairness sanity with nonzero average drain ticks |

## Design Trade-Offs

- Long-listener vs short-listener split: long listeners amortize startup cost in multi-thread mode, while short listeners preserve deterministic entry ordering in single-thread mode.
- Camera-prioritized `2-thread` mode: camera stays long-lived because it has the slower startup path and semantically opens the whole interaction chain.
- Startup-fixed topology: locking the mode at launch makes demonstrations and grading more stable than runtime `3 -> 2 -> 1` migration.
- Watchdog recovery over perfect migration bookkeeping: the project favors eventual recovery from stale listener state over complex exact ownership logic.
- Indirect `P3` batching: `BatchParticleExecutionTask` keeps throughput visible and fair instead of letting every worker mutate particle queues independently.

## Performance Observations

- `single`: best for deterministic explanation and repeatable fixed-phase behavior.
- `dual`: shows constrained overlap while preserving camera-first listener residency.
- `triple`: gives the best responsiveness because camera, microphone, and general throughput work can stay partitioned.
- Current counters make these comparisons visible at runtime through reseed counts, realtime slices per frame, watchdog recoveries, and average particle drain ticks.

## Limitations And Future Work

- This is a custom user-space scheduler/runtime, not a kernel scheduler.
- Listener freshness still relies on external camera / microphone bridge behavior.
- Timing goals such as the short-listener `10s` lease are approximate realtime guidance, not hard guarantees.
- Sample identity still primarily uses timestamps; explicit sequence ids would make replay and dedupe stricter.
- Fairness evidence is workload-specific to this interaction pipeline rather than a general scheduler proof.
- Good next steps would be deterministic replay traces, starvation counters, adaptive lease policy, and more formal latency logging.

## Source-of-Truth Notes

If documents disagree, use them in this order:

1. current code behavior
2. `DESIGN.md`
3. `PROJECT_PROGRESS.txt`
4. `ProjectGuideline.md`
