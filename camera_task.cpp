#include "thread.h"
#include "scheduler_task_support.h"

#include <algorithm>
#include <memory>
#include <string>

namespace {

void mark_camera_bridge_unavailable_and_stop(const std::string& status_text) {
    CameraBridgeState unavailable_state = runtime_state().camera_bridge;
    unavailable_state.device_unavailable = true;
    unavailable_state.bridge_connected = false;
    unavailable_state.sample_ready = false;
    unavailable_state.status_text = status_text;
    stop_camera_bridge();
    RuntimeState& state_ref = runtime_state();
    state_ref.camera_bridge = unavailable_state;
    state_ref.camera_device_available = false;
    state_ref.camera_available = false;
}

bool camera_soft_interrupt_requested(bool camera_interface_enabled,
                                     bool device_unavailable,
                                     bool device_available,
                                     bool bridge_connected,
                                     bool sample_ready,
                                     bool mouth_detected) {
    const int readiness_score =
        (camera_interface_enabled ? 1 : 0) +
        (device_available ? 1 : 0) +
        (bridge_connected ? 1 : 0) +
        (sample_ready ? 1 : 0) +
        (mouth_detected ? 2 : 0);
    return camera_interface_enabled &&
        !device_unavailable &&
        readiness_score < 5;
}

}  // namespace

CameraTask::CameraTask() {
    type = TaskType::CAMERA;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "CameraTask";
    support_resume = true;
}

void CameraTask::execute() {
    run_cycle(false);
}

void CameraTask::resume() {
    run_cycle(true);
}

void CameraTask::run_cycle(bool resumed) {
    refresh_bridge_inputs();
    ++cycle_count_;
    bool should_cancel_pending_blow_tasks = false;
    bool should_queue_microphone = false;
    bool should_queue_camera_followup = false;
    bool should_stop_unavailable_interface = false;
    std::string unavailable_status_text;
    bool stage_finished = false;
    with_shared_state_write(
        false,
        false,
        true,
        [this,
         resumed,
         &should_cancel_pending_blow_tasks,
         &should_queue_microphone,
         &should_queue_camera_followup,
         &should_stop_unavailable_interface,
         &unavailable_status_text,
         &stage_finished]() {
            RuntimeState& state_ref = runtime_state();
            RenderData& data = render_data();
            const CameraBridgeState& bridge = state_ref.camera_bridge;
            const bool bridge_fresh = scheduler_task_support::camera_bridge_is_fresh(bridge);
            const bool device_available = state_ref.camera_device_available;
            const bool mouth_detected =
                device_available &&
                bridge.bridge_connected &&
                bridge.sample_ready &&
                bridge_fresh &&
                bridge.face_detected &&
                bridge.mouth_open_state &&
                bridge.looking_forward;

            state_ref.camera_available =
                device_available &&
                bridge.bridge_connected &&
                bridge.sample_ready &&
                bridge_fresh &&
                bridge.face_detected;
            if (state_ref.camera_available) {
                state_ref.world.mouth_x = std::clamp(bridge.mouth_center_x, 0.0f, 1.0f);
                state_ref.world.mouth_y = std::clamp(bridge.mouth_center_y, 0.0f, 1.0f);
            } else {
                state_ref.world.mouth_x = state_ref.world.dandelion_x;
                state_ref.world.mouth_y = state_ref.world.dandelion_y;
            }

            data.camera_layer.enabled = current_runtime_state().phase == RuntimePhase::READY;
            data.camera_layer.mouth_detected = mouth_detected;
            if (bridge.device_unavailable) {
                state_ref.world.mouth_x = state_ref.world.dandelion_x;
                state_ref.world.mouth_y = state_ref.world.dandelion_y;
                data.camera_layer.status = "camera-device-unavailable";
                data.ui.camera_task_status = bridge.status_text.empty()
                    ? "CAM device unavailable"
                    : "CAM " + bridge.status_text;
                should_stop_unavailable_interface = true;
                unavailable_status_text = bridge.status_text.empty()
                    ? "camera device unavailable"
                    : bridge.status_text;
                should_queue_microphone = microphone_bridge_enabled();
                stage_finished = true;
            } else if (!device_available) {
                state_ref.camera_gate_open = false;
                state_ref.camera_gate_frame = -1;
                state_ref.camera_gate_until_ms = 0;
                state_ref.camera_focus_locked = false;
                state_ref.microphone_focus_locked = microphone_bridge_enabled();
                state_ref.microphone_focus_until_ms =
                    microphone_bridge_enabled()
                    ? scheduler_task_support::current_time_ms() +
                          scheduler_task_support::kCameraGateHoldMs
                    : 0;
                state_ref.microphone_focus_consumed_for_gate = false;
                data.camera_layer.status = "camera-bridge-waiting";
                data.ui.camera_task_status = camera_bridge_enabled()
                    ? "CAM waiting for device sample"
                    : "CAM interface disabled";
            } else if (data.camera_layer.mouth_detected) {
                state_ref.camera_gate_open = true;
                state_ref.camera_gate_frame = scheduler_task_support::current_scheduler_frame_index();
                state_ref.camera_gate_until_ms =
                    scheduler_task_support::current_time_ms() +
                    scheduler_task_support::kCameraGateHoldMs;
                state_ref.microphone_focus_consumed_for_gate = false;
                state_ref.camera_focus_locked = false;
                state_ref.microphone_focus_locked = true;
                state_ref.microphone_focus_until_ms =
                    scheduler_task_support::current_time_ms() +
                    scheduler_task_support::kCameraGateHoldMs;
                should_queue_microphone = state_ref.microphone_device_available;
                stage_finished = true;
            } else if (state_ref.camera_gate_until_ms > 0 &&
                       scheduler_task_support::current_time_ms() <= state_ref.camera_gate_until_ms &&
                       bridge.bridge_connected &&
                       bridge.sample_ready &&
                       bridge_fresh &&
                       bridge.face_detected) {
                state_ref.camera_gate_open = true;
                state_ref.camera_focus_locked = false;
                if (!state_ref.microphone_focus_consumed_for_gate) {
                    state_ref.microphone_focus_locked = true;
                    state_ref.microphone_focus_until_ms = std::max(
                        state_ref.microphone_focus_until_ms,
                        scheduler_task_support::current_time_ms() +
                            scheduler_task_support::kCameraGateHoldMs);
                }
            } else {
                state_ref.camera_gate_open = false;
                state_ref.camera_gate_frame = -1;
                state_ref.camera_gate_until_ms = 0;
                state_ref.camera_focus_locked = true;
                state_ref.microphone_focus_locked = false;
                state_ref.microphone_focus_until_ms = 0;
                state_ref.microphone_focus_consumed_for_gate = false;
            }
            should_cancel_pending_blow_tasks = !state_ref.camera_gate_open;
            data.camera_layer.mouth_x = state_ref.world.mouth_x;
            data.camera_layer.mouth_y = state_ref.world.mouth_y;
            data.camera_layer.update_tick = cycle_count_;
            if (bridge.device_unavailable) {
                data.camera_layer.status = "camera-device-unavailable";
                data.ui.camera_task_status = bridge.status_text.empty()
                    ? "CAM device unavailable"
                    : "CAM " + bridge.status_text;
            } else if (!device_available) {
                data.camera_layer.status = "camera-bridge-waiting";
                data.ui.camera_task_status = camera_bridge_enabled()
                    ? "CAM waiting for device sample"
                    : "CAM interface disabled";
            } else if (!bridge.bridge_connected) {
                data.camera_layer.status = "camera-bridge-disconnected";
                data.ui.camera_task_status = bridge.status_text.empty()
                    ? "CAM bridge disconnected"
                    : "CAM " + bridge.status_text;
                state_ref.camera_focus_locked = false;
                state_ref.microphone_focus_locked = true;
                state_ref.microphone_focus_until_ms =
                    scheduler_task_support::current_time_ms() +
                    scheduler_task_support::kCameraGateHoldMs;
                state_ref.microphone_focus_consumed_for_gate = false;
            } else if (!bridge.sample_ready) {
                data.camera_layer.status = "camera-bridge-waiting";
                data.ui.camera_task_status = bridge.status_text.empty()
                    ? "CAM waiting for bridge sample"
                    : "CAM " + bridge.status_text;
                state_ref.camera_focus_locked = true;
                state_ref.microphone_focus_locked = false;
                state_ref.microphone_focus_consumed_for_gate = false;
            } else if (!bridge_fresh) {
                data.camera_layer.status = "camera-bridge-stale";
                data.ui.camera_task_status = "CAM bridge sample stale";
            } else if (!bridge.face_detected) {
                data.camera_layer.status = "camera-bridge-ready";
                data.ui.camera_task_status = bridge.status_text.empty()
                    ? "CAM available, waiting for face"
                    : "CAM " + bridge.status_text;
                state_ref.camera_focus_locked = true;
                state_ref.microphone_focus_locked = false;
                state_ref.microphone_focus_consumed_for_gate = false;
            } else if (!bridge.mouth_open_state || !bridge.looking_forward) {
                data.camera_layer.status = "camera-bridge-face-idle";
                data.ui.camera_task_status = bridge.status_text.empty()
                    ? "CAM face tracked, mouth idle"
                    : "CAM " + bridge.status_text;
            } else {
                data.camera_layer.status = resumed ? "camera-bridge-resume" : "camera-bridge-active";
                data.ui.camera_task_status =
                    "CAM bridge active " + bridge.backend +
                    " conf " + std::to_string(static_cast<int>(bridge.confidence * 100.0f));
            }

            should_queue_camera_followup = camera_soft_interrupt_requested(
                camera_bridge_enabled(),
                bridge.device_unavailable,
                device_available,
                bridge.bridge_connected,
                bridge.sample_ready,
                mouth_detected);
            data.ui.input_focus_status = scheduler_task_support::build_input_focus_status(state_ref);
        });

    if (should_stop_unavailable_interface) {
        mark_camera_bridge_unavailable_and_stop(unavailable_status_text);
    }

    if (should_queue_camera_followup) {
        scheduler_task_support::queue_camera_stage_if_needed();
        push_runtime_note("CameraTask: soft-interrupted and requeued to queue tail.");
        state = TaskState::INTERRUPTED;
    } else if (stage_finished) {
        state = TaskState::FINISHED;
    } else {
        state = TaskState::RUNNING;
    }

    if (should_queue_microphone) {
        scheduler_task_support::queue_microphone_stage_if_needed();
    }

    if (should_cancel_pending_blow_tasks) {
        const bool had_generate =
            has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL);
        const bool had_breeze =
            has_queued_task(TaskType::BREEZE, PriorityLevel::P2_FUNCTIONAL);
        scheduler_task_support::erase_p2_tasks_by_type(TaskType::GENERATE_PARTICLE);
        scheduler_task_support::erase_p2_tasks_by_type(TaskType::BREEZE);
        if (had_generate || had_breeze) {
            push_runtime_note("CameraTask: mouth closed, cancelled pending blow tasks.");
        }
    }

    if (stage_finished) {
        push_runtime_note("CameraTask: stage completed.");
    }
}

std::unique_ptr<Task> make_camera_task() {
    return std::make_unique<CameraTask>();
}
