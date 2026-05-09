#include "thread.h"

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

long long next_timestamp_after(long long minimum_ms) {
    long long candidate = now_ms();
    while (candidate <= minimum_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

void write_bridge_json_file(const char* path, const std::string& payload) {
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    out << payload;
}

void write_camera_bridge_json() {
    write_bridge_json_file(
        "camera_bridge_latest.json",
        "{"
        "\"timestamp_ms\": 1111,"
        "\"backend\": \"json-camera-test\","
        "\"bridge_connected\": true,"
        "\"sample_ready\": true,"
        "\"face_detected\": true,"
        "\"mouth_open_state\": true,"
        "\"mouth_open_ratio\": 0.12,"
        "\"mouth_center_x\": 0.66,"
        "\"mouth_center_y\": 0.31,"
        "\"confidence\": 0.93,"
        "\"jaw_open_score\": 0.41,"
        "\"looking_forward\": true"
        "}");
}

void write_microphone_bridge_json() {
    write_bridge_json_file(
        "microphone_bridge_latest.json",
        "{"
        "\"timestamp_ms\": 2222,"
        "\"backend\": \"json-microphone-test\","
        "\"bridge_connected\": true,"
        "\"sample_ready\": true,"
        "\"voice_detected\": true,"
        "\"fallback_requested\": false,"
        "\"suggested_power\": 0.27,"
        "\"direction_x\": 0.15,"
        "\"direction_y\": -0.88,"
        "\"confidence\": 0.82"
        "}");
}

}  // namespace

int main() {
    bootstrap_runtime();
    seed_startup_flow();

    if (!init_visualization()) {
        std::cerr << "runtime smoke test failed: visualization scaffold init failed\n";
        return 1;
    }

    write_camera_bridge_json();
    write_microphone_bridge_json();
    process_visual_input();
    assert(current_runtime_state().camera_bridge.bridge_connected);
    assert(current_runtime_state().camera_bridge.backend == "json-camera-test");
    assert(current_runtime_state().microphone_bridge.bridge_connected);
    assert(current_runtime_state().microphone_bridge.backend == "json-microphone-test");

    scheduler_tick();
    scheduler_tick();
    render_visual_frame();

    SchedulerSnapshot snapshot = scheduler_snapshot();
    assert(snapshot.visualization_enabled);
    assert(snapshot.thread_mode == 1);
    assert(snapshot.remaining_particles == 100);
    assert(snapshot.p2_queue.size() == 1);
    assert(has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL));
    assert(current_runtime_state().phase == RuntimePhase::READY);

    const long long camera_open_ts = now_ms();
    set_camera_bridge_sample(true, true, true, 0.57f, 0.43f, 0.9f, camera_open_ts);
    set_microphone_bridge_sample(true, 0.25f, false, 0.8f, next_timestamp_after(camera_open_ts));

    scheduler_tick();
    assert(current_render_data().camera_layer.mouth_detected);
    assert(!has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));

    scheduler_tick();
    assert(current_render_data().wind_layer.status == "mic-bridge-active");
    assert(!has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL));

    scheduler_tick();
    assert(has_queued_task(TaskType::BATCH_PARTICLE_EXECUTION, PriorityLevel::P2_FUNCTIONAL));
    assert(current_render_data().ui.queued_particle_tasks > 0);

    for (int attempt = 0; attempt < 1024; ++attempt) {
        SchedulerSnapshot live = scheduler_snapshot();
        if (live.p2_queue.empty() && live.p3_queue.empty() &&
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

    shutdown_visualization();
    std::cout << "main_runtime_test passed\n";
    return 0;
}
