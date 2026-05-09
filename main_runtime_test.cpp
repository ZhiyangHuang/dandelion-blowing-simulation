#include "thread.h"

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

const TaskRecord& find_task_record(const std::vector<TaskRecord>& queue, TaskType type) {
    for (const TaskRecord& record : queue) {
        if (record.type == type) {
            return record;
        }
    }
    assert(false && "task record not found");
    return queue.front();
}

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
        "\"voice_detected\": true,"
        "\"fallback_requested\": false,"
        "\"suggested_power\": 0.27,"
        "\"direction_x\": 0.15,"
        "\"direction_y\": -0.88,"
        "\"confidence\": 0.82"
        "}");
}

void prepare_chain_runtime(int thread_mode) {
    reset_runtime();
    set_runtime_phase(RuntimePhase::READY);
    set_thread_mode(thread_mode);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    submit_task(make_camera_task());
    submit_task(make_microphone_task());
}

void tick_until_status(const std::string& expected_status, int max_ticks) {
    for (int attempt = 0; attempt < max_ticks; ++attempt) {
        scheduler_tick();
        if (current_render_data().ui.generate_task_status == expected_status) {
            return;
        }
    }
    assert(false && "timed out waiting for task-chain status");
}

void tick_until_no_particle_work(int max_ticks) {
    for (int attempt = 0; attempt < max_ticks; ++attempt) {
        if (scheduler_snapshot().p3_queue.empty() &&
            current_render_data().ui.queued_particle_tasks == 0) {
            return;
        }
        scheduler_tick();
    }
    assert(false && "timed out waiting for particle completion");
}

void tick_until_queued_particles(int expected_count, int max_ticks) {
    for (int attempt = 0; attempt < max_ticks; ++attempt) {
        if (current_render_data().ui.queued_particle_tasks == expected_count) {
            return;
        }
        scheduler_tick();
    }
    assert(false && "timed out waiting for queued particle count");
}

void restart_input_residents_for_replay() {
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    submit_task(make_camera_task());
    submit_task(make_microphone_task());
    render_data().ui.generate_task_status.clear();
    render_data().ui.particle_task_status.clear();
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
    assert(current_runtime_state().camera_bridge.mouth_center_x == 0.66f);
    assert(current_runtime_state().microphone_bridge.bridge_connected);
    assert(current_runtime_state().microphone_bridge.backend == "json-microphone-test");
    assert(current_runtime_state().microphone_bridge.suggested_power == 0.27f);
    scheduler_tick();
    scheduler_tick();
    render_visual_frame();

    SchedulerSnapshot snapshot = scheduler_snapshot();
    LockDebugState locks_after_reset = lock_debug_state();
    assert(snapshot.visualization_enabled);
    assert(snapshot.thread_mode == 1);
    assert(snapshot.remaining_particles == 100);
    assert(snapshot.p2_queue.size() == 3);
    assert(current_runtime_state().phase == RuntimePhase::READY);
    assert(locks_after_reset.last_ordered_write_sequence == "particle->power->render");

    set_camera_bridge_sample(true, true, true, 0.57f, 0.43f);
    scheduler_tick();
    assert(current_render_data().camera_layer.update_tick == 1);
    assert(current_render_data().wind_layer.sample_tick == 0);
    assert(current_render_data().wind_layer.tick_count == 0);
    assert(current_render_data().camera_layer.enabled);
    assert(current_render_data().camera_layer.mouth_detected);
    assert(current_render_data().camera_layer.status == "camera-bridge-active");
    assert(current_runtime_state().camera_gate_open);
    assert(current_render_data().ui.camera_task_status.find("CAM bridge active") != std::string::npos);
    SchedulerSnapshot after_camera = scheduler_snapshot();
    const TaskRecord& camera_after_execute =
        find_task_record(after_camera.p2_queue, TaskType::CAMERA);
    assert(camera_after_execute.dispatch_count == 1);
    assert(camera_after_execute.execute_count == 1);
    assert(camera_after_execute.interrupt_count == 1);
    assert(camera_after_execute.requeue_count == 1);
    assert(camera_after_execute.last_queue_action == "execute-tail");
    assert(after_camera.threads[0].state == ThreadState::WAITING);

    set_microphone_bridge_sample(true, 0.25f, false);
    scheduler_tick();
    LockDebugState locks_after_mic = lock_debug_state();
    SchedulerSnapshot after_mic = scheduler_snapshot();
    assert(current_render_data().wind_layer.sample_tick == 1);
    assert(current_render_data().wind_layer.status == "mic-bridge-active");
    assert(current_render_data().ui.microphone_task_status.find("MIC bridge active") != std::string::npos);
    const TaskRecord& microphone_after_execute =
        find_task_record(after_mic.p2_queue, TaskType::MICROPHONE);
    assert(microphone_after_execute.dispatch_count == 1);
    assert(microphone_after_execute.execute_count == 1);
    assert(microphone_after_execute.interrupt_count == 1);
    assert(microphone_after_execute.requeue_count == 1);
    assert(locks_after_mic.last_ordered_write_sequence == "power->render");

    scheduler_tick();
    LockDebugState locks_after_batch = lock_debug_state();
    assert(current_render_data().wind_layer.tick_count == 1);
    assert(current_render_data().ui.batch_task_status == "BATCH resident tick 1");
    assert(locks_after_batch.last_ordered_write_sequence == "particle->power->render");

    scheduler_tick();
    scheduler_tick();
    LockDebugState locks_after_generate = lock_debug_state();
    SchedulerSnapshot generated = scheduler_snapshot();
    assert(generated.p2_queue.size() == 3);
    assert(!generated.p3_queue.empty());
    assert(current_render_data().ui.generate_task_status == "GENERATE created 2 particle tasks");
    assert(current_render_data().ui.queued_particle_tasks == 2);
    assert(locks_after_generate.last_ordered_write_sequence == "particle->power->render");

    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    with_shared_state_write(true, false, true, []() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        state_ref.world.queued_particle_tasks = 1;
        data.ui.queued_particle_tasks = 1;
        data.particles[0].active = true;
        data.particles[0].attached = false;
        data.particles[0].ownership_token = 1;
        data.particles[0].status = "queued";
    });
    submit_task(make_single_particle_task(0, 1, 1));

    scheduler_tick();
    assert(current_render_data().particles[0].status == "particle-execute");
    SchedulerSnapshot particle_after_execute = scheduler_snapshot();
    assert(particle_after_execute.p3_queue.front().dispatch_count == 1);
    assert(particle_after_execute.p3_queue.front().execute_count == 1);
    assert(particle_after_execute.p3_queue.front().interrupt_count == 1);
    assert(particle_after_execute.p3_queue.front().requeue_count == 1);
    assert(particle_after_execute.threads[0].state == ThreadState::WAITING);
    scheduler_tick();
    assert(current_render_data().particles[0].status == "particle-resume");
    SchedulerSnapshot particle_after_resume = scheduler_snapshot();
    assert(particle_after_resume.p3_queue.front().dispatch_count == 2);
    assert(particle_after_resume.p3_queue.front().resume_count == 1);
    assert(particle_after_resume.p3_queue.front().interrupt_count == 2);
    assert(particle_after_resume.p3_queue.front().requeue_count == 2);
    assert(particle_after_resume.p3_queue.front().last_queue_action == "resume-tail");
    scheduler_tick();
    LockDebugState locks_after_particle = lock_debug_state();
    assert(current_render_data().particles[0].status == "particle-finished");
    assert(current_render_data().particles[0].source_task_id == -1);
    assert(current_render_data().ui.queued_particle_tasks == 0);
    assert(current_render_data().ui.particle_task_status.find("finished cleanup") !=
        std::string::npos);
    assert(locks_after_particle.last_ordered_write_sequence == "particle->render");
    submit_task(make_batch_particle_execution_task());
    scheduler_tick();
    assert(current_render_data().particles[0].status == "particle-cleaned");
    assert(current_render_data().particles[0].x == -1.0f);
    assert(current_render_data().particles[0].y == -1.0f);
    assert(current_render_data().ui.batch_task_status.find("cleaned 1 particle render records") !=
        std::string::npos);

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    with_shared_state_write(true, true, true, []() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        data.particles[0].attached = false;
        data.particles[0].active = true;
        data.particles[0].status = "queued";
        data.particles[1].attached = false;
        data.particles[1].active = true;
        data.particles[1].source_task_id = 42;
        data.particles[1].status = "particle-resume";
        state_ref.world.remaining_particles = 55;
        state_ref.world.queued_particle_tasks = 8;
        data.ui.remaining_particles = 55;
        data.ui.queued_particle_tasks = 8;
    });
    submit_task(make_batch_particle_execution_task());
    scheduler_tick();
    assert(current_runtime_state().world.remaining_particles == 98);
    assert(current_runtime_state().world.queued_particle_tasks == 1);
    assert(current_render_data().ui.remaining_particles == 98);
    assert(current_render_data().ui.queued_particle_tasks == 1);

    prepare_chain_runtime(2);
    set_camera_bridge_sample(true, true, true, 0.55f, 0.44f);
    set_microphone_bridge_sample(true, 0.32f, false);
    tick_until_status("GENERATE created 3 particle tasks", 6);
    SchedulerSnapshot voiced_chain = scheduler_snapshot();
    assert(current_runtime_state().camera_gate_open);
    assert(!has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL));
    assert(!voiced_chain.p3_queue.empty());
    assert(voiced_chain.p3_queue.front().type == TaskType::SINGLE_PARTICLE);
    assert(current_render_data().ui.generate_task_status == "GENERATE created 3 particle tasks");
    assert(current_render_data().ui.queued_particle_tasks == 3);
    tick_until_no_particle_work(40);
    assert(scheduler_snapshot().p3_queue.empty());
    assert(has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));
    assert(current_render_data().ui.queued_particle_tasks == 0);
    assert(
        current_render_data().particles[0].status == "particle-finished" ||
        current_render_data().particles[0].status == "particle-cleaned");
    assert(
        current_render_data().particles[1].status == "particle-finished" ||
        current_render_data().particles[1].status == "particle-cleaned");
    assert(
        current_render_data().particles[2].status == "particle-finished" ||
        current_render_data().particles[2].status == "particle-cleaned");

    prepare_chain_runtime(2);
    set_camera_bridge_sample(true, true, true, 0.46f, 0.57f);
    set_microphone_bridge_sample(false, 5.0f, true);
    tick_until_status("BREEZE fallback created 1 particle task", 6);
    SchedulerSnapshot breath_chain = scheduler_snapshot();
    assert(current_runtime_state().camera_gate_open);
    assert(!has_queued_task(TaskType::BREEZE, PriorityLevel::P2_FUNCTIONAL));
    assert(!breath_chain.p3_queue.empty());
    assert(breath_chain.p3_queue.front().type == TaskType::SINGLE_PARTICLE);
    assert(current_render_data().wind_layer.status == "breeze-fallback");
    assert(current_render_data().ui.generate_task_status == "BREEZE fallback created 1 particle task");
    assert(current_render_data().ui.queued_particle_tasks == 1);
    tick_until_no_particle_work(20);
    assert(scheduler_snapshot().p3_queue.empty());
    assert(has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));
    assert(current_render_data().ui.queued_particle_tasks == 0);
    assert(
        current_render_data().particles[0].status == "particle-finished" ||
        current_render_data().particles[0].status == "particle-cleaned");

    prepare_chain_runtime(2);
    const long long replay_base_ms = now_ms();
    set_camera_bridge_sample(true, true, true, 0.54f, 0.45f, 0.9f, replay_base_ms);
    set_microphone_bridge_sample(true, 0.32f, false, 0.8f, replay_base_ms);
    tick_until_status("GENERATE created 3 particle tasks", 6);
    tick_until_no_particle_work(40);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    submit_task(make_batch_particle_execution_task());
    scheduler_tick();
    assert(current_render_data().particles[0].status == "particle-cleaned");
    assert(current_render_data().particles[1].status == "particle-cleaned");
    assert(current_render_data().particles[2].status == "particle-cleaned");
    assert(current_render_data().particles[3].status == "idle");
    assert(current_render_data().particles[0].ownership_token == 1);
    restart_input_residents_for_replay();
    const long long replay_second_ms =
        next_timestamp_after(current_runtime_state().last_consumed_microphone_sample_ms);
    set_camera_bridge_sample(true, true, true, 0.53f, 0.46f, 0.9f, replay_second_ms);
    set_microphone_bridge_sample(true, 0.32f, false, 0.8f, replay_second_ms);
    tick_until_queued_particles(3, 8);
    assert(current_render_data().ui.generate_task_status == "GENERATE created 3 particle tasks");
    assert(current_render_data().particles[0].status == "particle-cleaned");
    assert(current_render_data().particles[1].status == "particle-cleaned");
    assert(current_render_data().particles[2].status == "particle-cleaned");
    assert(
        current_render_data().particles[3].status == "queued" ||
        current_render_data().particles[3].status == "particle-execute" ||
        current_render_data().particles[3].status == "particle-resume");
    assert(
        current_render_data().particles[4].status == "queued" ||
        current_render_data().particles[4].status == "particle-execute" ||
        current_render_data().particles[4].status == "particle-resume");
    assert(
        current_render_data().particles[5].status == "queued" ||
        current_render_data().particles[5].status == "particle-execute" ||
        current_render_data().particles[5].status == "particle-resume");
    assert(current_render_data().particles[3].ownership_token == 1);
    assert(current_render_data().particles[4].ownership_token == 1);
    assert(current_render_data().particles[5].ownership_token == 1);
    tick_until_no_particle_work(40);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    submit_task(make_batch_particle_execution_task());
    scheduler_tick();

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    with_shared_state_write(true, false, true, []() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        state_ref.world.queued_particle_tasks = 1;
        data.ui.queued_particle_tasks = 1;
        data.particles[0].x = 0.995f;
        data.particles[0].y = 0.995f;
        data.particles[0].active = true;
        data.particles[0].attached = false;
        data.particles[0].ownership_token = 1;
        data.particles[0].status = "queued";
    });
    submit_task(make_single_particle_task(0, 1, 1));
    scheduler_tick();
    assert(scheduler_snapshot().p3_queue.empty());
    assert(current_render_data().particles[0].status == "particle-boundary-stop");
    assert(!current_render_data().particles[0].active);
    assert(current_render_data().particles[0].source_task_id == -1);
    assert(current_render_data().ui.queued_particle_tasks == 0);
    assert(current_render_data().ui.particle_task_status.find("boundary stop cleanup") !=
        std::string::npos);
    submit_task(make_batch_particle_execution_task());
    scheduler_tick();
    assert(current_render_data().particles[0].status == "particle-cleaned");
    assert(current_render_data().particles[0].x == -1.0f);
    assert(current_render_data().particles[0].y == -1.0f);

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    submit_task(make_camera_task());
    submit_task(make_microphone_task());
    set_camera_bridge_sample(true, false, true, 0.52f, 0.47f);
    set_microphone_bridge_sample(true, 0.3f, false);
    scheduler_tick();
    scheduler_tick();
    assert(!current_runtime_state().camera_gate_open);
    assert(current_render_data().wind_layer.status == "mic-waiting-for-camera");
    assert(current_render_data().ui.microphone_task_status == "MIC waiting for camera gate");
    assert(!has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL));

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    submit_task(make_camera_task());
    set_camera_bridge_sample(true, true, true, 0.5f, 0.5f, 0.9f, now_ms() - 5000);
    scheduler_tick();
    assert(!current_runtime_state().camera_gate_open);
    assert(current_render_data().camera_layer.status == "camera-bridge-stale");

    set_thread_mode(1);
    SchedulerSnapshot single_mode = scheduler_snapshot();
    assert(single_mode.p2_queue.size() == 1);
    assert(single_mode.p2_queue.front().type == TaskType::CAMERA);
    assert(current_render_data().ui.microphone_task_status == "MIC waiting for thread mode");
    assert(current_render_data().ui.batch_task_status == "BATCH waiting for thread mode");

    set_thread_mode(3);
    SchedulerSnapshot full_mode = scheduler_snapshot();
    assert(full_mode.p2_queue.size() == 3);
    assert(has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::BATCH_PARTICLE_EXECUTION, PriorityLevel::P2_FUNCTIONAL));
    assert(current_render_data().ui.microphone_task_status == "offline");
    assert(current_render_data().ui.batch_task_status == "idle");

    shutdown_visualization();
    assert(!scheduler_snapshot().visualization_enabled);

    std::cout << "runtime smoke test passed\n";
    return 0;
}
