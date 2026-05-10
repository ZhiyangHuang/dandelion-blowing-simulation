#include "thread.h"
#include "scheduler_task_support.h"

#include <algorithm>
#include <memory>
#include <string>

namespace {

void mark_microphone_bridge_unavailable_and_stop(const std::string& status_text) {
    MicrophoneBridgeState unavailable_state = runtime_state().microphone_bridge;
    unavailable_state.device_unavailable = true;
    unavailable_state.bridge_connected = false;
    unavailable_state.sample_ready = false;
    unavailable_state.status_text = status_text;
    stop_microphone_bridge();
    RuntimeState& state_ref = runtime_state();
    state_ref.microphone_bridge = unavailable_state;
    state_ref.microphone_device_available = false;
    state_ref.microphone_available = false;
}

bool microphone_soft_interrupt_requested(bool microphone_interface_enabled,
                                         bool device_unavailable,
                                         bool stage_finished,
                                         bool should_queue_generate,
                                         bool should_queue_breeze) {
    return microphone_interface_enabled &&
        !device_unavailable &&
        !stage_finished &&
        !should_queue_generate &&
        !should_queue_breeze;
}

}  // namespace

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

void MicrophoneTask::run_cycle(bool resumed) {
    refresh_bridge_inputs();
    ++sample_count_;
    bool should_queue_generate = false;
    bool should_queue_breeze = false;
    bool should_queue_microphone_followup = false;
    bool should_stop_unavailable_interface = false;
    std::string unavailable_status_text;
    bool stage_finished = false;
    with_shared_state_write(
        false,
        true,
        true,
        [this,
         resumed,
         &should_queue_generate,
         &should_queue_breeze,
         &should_queue_microphone_followup,
         &should_stop_unavailable_interface,
         &unavailable_status_text,
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
                ? (bridge_fresh ? std::clamp(bridge.suggested_power, 0.1f, 10.0f) : 0.1f)
                : 0.1f;

            data.wind_layer.active = state_ref.microphone_available;
            data.wind_layer.power = state_ref.world.power;
            data.wind_layer.sample_tick = sample_count_;
            if (bridge.device_unavailable) {
                data.wind_layer.status = "mic-device-unavailable";
                data.ui.microphone_task_status = bridge.status_text.empty()
                    ? "MIC device unavailable"
                    : "MIC " + bridge.status_text;
                should_stop_unavailable_interface = true;
                unavailable_status_text = bridge.status_text.empty()
                    ? "microphone device unavailable"
                    : bridge.status_text;
                stage_finished = true;
            } else if (!state_ref.microphone_device_available) {
                data.wind_layer.status = "mic-bridge-waiting";
                data.ui.microphone_task_status = microphone_bridge_enabled()
                    ? "MIC waiting for device sample"
                    : "MIC interface disabled";
            } else if (!bridge.bridge_connected) {
                data.wind_layer.status = "mic-bridge-disconnected";
                data.ui.microphone_task_status = bridge.status_text.empty()
                    ? "MIC bridge disconnected"
                    : "MIC " + bridge.status_text;
            } else if (!bridge.sample_ready) {
                data.wind_layer.status = "mic-bridge-waiting";
                data.ui.microphone_task_status = bridge.status_text.empty()
                    ? "MIC waiting for bridge sample"
                    : "MIC " + bridge.status_text;
            } else if (!input_window_ready) {
                data.wind_layer.status = "mic-ready-waiting-camera";
                data.ui.microphone_task_status = "MIC available, waiting for camera gate";
                stage_finished = true;
            } else if (!bridge_fresh) {
                data.wind_layer.status = "mic-bridge-stale";
                data.ui.microphone_task_status = "MIC bridge sample stale";
            } else if (bridge.fallback_requested) {
                data.wind_layer.status = "mic-bridge-fallback";
                data.ui.microphone_task_status = "MIC requested breeze fallback";
            } else if (!bridge.voice_detected) {
                data.wind_layer.status = "mic-bridge-ready";
                data.ui.microphone_task_status = bridge.status_text.empty()
                    ? "MIC available, waiting for voice"
                    : "MIC " + bridge.status_text;
                stage_finished = true;
            } else {
                data.wind_layer.status = resumed ? "mic-bridge-resume" : "mic-bridge-active";
                data.ui.microphone_task_status =
                    "MIC bridge active " + bridge.backend +
                    " power " +
                    std::to_string(static_cast<int>(state_ref.world.power * 100.0f) / 100.0f);
            }
            data.ui.power = state_ref.world.power;

            should_queue_breeze =
                input_window_ready &&
                bridge.bridge_connected &&
                bridge.sample_ready &&
                bridge_fresh &&
                bridge.fallback_requested &&
                !sample_already_consumed &&
                !has_queued_task(TaskType::BREEZE, PriorityLevel::P2_FUNCTIONAL);
            should_queue_generate =
                input_window_ready &&
                state_ref.microphone_available &&
                !bridge.fallback_requested &&
                !sample_already_consumed &&
                !has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL);

            if ((should_queue_generate || should_queue_breeze) && bridge.timestamp_ms > 0) {
                state_ref.last_consumed_microphone_sample_ms = bridge.timestamp_ms;
                state_ref.microphone_focus_locked = false;
                state_ref.microphone_focus_until_ms = 0;
                state_ref.microphone_focus_consumed_for_gate = true;
                state_ref.camera_focus_locked = false;
                state_ref.camera_gate_open = false;
                state_ref.camera_gate_frame = -1;
                state_ref.camera_gate_until_ms = 0;
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
            should_queue_microphone_followup = microphone_soft_interrupt_requested(
                microphone_bridge_enabled(),
                bridge.device_unavailable,
                stage_finished,
                should_queue_generate,
                should_queue_breeze);
            data.ui.input_focus_status = scheduler_task_support::build_input_focus_status(state_ref);
        });

    if (should_stop_unavailable_interface) {
        mark_microphone_bridge_unavailable_and_stop(unavailable_status_text);
    }

    if (should_queue_generate) {
        submit_task(make_generate_particle_task());
    }
    if (should_queue_breeze) {
        submit_task(make_breeze_task());
    }

    if (should_queue_microphone_followup) {
        scheduler_task_support::queue_microphone_stage_if_needed();
        push_runtime_note("MicrophoneTask: soft-interrupted and requeued to queue tail.");
        state = TaskState::INTERRUPTED;
    } else if (stage_finished) {
        push_runtime_note("MicrophoneTask: stage completed.");
        state = TaskState::FINISHED;
    } else {
        state = TaskState::RUNNING;
    }
}

std::unique_ptr<Task> make_microphone_task() {
    return std::make_unique<MicrophoneTask>();
}
