#ifndef SCHEDULER_TASK_SUPPORT_H
#define SCHEDULER_TASK_SUPPORT_H

#include "thread.h"

#include <string>
#include <vector>

namespace scheduler_task_support {

inline constexpr long long kCameraGateHoldMs = 450;
inline constexpr long long kShortListenerLeaseMs = 10000;
inline constexpr long long kShortListenerWarmupMs = 3000;
inline constexpr long long kShortListenerDetectReadyMs = 5000;
inline constexpr int kRrQuantumMs = 16;
inline constexpr float kVelocityPerPowerPixelsPerSecond = 10.0f;
inline constexpr float kNormalizedWorldPixels = 50.0f;

long long current_time_ms();
bool camera_bridge_is_fresh(const CameraBridgeState& bridge);
bool microphone_bridge_is_fresh(const MicrophoneBridgeState& bridge);
bool camera_gate_is_fresh(const RuntimeState& state);
bool microphone_window_is_fresh(const RuntimeState& state);
std::string build_input_focus_status(const RuntimeState& state);
void erase_p2_tasks_by_type(TaskType type);
void queue_camera_stage_if_needed();
void queue_microphone_stage_if_needed();
void queue_batch_stage_if_needed();
void seed_input_entry_task_if_idle();
void seed_mode_specific_short_listener_if_needed(const char* reason);
int current_scheduler_frame_index();
void rebuild_particle_ring_for_world(float center_x, float center_y, int count);
std::vector<int> collect_spawnable_particle_slots(int limit);
void reconcile_particle_bookkeeping(RuntimeState& state_ref, RenderData& data);
void reconcile_runtime_particle_bookkeeping();
bool run_particle_rr_slice_from_batch();

}  // namespace scheduler_task_support

#endif
