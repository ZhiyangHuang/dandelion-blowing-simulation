#include "thread.h"
#include "runtime_orchestration.h"
#include "scheduler_task_support.h"

#include <algorithm>
#include <memory>
#include <string>

CameraListenerServiceTask::CameraListenerServiceTask() {
    type = TaskType::CAMERA_LISTENER_SERVICE;
    priority = PriorityLevel::P2_REALTIME;
    name = "CameraListenerServiceTask";
    support_resume = true;
}

void CameraListenerServiceTask::execute() {
    run_cycle(false);
}

void CameraListenerServiceTask::resume() {
    run_cycle(true);
}

std::unique_ptr<Task> CameraListenerServiceTask::clone_for_requeue() const {
    return std::make_unique<CameraListenerServiceTask>(*this);
}

void CameraListenerServiceTask::run_cycle(bool resumed) {
    (void)resumed;
    if (camera_bridge_enabled()) {
        refresh_bridge_inputs();
    }

    bool keep_alive = false;
    bool should_queue_camera = false;
    with_shared_state_write(false, false, true, [&keep_alive, &should_queue_camera]() {
        RuntimeState& state_ref = runtime_state();
        keep_alive =
            state_ref.phase == RuntimePhase::READY &&
            state_ref.camera_listener.enabled &&
            current_thread_mode() >= 2;
        if (!keep_alive) {
            return;
        }

        const bool consumer_missing =
            !has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL);
        const bool fresh_sample_seen =
            state_ref.camera_listener.last_seen_sample_ms > 0 &&
            state_ref.camera_listener.last_seen_sample_ms !=
                state_ref.camera_listener.last_seeded_sample_ms;
        const bool needs_scan_cycle =
            state_ref.camera_focus_locked ||
            !state_ref.camera_available ||
            !state_ref.camera_listener.sample_ready;

        should_queue_camera = consumer_missing && (fresh_sample_seen || needs_scan_cycle);
    });

    if (should_queue_camera) {
        scheduler_task_support::queue_camera_stage_if_needed();
    }

    state = keep_alive ? TaskState::RUNNING : TaskState::FINISHED;
}

CameraListenerBurstTask::CameraListenerBurstTask() {
    type = TaskType::CAMERA_LISTENER_BURST;
    priority = PriorityLevel::P2_REALTIME;
    name = "CameraListenerBurstTask";
    support_resume = true;
}

void CameraListenerBurstTask::execute() {
    run_cycle(false);
}

void CameraListenerBurstTask::resume() {
    run_cycle(true);
}

std::unique_ptr<Task> CameraListenerBurstTask::clone_for_requeue() const {
    return std::make_unique<CameraListenerBurstTask>(*this);
}

void CameraListenerBurstTask::run_cycle(bool resumed) {
    (void)resumed;
    if (camera_bridge_enabled()) {
        refresh_bridge_inputs();
    }

    bool keep_alive = false;
    bool should_queue_camera = false;
    with_shared_state_write(false, false, true, [&keep_alive, &should_queue_camera]() {
        RuntimeState& state_ref = runtime_state();
        const bool short_role_active =
            current_thread_mode() == 1 &&
            state_ref.camera_listener.enabled;
        const bool lease_active =
            state_ref.camera_listener.short_lease_active &&
            state_ref.camera_listener.short_lease_until_ms >
                scheduler_task_support::current_time_ms();
        keep_alive =
            short_role_active &&
            state_ref.phase == RuntimePhase::READY &&
            lease_active;
        if (!keep_alive) {
            if (state_ref.camera_listener.short_lease_active && !lease_active) {
                state_ref.camera_listener.short_lease_active = false;
                state_ref.camera_listener.short_lease_started_at_ms = 0;
                state_ref.camera_listener.short_lease_until_ms = 0;
                state_ref.camera_listener.short_warmup_until_ms = 0;
                state_ref.camera_listener.short_detect_ready_at_ms = 0;
            }
            return;
        }

        const bool consumer_missing =
            !has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL);
        const bool fresh_sample_seen =
            state_ref.camera_listener.last_seen_sample_ms > 0 &&
            state_ref.camera_listener.last_seen_sample_ms !=
                state_ref.camera_listener.last_seeded_sample_ms;
        const bool needs_scan_cycle =
            state_ref.camera_focus_locked ||
            !state_ref.camera_available ||
            !state_ref.camera_listener.sample_ready;

        should_queue_camera = consumer_missing && (fresh_sample_seen || needs_scan_cycle);
    });

    if (should_queue_camera) {
        scheduler_task_support::queue_camera_stage_if_needed();
    }

    state = keep_alive ? TaskState::RUNNING : TaskState::FINISHED;
}

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

std::unique_ptr<Task> CameraTask::clone_for_requeue() const {
    return std::make_unique<CameraTask>(*this);
}

void CameraTask::run_cycle(bool resumed) {
    if (camera_bridge_enabled()) {
        refresh_bridge_inputs();
    }
    ++cycle_count_;
    const bool allow_microphone_handoff = current_thread_mode() > 1;
    bool should_cancel_pending_blow_tasks = false;
    bool should_queue_microphone = false;
    bool stage_finished = false;
    with_shared_state_write(
        false,
        false,
        true,
        [this,
         resumed,
         allow_microphone_handoff,
         &should_cancel_pending_blow_tasks,
         &should_queue_microphone,
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
            const bool camera_sample_available =
                device_available &&
                bridge.bridge_connected &&
                bridge.sample_ready &&
                bridge_fresh &&
                bridge.face_detected;

            if (camera_sample_available) {
                state_ref.world.mouth_x = std::clamp(bridge.mouth_center_x, 0.0f, 1.0f);
                state_ref.world.mouth_y = std::clamp(bridge.mouth_center_y, 0.0f, 1.0f);
                if (bridge.timestamp_ms > 0) {
                    state_ref.camera_listener.last_consumed_sample_ms = bridge.timestamp_ms;
                }
            } else {
                state_ref.world.mouth_x = state_ref.world.dandelion_x;
                state_ref.world.mouth_y = state_ref.world.dandelion_y;
            }

            data.camera_layer.enabled = current_runtime_state().phase == RuntimePhase::READY;
            data.camera_layer.mouth_detected = mouth_detected;
            if (bridge.device_unavailable) {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::CAMERA,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::FALLBACK,
                    OrchestrationEvent::CAMERA_DEVICE_UNAVAILABLE,
                    state_ref.microphone_listener.enabled);
                state_ref.world.mouth_x = state_ref.world.dandelion_x;
                state_ref.world.mouth_y = state_ref.world.dandelion_y;
                data.camera_layer.status = "camera-device-unavailable";
                data.ui.camera_task_status = bridge.status_text.empty()
                    ? "CAM device unavailable"
                    : "CAM " + bridge.status_text;
                should_queue_microphone =
                    allow_microphone_handoff && state_ref.microphone_listener.enabled;
                stage_finished = true;
            } else if (!device_available) {
                state_ref.camera_gate_open = false;
                state_ref.camera_gate_frame = -1;
                state_ref.camera_gate_until_ms = 0;
                state_ref.camera_focus_locked = true;
                state_ref.microphone_focus_locked = false;
                state_ref.microphone_focus_until_ms = 0;
                state_ref.microphone_focus_consumed_for_gate = false;
                data.camera_layer.status = "camera-bridge-waiting";
                data.ui.camera_task_status = state_ref.camera_listener.enabled
                    ? "CAM waiting for device sample"
                    : "CAM interface disabled";
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::CAMERA,
                    id,
                    OrchestrationNode::CAMERA_DETECT,
                    OrchestrationStatus::WAITING,
                    OrchestrationEvent::CAMERA_DEVICE_WAIT,
                    true);
                stage_finished = true;
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
                should_queue_microphone = state_ref.microphone_listener.enabled;
                stage_finished = true;
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::CAMERA,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::HANDOFF,
                    OrchestrationEvent::MOUTH_OPEN,
                    state_ref.microphone_listener.enabled);
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
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::CAMERA,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::BLOCKED,
                    OrchestrationEvent::CAMERA_GATE_HOLD,
                    true);
                stage_finished = true;
            } else {
                state_ref.camera_gate_open = false;
                state_ref.camera_gate_frame = -1;
                state_ref.camera_gate_until_ms = 0;
                state_ref.camera_focus_locked = true;
                state_ref.microphone_focus_locked = false;
                state_ref.microphone_focus_until_ms = 0;
                state_ref.microphone_focus_consumed_for_gate = false;
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::CAMERA,
                    id,
                    OrchestrationNode::CAMERA_DETECT,
                    OrchestrationStatus::ACTIVE,
                    OrchestrationEvent::CAMERA_MOUTH_WAIT,
                    true);
                stage_finished = true;
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
                data.ui.camera_task_status = state_ref.camera_listener.enabled
                    ? "CAM waiting for device sample"
                    : "CAM interface disabled";
            } else if (!bridge.bridge_connected) {
                data.camera_layer.status = "camera-bridge-disconnected";
                data.ui.camera_task_status = bridge.status_text.empty()
                    ? "CAM bridge disconnected"
                    : "CAM " + bridge.status_text;
                state_ref.camera_focus_locked = true;
                state_ref.microphone_focus_locked = false;
                state_ref.microphone_focus_until_ms = 0;
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
                    ? "CAM device ready, waiting for face"
                    : "CAM " + bridge.status_text;
                state_ref.camera_focus_locked = true;
                state_ref.microphone_focus_locked = false;
                state_ref.microphone_focus_consumed_for_gate = false;
            } else if (!bridge.mouth_open_state || !bridge.looking_forward) {
                data.camera_layer.status = "camera-bridge-face-idle";
                data.ui.camera_task_status = bridge.status_text.empty()
                    ? "CAM ready, waiting for first mouth"
                    : "CAM " + bridge.status_text;
            } else {
                data.camera_layer.status = resumed ? "camera-bridge-resume" : "camera-bridge-active";
                data.ui.camera_task_status =
                    "CAM bridge active " + bridge.backend +
                    " conf " + std::to_string(static_cast<int>(bridge.confidence * 100.0f));
            }
            data.ui.input_focus_status = scheduler_task_support::build_input_focus_status(state_ref);
        });

    if (stage_finished) {
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

std::unique_ptr<Task> make_camera_listener_service_task() {
    return std::make_unique<CameraListenerServiceTask>();
}

std::unique_ptr<Task> make_camera_listener_burst_task() {
    return std::make_unique<CameraListenerBurstTask>();
}
