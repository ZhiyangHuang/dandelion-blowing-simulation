# Design Notes

## Goal

The goal of this project is to turn an operating-systems course runtime into a real interactive visual application.

The core idea remains unchanged:

- a user-level scheduler controls work
- events wake blocked tasks
- particle simulation is executed by low-priority workers
- the latest-value buffer stores only the newest relevant state

The new addition is a graphical SDL2 shell that visualizes the runtime and accepts interactive input.

## Final Layered Structure

The current implementation is organized into five practical layers.

### 1. Shared Type Layer

[thread.h](/c:/Users/ZhiyangHuang/Desktop/Code练习/2026/CSCI%2049392%20OS/Blowing/thread.h:1) defines:

- thread state enums
- priority enums
- event enums
- render state enums
- `Thread`
- `Event`
- `Mutex`
- `LatestValueBuffer`

This file is the contract shared by the scheduler, simulation logic, and SDL shell.

### 2. Scheduler Layer

[scheduler.cpp](/c:/Users/ZhiyangHuang/Desktop/Code练习/2026/CSCI%2049392%20OS/Blowing/scheduler.cpp:1) contains:

- `HIGH`, `MEDIUM`, and `LOW` ready queues
- thread creation
- `yield()`
- `block()`
- `wakeup()`
- mutex coordination
- timer hook
- event loop

Priority policy:

- `HIGH`: mic
- `MEDIUM`: render
- `LOW`: camera and particle workers

The scheduler is still the core of the application. SDL does not schedule work; it only feeds input and renders state.

### 3. Event and State Layer

[events.cpp](/c:/Users/ZhiyangHuang/Desktop/Code练习/2026/CSCI%2049392%20OS/Blowing/events.cpp:1) stores:

- the event queue
- the latest-value buffer
- the global render state

All user interaction eventually becomes events. This means the GUI follows the same architecture as future real camera or microphone input.

### 4. Simulation Layer

[simulation.cpp](/c:/Users/ZhiyangHuang/Desktop/Code练习/2026/CSCI%2049392%20OS/Blowing/simulation.cpp:1) implements:

- `camera()`
- `mic_thread()`
- `render_thread()`
- `particle()`
- `handle_event()`
- `simulation_tick()`
- `simulation_done()`
- `reset_simulation()`

Responsibilities:

- camera updates mouth position and mouth-open state
- mic controls wind start, stop, and strength
- render thread reports UI-related status
- particle workers perform movement updates

### 5. SDL2 Shell Layer

[render_sdl.cpp](/c:/Users/ZhiyangHuang/Desktop/Code练习/2026/CSCI%2049392%20OS/Blowing/render_sdl.cpp:1) provides:

- SDL window creation
- webcam background rendering
- default microphone capture
- input polling
- on-screen buttons
- HUD panels
- custom bitmap text
- particle drawing
- dandelion scene rendering

This layer does not replace the runtime. It visualizes it.

## Why This Architecture Works

The key design rule is separation:

- scheduler decides who runs
- event system transports intent
- simulation updates state
- SDL draws the current state and emits user actions

Because of this separation, the SDL shell can later be replaced or extended with:

- MediaPipe Face Mesh
- stronger audio/VAD input
- richer rendering

without rewriting the scheduling core.

## Thread State Model

Threads still move through:

- `READY`
- `RUNNING`
- `BLOCKED`
- `FINISHED`

Important transitions:

- `READY -> RUNNING`: selected by scheduler
- `RUNNING -> READY`: voluntary `yield()`
- `RUNNING -> BLOCKED`: waits for event or resource
- `BLOCKED -> READY`: event-driven `wakeup()`
- `RUNNING -> FINISHED`: task completes

This remains one of the main OS concepts demonstrated by the project.

## Event Flow In The SDL Version

The GUI does not call simulation logic directly.

Instead:

1. Keyboard or mouse input is read in `render_sdl.cpp`.
2. The SDL shell emits an event such as `CAMERA_OPEN`, `MIC_START`, `MIC_END`, or `UI_RESET`.
3. The scheduler loop polls and dispatches the event.
4. `handle_event()` wakes the appropriate thread.
5. The thread runs according to priority rules.
6. Updated state is rendered in the next frame.

This keeps the graphical version faithful to the original event-driven runtime model.

## Latest-Value Buffer Design

The project uses `LatestValueBuffer` instead of a normal queue for simulation state.

Stored state:

- `spawn_budget`
- `max_spawn_budget`
- `mouth_open`
- `wind_active`
- `mouth_x/y`
- `wind_strength`
- `wind_dir_x/y`
- `seeds_total/seeds_left`
- `overflow_count`

Why:

- the latest state matters more than historical backlog
- low-latency behavior fits real-time interaction better
- overflow can be visualized and measured

This is a deliberate OS and systems design choice, not just a simplification.

## Current Runtime Modes

The live version now defaults to `1T`.

- `1T`: camera, mic, and particle phases alternate through the same scheduling flow
- `2T`: camera and mic share input flow while render remains separate
- `3T`: camera, mic, and render can remain independently active

The UI exposes `1T / 2T / 3T` buttons and keyboard shortcuts `1 / 2 / 3`.

Two explicit test entry points are also included now:

- [main_logic_test.cpp](/c:/Users/ZhiyangHuang/Desktop/Code练习/2026/CSCI%2049392%20OS/Blowing/main_logic_test.cpp:1) for core runtime behavior
- [main_runtime_test.cpp](/c:/Users/ZhiyangHuang/Desktop/Code练习/2026/CSCI%2049392%20OS/Blowing/main_runtime_test.cpp:1) for real SDL/OpenCV startup behavior

## SDL2 Visual Design

The current SDL2 shell includes:

- larger application window
- left-side scene composition
- right-side metrics and controls panels
- stylized dandelion core
- seed-like particles with stems and fan shapes
- wind ribbons in the background
- clickable buttons
- simple HUD rendered with a custom bitmap font
- webcam background as the primary scene layer
- mouth cursor and mouth-to-dandelion guide line
- microphone HUD with live RMS and threshold display
- no separate camera preview panel
- no start/continue placeholder UI text

This gives the project a real application feel while keeping dependencies light.

## How To Evaluate Correctness

Correctness should now be checked at two levels.

### Runtime correctness

Check the console trace for:

- event ordering
- Round Robin particle stepping
- timer interrupt messages
- reset behavior

### Visual correctness

Check the SDL window for:

- successful startup
- responsive buttons and keys
- visible particle bursts
- stronger scatter while wind is on
- HUD state updates
- successful reset
- mouth box appears only during mouth-open task state
- camera highlight appears only during the camera input window
- blue wind ribbons appear only during mic activity
- dandelion sway appears only while particle work is active
- default startup mode is `1T`
- `1T / 2T / 3T` can be switched while keeping task alternation functional

If both levels behave correctly, then the GUI shell and the runtime core are consistent.

## Strengths

- preserves the OS scheduling core
- adds a real interactive shell
- clear event-driven separation
- supports both keyboard and mouse input
- easier to demo than a console-only version

## Limitations

- mouth-open detection still uses OpenCV Haar cascades, not MediaPipe Face Mesh yet
- audio capture uses the system default microphone device selected by SDL
- text rendering is custom and minimal
- preemption is still simulated

The project is still aligned with the original goal because:

- the scheduler remains the source of truth
- thread state transitions still drive execution
- the latest-value buffer still controls low-latency state handoff
- the UI only exposes and visualizes scheduler-driven behavior

## Next Engineering Steps

- replace Haar mouth-open detection with MediaPipe Face Mesh
- improve microphone device selection and threshold tuning
- improve reset flow and UI transitions
- add denser particle burst behavior
- add automated validation of event order and state reset
