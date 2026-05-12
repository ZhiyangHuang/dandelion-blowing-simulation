#include "thread.h"
#include "runtime_orchestration.h"
#include "scheduler_task_support.h"

#include <algorithm>
#include <memory>
#include <string>
namespace {

constexpr float kBaselineMicrophonePower = 0.1f;

}  // namespace

MicrophoneListenerServiceTask::MicrophoneListenerServiceTask() {
    type = TaskType::MICROPHONE_LISTENER_SERVICE;
    priority = PriorityLevel::P2_REALTIME;
    name = "MicrophoneListenerServiceTask";
    support_resume = true;
}

void MicrophoneListenerServiceTask::execute() {
    run_cycle(false);
}

void MicrophoneListenerServiceTask::resume() {
    run_cycle(true);
}

std::unique_ptr<Task> MicrophoneListenerServiceTask::clone_for_requeue() const {
    return std::make_unique<MicrophoneListenerServiceTask>(*this);
}

void MicrophoneListenerServiceTask::run_cycle(bool resumed) {
    (void)resumed;
    if (microphone_bridge_enabled()) {
        refresh_bridge_inputs();
    }

    bool keep_alive = false;
    bool should_queue_microphone = false;
    with_shared_state_write(false, false, true, [&keep_alive, &should_queue_microphone]() {
        RuntimeState& state_ref = runtime_state();
        keep_alive =
            state_ref.phase == RuntimePhase::READY &&
            state_ref.microphone_listener.enabled &&
            (current_thread_mode() >= 3 ||
             (current_thread_mode() == 2 && !state_ref.camera_listener.enabled));
        if (!keep_alive) {
            return;
        }

        const bool consumer_missing =
            !has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL);
        const bool input_window_ready =
            !state_ref.camera_device_available ||
            scheduler_task_support::camera_gate_is_fresh(state_ref) ||
            scheduler_task_support::microphone_window_is_fresh(state_ref);
        const bool fresh_microphone_sample =
            state_ref.microphone_listener.sample_ready &&
            state_ref.microphone_bridge.bridge_connected &&
            scheduler_task_support::microphone_bridge_is_fresh(
                state_ref.microphone_bridge);
        const bool unconsumed_sample =
            state_ref.microphone_bridge.timestamp_ms > 0 &&
            state_ref.microphone_bridge.timestamp_ms !=
                state_ref.last_consumed_microphone_sample_ms;

        should_queue_microphone =
            consumer_missing &&
            input_window_ready &&
            fresh_microphone_sample &&
            unconsumed_sample;
    });

    if (should_queue_microphone) {
        scheduler_task_support::queue_microphone_stage_if_needed();
    }

    state = keep_alive ? TaskState::RUNNING : TaskState::FINISHED;
}

MicrophoneListenerBurstTask::MicrophoneListenerBurstTask() {
    type = TaskType::MICROPHONE_LISTENER_BURST;
    priority = PriorityLevel::P2_REALTIME;
    name = "MicrophoneListenerBurstTask";
    support_resume = true;
}

void MicrophoneListenerBurstTask::execute() {
    run_cycle(false);
}

void MicrophoneListenerBurstTask::resume() {
    run_cycle(true);
}

std::unique_ptr<Task> MicrophoneListenerBurstTask::clone_for_requeue() const {
    return std::make_unique<MicrophoneListenerBurstTask>(*this);
}

void MicrophoneListenerBurstTask::run_cycle(bool resumed) {
    (void)resumed;
    if (microphone_bridge_enabled()) {
        refresh_bridge_inputs();
    }

    bool keep_alive = false;
    bool should_queue_microphone = false;
    with_shared_state_write(false, false, true, [&keep_alive, &should_queue_microphone]() {
        RuntimeState& state_ref = runtime_state();
        const bool short_role_active =
            (current_thread_mode() == 1 && state_ref.microphone_listener.enabled) ||
            (current_thread_mode() == 2 &&
             state_ref.camera_listener.enabled &&
             state_ref.microphone_listener.enabled);
        const bool lease_active =
            state_ref.microphone_listener.short_lease_active &&
            state_ref.microphone_listener.short_lease_until_ms >
                scheduler_task_support::current_time_ms();
        keep_alive =
            short_role_active &&
            state_ref.phase == RuntimePhase::READY &&
            lease_active;
        if (!keep_alive) {
            if (state_ref.microphone_listener.short_lease_active && !lease_active) {
                state_ref.microphone_listener.short_lease_active = false;
                state_ref.microphone_listener.short_lease_started_at_ms = 0;
                state_ref.microphone_listener.short_lease_until_ms = 0;
                state_ref.microphone_listener.short_warmup_until_ms = 0;
                state_ref.microphone_listener.short_detect_ready_at_ms = 0;
            }
            return;
        }

        const bool consumer_missing =
            !has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL);
        const bool input_window_ready =
            !state_ref.camera_device_available ||
            scheduler_task_support::camera_gate_is_fresh(state_ref) ||
            scheduler_task_support::microphone_window_is_fresh(state_ref);
        const bool fresh_microphone_sample =
            state_ref.microphone_listener.sample_ready &&
            state_ref.microphone_bridge.bridge_connected &&
            scheduler_task_support::microphone_bridge_is_fresh(
                state_ref.microphone_bridge);
        const bool unconsumed_sample =
            state_ref.microphone_bridge.timestamp_ms > 0 &&
            state_ref.microphone_bridge.timestamp_ms !=
                state_ref.last_consumed_microphone_sample_ms;

        should_queue_microphone =
            consumer_missing &&
            input_window_ready &&
            fresh_microphone_sample &&
            unconsumed_sample;
    });

    if (should_queue_microphone) {
        scheduler_task_support::queue_microphone_stage_if_needed();
    }

    state = keep_alive ? TaskState::RUNNING : TaskState::FINISHED;
}

MicrophoneTask::MicrophoneTask() {
    type = TaskType::MICROPHONE;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "MicrophoneTask";
    support_resume = true;
}

void MicrophoneTask::execute() {
    run_cycle(false);
}

void MicrophoneTask::resume() {
    run_cycle(true);
}

std::unique_ptr<Task> MicrophoneTask::clone_for_requeue() const {
    return std::make_unique<MicrophoneTask>(*this);
}

void MicrophoneTask::run_cycle(bool resumed) {
    if (microphone_bridge_enabled()) {
        refresh_bridge_inputs();
    }
    ++sample_count_;
    const bool allow_parallel_particle_handoff = current_thread_mode() > 1;
    bool should_queue_generate = false;
    bool should_queue_breeze = false;
    bool stage_finished = false;
    with_shared_state_write(
        false,
        true,
        true,
        [this,
         resumed,
         allow_parallel_particle_handoff,
         &should_queue_generate,
         &should_queue_breeze,
         &stage_finished]() {
            RuntimeState& state_ref = runtime_state();
            RenderData& data = render_data();
            const MicrophoneBridgeState& bridge = state_ref.microphone_bridge;
            const bool camera_gate_ready = scheduler_task_support::camera_gate_is_fresh(state_ref);
            const bool microphone_focus_ready =
                state_ref.microphone_focus_locked &&
                state_ref.microphone_focus_until_ms > 0 &&
                scheduler_task_support::current_time_ms() <= state_ref.microphone_focus_until_ms;
            const bool input_window_ready =
                !state_ref.camera_device_available || camera_gate_ready || microphone_focus_ready;
            const bool bridge_fresh = scheduler_task_support::microphone_bridge_is_fresh(bridge);
            const bool sample_already_consumed =
                bridge.timestamp_ms > 0 &&
                bridge.timestamp_ms == state_ref.last_consumed_microphone_sample_ms;

            state_ref.microphone_available =
                state_ref.microphone_device_available &&
                input_window_ready &&
                bridge.bridge_connected &&
                bridge.sample_ready &&
                bridge_fresh &&
                bridge.voice_detected;
            state_ref.world.power = input_window_ready
                ? (bridge_fresh ? std::clamp(bridge.suggested_power, kBaselineMicrophonePower, 10.0f)
                                : kBaselineMicrophonePower)
                : kBaselineMicrophonePower;
            const bool baseline_power =
                state_ref.world.power <= (kBaselineMicrophonePower + 0.0001f);

            data.wind_layer.active = state_ref.microphone_available;
            data.wind_layer.power = state_ref.world.power;
            data.wind_layer.sample_tick = sample_count_;
            if (bridge.device_unavailable) {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::MICROPHONE,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::ERROR_STATE,
                    OrchestrationEvent::MICROPHONE_DEVICE_UNAVAILABLE,
                    false);
                data.wind_layer.status = "mic-device-unavailable";
                data.ui.microphone_task_status = bridge.status_text.empty()
                    ? "MIC device unavailable"
                    : "MIC " + bridge.status_text;
                stage_finished = true;
            } else if (!state_ref.microphone_device_available) {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::MICROPHONE,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::WAITING,
                    OrchestrationEvent::MICROPHONE_DEVICE_WAIT,
                    true);
                data.wind_layer.status = "mic-bridge-waiting";
                data.ui.microphone_task_status = state_ref.microphone_listener.enabled
                    ? "MIC waiting for device sample"
                    : "MIC interface disabled";
                stage_finished = true;
            } else if (!bridge.bridge_connected) {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::MICROPHONE,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::BLOCKED,
                    OrchestrationEvent::MICROPHONE_BRIDGE_DISCONNECTED,
                    true);
                data.wind_layer.status = "mic-bridge-disconnected";
                data.ui.microphone_task_status = bridge.status_text.empty()
                    ? "MIC bridge disconnected"
                    : "MIC " + bridge.status_text;
                stage_finished = true;
            } else if (!bridge.sample_ready) {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::MICROPHONE,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::WAITING,
                    OrchestrationEvent::MICROPHONE_SAMPLE_WAIT,
                    true);
                data.wind_layer.status = "mic-bridge-waiting";
                data.ui.microphone_task_status = bridge.status_text.empty()
                    ? "MIC waiting for bridge sample"
                    : "MIC " + bridge.status_text;
                stage_finished = true;
            } else if (!input_window_ready) {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::MICROPHONE,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::BLOCKED,
                    OrchestrationEvent::MICROPHONE_GATE_WAIT,
                    true);
                data.wind_layer.status = "mic-ready-waiting-camera";
                data.ui.microphone_task_status = "MIC available, waiting for camera gate";
                stage_finished = true;
            } else if (!bridge_fresh) {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::MICROPHONE,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::BLOCKED,
                    OrchestrationEvent::MICROPHONE_SAMPLE_STALE,
                    true);
                data.wind_layer.status = "mic-bridge-stale";
                data.ui.microphone_task_status = "MIC bridge sample stale";
                stage_finished = true;
            } else if (bridge.fallback_requested) {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::MICROPHONE,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::FALLBACK,
                    OrchestrationEvent::BLOW_FALLBACK,
                    false);
                data.wind_layer.status = "mic-bridge-fallback";
                data.ui.microphone_task_status = "MIC requested breeze fallback";
                stage_finished = true;
            } else if (!bridge.voice_detected) {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::MICROPHONE,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::WAITING,
                    OrchestrationEvent::VOICE_WAIT,
                    true);
                data.wind_layer.status = "mic-bridge-ready";
                data.ui.microphone_task_status = bridge.status_text.empty()
                    ? "MIC available, waiting for voice"
                    : "MIC " + bridge.status_text;
                (void)baseline_power;
                stage_finished = true;
            } else {
                runtime_orchestration::advance_human_behavior_flow(
                    state_ref,
                    TaskType::MICROPHONE,
                    id,
                    OrchestrationNode::BLOW_DETECT,
                    OrchestrationStatus::HANDOFF,
                    OrchestrationEvent::BLOW_DETECTED,
                    false);
                data.wind_layer.status = resumed ? "mic-bridge-resume" : "mic-bridge-active";
                data.ui.microphone_task_status =
                    "MIC bridge active " + bridge.backend +
                    " power " +
                    std::to_string(static_cast<int>(state_ref.world.power * 100.0f) / 100.0f);
                stage_finished = true;
            }
            data.ui.power = state_ref.world.power;

            should_queue_breeze =
                input_window_ready &&
                bridge.bridge_connected &&
                bridge.sample_ready &&
                bridge_fresh &&
                bridge.fallback_requested &&
                !sample_already_consumed &&
                (allow_parallel_particle_handoff ||
                 !has_queued_task(TaskType::BREEZE, PriorityLevel::P2_FUNCTIONAL));
            should_queue_generate =
                input_window_ready &&
                state_ref.microphone_available &&
                !bridge.fallback_requested &&
                !sample_already_consumed &&
                (allow_parallel_particle_handoff ||
                 !has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL));

            if ((should_queue_generate || should_queue_breeze) && bridge.timestamp_ms > 0) {
                state_ref.last_consumed_microphone_sample_ms = bridge.timestamp_ms;
                state_ref.microphone_listener.last_consumed_sample_ms = bridge.timestamp_ms;
                state_ref.camera_focus_locked = false;
                if (allow_parallel_particle_handoff) {
                    state_ref.microphone_focus_locked = true;
                    state_ref.microphone_focus_until_ms = std::max(
                        state_ref.microphone_focus_until_ms,
                        scheduler_task_support::current_time_ms() +
                            scheduler_task_support::kCameraGateHoldMs);
                    state_ref.microphone_focus_consumed_for_gate = true;
                } else {
                    state_ref.microphone_focus_locked = false;
                    state_ref.microphone_focus_until_ms = 0;
                    state_ref.microphone_focus_consumed_for_gate = true;
                    state_ref.camera_gate_open = false;
                    state_ref.camera_gate_frame = -1;
                    state_ref.camera_gate_until_ms = 0;
                }
                stage_finished = true;
            } else if (microphone_focus_ready) {
                state_ref.camera_focus_locked = false;
                state_ref.microphone_focus_locked = true;
            } else if (!input_window_ready) {
                state_ref.microphone_focus_locked = false;
                state_ref.microphone_focus_until_ms = 0;
                state_ref.microphone_focus_consumed_for_gate = false;
                state_ref.camera_focus_locked = true;
            } else {
                state_ref.camera_focus_locked = false;
                state_ref.microphone_focus_locked = true;
                state_ref.microphone_focus_until_ms =
                    scheduler_task_support::current_time_ms() +
                    scheduler_task_support::kCameraGateHoldMs;
            }
            data.ui.input_focus_status = scheduler_task_support::build_input_focus_status(state_ref);
        });

    if (should_queue_generate) {
        submit_task(make_generate_particle_task());
    }
    if (should_queue_breeze) {
        submit_task(make_breeze_task());
    }

    if (stage_finished) {
        push_runtime_note("MicrophoneTask: stage completed.");
        state = TaskState::FINISHED;
    } else {
        state = TaskState::RUNNING;
    }
}

std::unique_ptr<Task> make_microphone_task() {
    return std::make_unique<MicrophoneTask>();
}

std::unique_ptr<Task> make_microphone_listener_service_task() {
    return std::make_unique<MicrophoneListenerServiceTask>();
}

std::unique_ptr<Task> make_microphone_listener_burst_task() {
    return std::make_unique<MicrophoneListenerBurstTask>();
}
