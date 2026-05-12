# DandelionOS Design

## 1. Canonical Goal

DandelionOS is a visual operating-systems scheduling project.

Its scheduler does not rank work by business "importance".
It ranks work by maximum acceptable delay.

That delay-budget view is the canonical meaning of priority in this project.

## 2. System Purpose

The runtime turns a human interaction loop into a scheduler-visible pipeline:

1. Camera listener keeps face and mouth state fresh.
2. Camera task detects valid mouth-open state.
3. Microphone listener keeps audio samples fresh.
4. Microphone task classifies blow / fallback intent.
5. Particle-generation tasks create visible dandelion motion.
6. Throughput tasks drain movement and fade work fairly.

The project is deliberately written so scheduler behavior is visible in runtime state,
diagnostics, SDL panels, and verification output.

## 3. Core Ownership Rule

Listeners and tasks are different things.

Listeners:

- own device / process / bridge lifecycle
- own freshness, availability, and latest snapshot state
- may stay alive across many business-task cycles

Tasks:

- consume listener snapshots
- advance the business pipeline
- may yield, requeue, or finish
- must not auto-start or auto-revive listeners

Write ownership:

- `bridge_io.cpp` is the central writer for bridge and listener runtime state
- tasks read bridge snapshots and write scheduler / business state
- renderer only reads render-facing state

## 4. Queue Model

The runtime now uses four physical queues mapped to four latency classes.

| Physical queue | Latency class | Meaning |
| --- | --- | --- |
| `P1_SYSTEM` | `L0 System` | reset, exit, startup, fatal control work |
| `P2_REALTIME` | `L1 Realtime` | listener service / burst listener control work |
| `P2_FUNCTIONAL` | `L2 Interactive` | camera, microphone, generate, breeze, batch feeder |
| `P3_PARTICLE` | `L3 Throughput` | particle move / fade / drain work |

Canonical scheduler precedence:

```text
L0 system
-> L1 long listener
-> L1 short listener
-> L2 interactive consumers
-> L3 throughput
```

Important interpretation rule:

- `P1/P2/P3/P4` are delay-budget buckets
- they are not business-importance labels

## 5. Listener Split

The realtime layer is split into two explicit listener task families.

### 5.1 Long Listener

Long-listener tasks:

- `CameraListenerServiceTask`
- `MicrophoneListenerServiceTask`

Properties:

- multi-thread primary form
- live in `P2_REALTIME`
- higher priority than short listeners
- absorb startup cost once and keep bridge state hot
- may yield internally, but are not ordinary short-lived interactive tasks
- may only be removed by:
  - mode collapse
  - `ResetTask`
  - `ExitTask`
  - explicit listener disable

### 5.2 Short Listener

Short-listener tasks:

- `CameraListenerBurstTask`
- `MicrophoneListenerBurstTask`

Properties:

- single-thread primary form
- constrained `2-thread` fallback form
- still live in `P2_REALTIME`
- lower priority than long listeners, but still above normal `L2`
- designed around a protected short lease rather than permanent residency

Short-listener timing contract:

- total lease target: about `10s`
- startup warmup guidance: about `0s -> 3s`
- stable coordinate-detection guidance: about `3s -> 5s`
- external scheduler paths should not evict a short listener during an active lease
- internal early finish is still allowed

## 6. Thread-Mode Mapping

The current intended runtime shape is:

```text
1 thread
  camera short + microphone short + deterministic business chain

2 threads
  camera long + microphone short + general lane

3 threads
  camera long + microphone long + general lane
```

Camera is preferred as the long-lived listener when threads are scarce because:

- camera startup is slower
- camera is the semantic beginning of the interaction chain
- microphone semantics depend on the camera gate / microphone window handoff

## 7. Pipeline Views

### 7.1 Ownership Tree

```text
HumanBehaviorTask
|
+-- CameraDetectTask
|     `-- MouthOpenEvent
|
+-- BlowDetectTask
|     +-- BlowEvent
|     `-- BreezeFallbackEvent
|
`-- ParticleRootTask
      +-- ParticleGenerateTask
      |     `-- ParticleBatchTask
      |           `-- ParticleMoveTask[]
      |
      `-- ParticleFadeTask
```

`HumanBehaviorTask` and `ParticleRootTask` are currently semantic orchestration
records. They are not executable parent tasks with their own worker ownership.

### 7.2 Control-Flow Loop

```text
Camera listener
-> CameraTask
-> mouth-open gate / microphone window
-> Microphone listener
-> MicrophoneTask
-> GenerateParticleTask or BreezeTask
-> BatchParticleExecutionTask
-> ParticleMove / ParticleFade drain
-> reseed next listener entry
```

### 7.3 Single-Thread Fixed Chain

```text
CameraListenerBurstTask
-> CameraTask
-> MicrophoneListenerBurstTask
-> MicrophoneTask
-> GenerateParticleTask / BreezeTask
-> BatchParticleExecutionTask
-> SingleParticleTask RR
-> idle drain
-> return to CameraListenerBurstTask
```

Single-thread mode is for deterministic ordering, not for maximum throughput.

### 7.4 Multi-Thread Split View

```text
3-thread

  Lane 1: Camera long listener -> camera-side interactive work
  Lane 2: Microphone long listener -> microphone-side interactive work
  Lane 3: General interactive work -> batch feeder -> throughput drain

2-thread

  Lane 1: Camera long listener -> camera-side interactive work
  Lane 2: Microphone short listener + general interactive/throughput work
```

Evaluation policy:

- live runs are locked at startup with `--single`, `--dual`, or `--triple`
- the project no longer depends on runtime thread-mode hot switching for its main demonstration path

## 8. Scheduler Contracts

### 8.1 `L0`

- `StartTask`, `ResetTask`, and `ExitTask` are runtime-wide overrides
- any active worker may claim them before lane-specific interactive work
- once reset or exit executes, peer work in the same frame must stop

### 8.2 `L1`

- queue family: `P2_REALTIME`
- long listeners outrank short listeners inside the realtime queue
- realtime slices may cut across `L2` dispatch in multi-thread mode
- listener queue dedupe keeps at most one resident task per listener type

### 8.3 `L2`

- `CameraTask` and `MicrophoneTask` are one-snapshot business consumers
- `GenerateParticleTask`, `BreezeTask`, and `ChangeDandelionTask` are one-shot
- `BatchParticleExecutionTask` is the only intentional resumable `L2` resident

### 8.4 `L3`

- `SingleParticleTask` and `ParticleFadeTask` drain through the batch feeder
- throughput remains visible and fair through round-robin scheduling

## 9. Listener-to-Task Handoff

### 9.1 Camera Side

Camera listener owns:

- enabled / disabled
- bridge running
- device availability
- sample freshness
- latest mouth snapshot
- listener residency mode

Camera task owns:

- `camera_available`
- mouth-open detection
- camera gate / microphone window opening
- handoff to microphone stage

### 9.2 Microphone Side

Microphone listener owns:

- enabled / disabled
- bridge running
- device availability
- sample freshness
- latest audio snapshot
- listener residency mode

Microphone task owns:

- `microphone_available`
- suggested power
- blow classification
- fallback branch selection
- particle-generation handoff

## 10. Particle Direction Rule

Particle motion must use the mouth snapshot that created the particle work.

Therefore:

- `GenerateParticleTask` snapshots mouth coordinates
- `BreezeTask` snapshots mouth coordinates
- `SingleParticleTask` stores those coordinates in its own payload

This prevents late throughput execution from reading a newer global mouth value
from a later frame.

## 11. Diagnostics and Verification

The runtime is intentionally self-describing.

Current diagnostic surfaces:

- scheduler queue snapshots
- thread / lane status
- `L1 Realtime` diagnostics
- listener heartbeat / seen / seeded / consumed sample state
- single-thread chain diagnostics
- runtime counters for reseed, watchdog recovery, realtime slices, and average drain ticks
- orchestration-flow records
- headless verifier output in `queue_verification_output.txt`

Verification currently covers:

- four-queue precedence
- single-thread full-cycle reseed
- single-thread fallback-cycle reseed
- multi-thread camera and microphone lane partitioning
- `2-thread` camera-long / microphone-short residency
- watchdog recovery after stale listener heartbeat timeout
- scripted replay of `mouth-open -> blow -> overlap -> reset`
- fairness sanity that `L1`, `L2`, and `L3` all make visible progress
- `3 -> 1` collapse with persistent-service hard stop and burst reseed
- reset / exit preemption behavior
- realtime slices cutting across `L2`
- short microphone lease not auto-renewing after expiration in `2-thread`

## 12. Implemented Status

Implemented and considered landed:

- four-queue model with explicit `P2_REALTIME`
- semantic orchestration records for `HumanBehaviorTask` and `ParticleRootTask`
- `ParticleFadeTask` split from move work
- explicit long-listener / short-listener task families
- short-listener lease timing state in runtime diagnostics
- `2-thread` camera-long / microphone-short preference
- `3-thread` camera-long / microphone-long / general-lane split
- mode-collapse hard stop of persistent listeners
- mode-expansion reseed of mode-specific listeners
- watchdog-based stale-listener recovery
- runtime counters for reseed / watchdog / realtime slices / average drain ticks

Still active as follow-through work:

- narrow `CameraTask` / `MicrophoneTask` further into pure one-snapshot `L2` consumers
- continue moving reseed ownership from scheduler fallback paths into `L1`
- preserve current verification coverage while tightening `L1 -> L2` ownership boundaries

## 13. Non-Negotiable Rules

- No task may internally revive its listener.
- Listener start/stop ownership stays outside normal business tasks.
- Bridge writes stay centralized in `bridge_io.cpp`.
- Single-thread mode preserves fixed-phase order.
- Multi-thread mode preserves latency-aware partitioning.
- Camera gate / microphone window validity must not be bypassed for throughput convenience.
- Long-listener and short-listener priority must remain visibly distinct in code and diagnostics.
