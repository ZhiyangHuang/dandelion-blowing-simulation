#include "thread.h"

#include <cassert>
#include <chrono>
#include <iostream>

namespace {

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

long long next_timestamp_after(long long minimum_ms) {
    long long candidate = now_ms();
    while (candidate <= minimum_ms) {
        candidate = now_ms();
    }
    return candidate;
}

void set_camera_bridge_sample(bool face_detected,
                              bool mouth_open,
                              bool looking_forward,
                              float mouth_x = 0.5f,
                              float mouth_y = 0.5f,
                              float confidence = 0.9f,
                              long long timestamp_ms = 0) {
    RuntimeState& state_ref = runtime_state();
    state_ref.camera_device_available = true;
    state_ref.camera_bridge.bridge_connected = true;
    state_ref.camera_bridge.sample_ready = true;
    state_ref.camera_bridge.face_detected = face_detected;
    state_ref.camera_bridge.mouth_open_state = mouth_open;
    state_ref.camera_bridge.looking_forward = looking_forward;
    state_ref.camera_bridge.mouth_center_x = mouth_x;
    state_ref.camera_bridge.mouth_center_y = mouth_y;
    state_ref.camera_bridge.confidence = confidence;
    state_ref.camera_bridge.timestamp_ms = timestamp_ms > 0 ? timestamp_ms : now_ms();
    state_ref.camera_bridge.backend = "test-camera";
}

void set_microphone_bridge_sample(bool voice_detected,
                                  float suggested_power,
                                  bool fallback_requested = false,
                                  float confidence = 0.8f,
                                  long long timestamp_ms = 0) {
    RuntimeState& state_ref = runtime_state();
    state_ref.microphone_device_available = true;
    state_ref.microphone_bridge.bridge_connected = true;
    state_ref.microphone_bridge.sample_ready = true;
    state_ref.microphone_bridge.voice_detected = voice_detected;
    state_ref.microphone_bridge.fallback_requested = fallback_requested;
    state_ref.microphone_bridge.suggested_power = suggested_power;
    state_ref.microphone_bridge.confidence = confidence;
    state_ref.microphone_bridge.timestamp_ms = timestamp_ms > 0 ? timestamp_ms : now_ms();
    state_ref.microphone_bridge.backend = "test-microphone";
}

void tick_until_particle_terminal(int slot, int max_ticks) {
    for (int attempt = 0; attempt < max_ticks; ++attempt) {
        const std::string& status = current_render_data().particles[slot].status;
        if (status == "particle-boundary-stop" || status == "particle-cleaned") {
            return;
        }
        scheduler_tick();
    }
    assert(false && "timed out waiting for particle terminal state");
}

}  // namespace

int main() {
    bootstrap_runtime();
    assert(current_thread_mode() == 1);
    assert(runtime_threads().size() == 3);
    assert(current_render_data().particles.size() == 100);
    assert(current_render_data().ui.remaining_particles == 100);
    assert(current_runtime_state().phase == RuntimePhase::BOOTSTRAP);

    seed_startup_flow();
    scheduler_tick();
    scheduler_tick();
    assert(current_runtime_state().phase == RuntimePhase::READY);

    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    const long long camera_open_ts = now_ms();
    set_camera_bridge_sample(true, true, true, 0.61f, 0.39f, 0.9f, camera_open_ts);
    set_microphone_bridge_sample(true, 0.25f, false, 0.8f, next_timestamp_after(camera_open_ts));

    submit_task(make_camera_task());
    scheduler_tick();
    SchedulerSnapshot after_camera = scheduler_snapshot();
    assert(!has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));
    assert(after_camera.threads[0].last_task_event == "finished");
    assert(after_camera.threads[0].last_completed_task_name.find("CameraTask#") != std::string::npos);

    scheduler_tick();
    SchedulerSnapshot after_microphone = scheduler_snapshot();
    assert(!has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL));
    assert(after_microphone.threads[0].last_task_event == "finished");
    assert(after_microphone.threads[0].last_completed_task_name.find("MicrophoneTask#") != std::string::npos);

    scheduler_tick();
    SchedulerSnapshot after_generate = scheduler_snapshot();
    assert(has_queued_task(TaskType::BATCH_PARTICLE_EXECUTION, PriorityLevel::P2_FUNCTIONAL));
    assert(!after_generate.p3_queue.empty());
    assert(current_render_data().ui.queued_particle_tasks > 0);

    tick_until_particle_terminal(0, 512);
    for (int attempt = 0; attempt < 512; ++attempt) {
        SchedulerSnapshot snapshot = scheduler_snapshot();
        if (snapshot.p2_queue.empty() && snapshot.p3_queue.empty() &&
            current_render_data().ui.queued_particle_tasks == 0) {
            break;
        }
        scheduler_tick();
    }
    SchedulerSnapshot drained = scheduler_snapshot();
    assert(drained.p2_queue.empty());
    assert(drained.p3_queue.empty());
    assert(current_render_data().ui.queued_particle_tasks == 0);

    scheduler_tick();
    assert(!scheduler_snapshot().p2_queue.empty() ||
           runtime_threads()[0].last_completed_task_name.find("CameraTask#") != std::string::npos);

    reset_runtime();
    set_runtime_phase(RuntimePhase::READY);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    runtime_state().camera_device_available = false;
    runtime_state().microphone_device_available = true;
    scheduler_tick();
    assert(!has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));

    std::cout << "main_logic_test passed\n";
    return 0;
}
