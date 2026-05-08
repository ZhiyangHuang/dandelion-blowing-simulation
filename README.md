# DandelionOS Scaffold

This repository is now aligned to `DESIGN.md` as a clean scaffold instead of keeping the old mixed implementation.

## What Was Kept

- `DESIGN.md`: source of truth for the new architecture
- `python_mediapipe_bridge.py`: still reusable for the future `CameraTask`
- `run_mediapipe_bridge.bat`: still useful for launching the Python bridge
- `LICENSE` and `.gitignore`

## What Was Reset

The previous runtime, SDL shell, and test flow were no longer consistent with the new design, so they were reduced to a compile-ready skeleton:

- `thread.h`
- `scheduler.cpp`
- `simulation.cpp`
- `events.cpp`
- `render_sdl.cpp`
- `main.cpp`
- `main_logic_test.cpp`
- `main_runtime_test.cpp`

These files now provide:

- design-aligned enums and data structures
- a placeholder task model
- a scheduler scaffold with queue policies for `P1 / P2 / P3`
- a `RenderData` world scaffold
- placeholder visualization hooks
- lightweight smoke tests for the new skeleton

## Current Structure

- `main.cpp`: minimal scaffold entry
- `thread.h`: shared architecture types and APIs
- `scheduler.cpp`: queue policy and dispatch scaffold
- `simulation.cpp`: world-state reset and particle-ring initialization
- `events.cpp`: runtime note buffer
- `render_sdl.cpp`: visualization placeholder
- `visualization_stub.cpp`: test-only visualization stub
- `main_logic_test.cpp`: logic scaffold smoke test
- `main_runtime_test.cpp`: runtime scaffold smoke test
- `python_mediapipe_bridge.py`: future camera bridge integration

## Next Implementation Order

1. Replace placeholder tasks with real `StartTask`, `ResetTask`, and `ExitTask`.
2. Implement `RenderData` ownership and mutex boundaries exactly as defined in `DESIGN.md`.
3. Build the `P1 / P2 / P3` scheduler execution path and resumable preemption model.
4. Add `CameraTask`, `MicrophoneTask`, and particle task chain incrementally.
5. Reintroduce the real renderer only after runtime ownership rules are stable.
