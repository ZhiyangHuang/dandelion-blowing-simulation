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
- initial real system tasks for startup, reset, and shutdown
- a scheduler scaffold with queue policies for `P1 / P2 / P3`
- a `RenderData` world scaffold
- shared runtime-state scaffolding
- ordered shared-state write scaffolding for `particle -> power -> render`
- resident task scaffolds for camera, microphone, and batch world ticking
- a `GenerateParticleTask` scaffold that creates P3 task records
- a resumable `SingleParticleTask` skeleton that advances across multiple ticks
- bridge adapters that load `camera_bridge_latest.json` and `microphone_bridge_latest.json`
- placeholder visualization hooks
- lightweight smoke tests for the new skeleton

## Current Structure

- `main.cpp`: minimal scaffold entry
- `thread.h`: shared architecture types and APIs
- `scheduler.cpp`: queue policy and dispatch scaffold
- `simulation.cpp`: world-state reset and particle-ring initialization
- `events.cpp`: runtime note buffer
- `PROJECT_PROGRESS.txt`: step-by-step implementation checklist
- `render_sdl.cpp`: visualization placeholder
- `visualization_stub.cpp`: test-only visualization stub
- `main_logic_test.cpp`: logic scaffold smoke test
- `main_runtime_test.cpp`: runtime scaffold smoke test
- `python_mediapipe_bridge.py`: future camera bridge integration

## Bridge Files

- `camera_bridge_latest.json`: consumed by `refresh_bridge_inputs()` and mapped into `CameraBridgeState`
- `microphone_bridge_latest.json`: consumed by `refresh_bridge_inputs()` and mapped into `MicrophoneBridgeState`

Expected camera JSON fields:
- `timestamp_ms`
- `backend`
- `face_detected`
- `mouth_open_state`
- `mouth_open_ratio`
- `mouth_center_x`
- `mouth_center_y`
- `confidence`
- `jaw_open_score`
- `looking_forward`

Expected microphone JSON fields:
- `timestamp_ms`
- `backend`
- `voice_detected`
- `fallback_requested`
- `suggested_power`
- `direction_x`
- `direction_y`
- `confidence`

## Next Implementation Order

1. Build stronger interrupt, requeue, and resume bookkeeping in the scheduler.
2. Make P2/P3 interaction closer to DESIGN.md preemption behavior.
3. Add fuller particle lifecycle rules such as border stop and cleanup.
4. Let resident tasks respond to mode changes and reset more explicitly.
5. Reintroduce the real renderer only after runtime ownership rules are stable.
