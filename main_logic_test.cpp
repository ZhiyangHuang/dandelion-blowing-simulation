#include "thread.h"

#include <cassert>
#include <chrono>
#include <iostream>

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

const TaskRecord& find_task_record_by_id(const std::vector<TaskRecord>& queue, int id) {
    for (const TaskRecord& record : queue) {
        if (record.id == id) {
            return record;
        }
    }
    assert(false && "task record id not found");
    return queue.front();
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
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

class FinishingTestTask final : public Task {
public:
    FinishingTestTask(PriorityLevel task_priority, std::string task_name) {
        type = TaskType::PLACEHOLDER;
        priority = task_priority;
        name = std::move(task_name);
        support_resume = false;
    }

    void execute() override {
        state = TaskState::FINISHED;
    }
};

class TriggerPreemptionTask final : public Task {
public:
    TriggerPreemptionTask(PriorityLevel task_priority,
                          std::string task_name,
                          std::unique_ptr<Task> triggered_task)
        : triggered_task_(std::move(triggered_task)) {
        type = TaskType::PLACEHOLDER;
        priority = task_priority;
        name = std::move(task_name);
        support_resume = true;
    }

    void execute() override {
        if (!triggered_) {
            submit_task(std::move(triggered_task_));
            triggered_ = true;
        }
        state = TaskState::RUNNING;
    }

    void resume() override {
        state = TaskState::FINISHED;
    }

private:
    bool triggered_ = false;
    std::unique_ptr<Task> triggered_task_;
};

}  // namespace

int main() {
    bootstrap_runtime();

    assert(current_thread_mode() == 1);
    assert(runtime_threads().size() == 3);
    assert(current_render_data().particles.size() == 100);
    assert(current_render_data().ui.remaining_particles == 100);
    assert(current_runtime_state().phase == RuntimePhase::BOOTSTRAP);

    seed_startup_flow();
    SchedulerSnapshot before_tick = scheduler_snapshot();
    assert(before_tick.p1_queue.size() == 1);
    assert(before_tick.p2_queue.empty());
    assert(before_tick.p3_queue.empty());

    scheduler_tick();
    SchedulerSnapshot after_start = scheduler_snapshot();
    LockDebugState locks_after_start = lock_debug_state();
    assert(after_start.frame_index == 1);
    assert(after_start.p1_queue.size() == 1);
    assert(current_runtime_state().phase == RuntimePhase::STARTING);

    scheduler_tick();
    SchedulerSnapshot after_reset = scheduler_snapshot();
    LockDebugState locks_after_reset = lock_debug_state();
    assert(after_reset.frame_index == 2);
    assert(after_reset.p1_queue.empty());
    assert(after_reset.p2_queue.size() == 3);
    assert(has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::BATCH_PARTICLE_EXECUTION, PriorityLevel::P2_FUNCTIONAL));
    assert(current_runtime_state().phase == RuntimePhase::READY);
    assert(current_render_data().camera_layer.update_tick == 0);
    assert(current_render_data().wind_layer.sample_tick == 0);
    assert(current_render_data().wind_layer.tick_count == 0);
    assert(locks_after_reset.particle_lock_count == locks_after_start.particle_lock_count + 1);
    assert(locks_after_reset.power_lock_count == locks_after_start.power_lock_count + 1);
    assert(locks_after_reset.last_ordered_write_sequence == "particle->power->render");

    set_camera_bridge_sample(true, true, true, 0.61f, 0.39f);
    scheduler_tick();
    SchedulerSnapshot resident_tick_1 = scheduler_snapshot();
    LockDebugState locks_after_camera_tick = lock_debug_state();
    assert(resident_tick_1.frame_index == 3);
    assert(resident_tick_1.p2_queue.size() == 3);
    assert(current_render_data().camera_layer.update_tick == 1);
    assert(current_render_data().wind_layer.sample_tick == 0);
    assert(current_render_data().wind_layer.tick_count == 0);
    assert(current_render_data().camera_layer.enabled);
    assert(current_render_data().camera_layer.mouth_detected);
    assert(current_render_data().camera_layer.mouth_x == 0.61f);
    assert(current_render_data().camera_layer.mouth_y == 0.39f);
    assert(current_runtime_state().camera_gate_open);
    assert(current_runtime_state().camera_gate_frame == 3);
    assert(current_render_data().ui.camera_task_status.find("CAM bridge active") != std::string::npos);
    assert(current_render_data().ui.microphone_task_status == "offline");
    assert(current_render_data().ui.batch_task_status == "idle");
    const TaskRecord& camera_after_execute =
        find_task_record(resident_tick_1.p2_queue, TaskType::CAMERA);
    assert(camera_after_execute.dispatch_count == 1);
    assert(camera_after_execute.execute_count == 1);
    assert(camera_after_execute.resume_count == 0);
    assert(camera_after_execute.interrupt_count == 1);
    assert(camera_after_execute.requeue_count == 1);
    assert(!camera_after_execute.last_run_used_resume);
    assert(camera_after_execute.last_queue_action == "execute-tail");
    assert(camera_after_execute.last_interrupt_reason == "timeslice-expired");
    assert(camera_after_execute.last_transition == "requeued");
    assert(resident_tick_1.threads[0].dispatch_count == 3);
    assert(resident_tick_1.threads[0].state == ThreadState::WAITING);
    assert(resident_tick_1.threads[0].last_completed_task_id == camera_after_execute.id);
    assert(resident_tick_1.threads[0].last_task_event == "interrupted-requeued");
    assert(locks_after_camera_tick.particle_lock_count == locks_after_reset.particle_lock_count);
    assert(locks_after_camera_tick.power_lock_count == locks_after_reset.power_lock_count);
    assert(locks_after_camera_tick.last_ordered_write_sequence == "particle->power->render");

    set_microphone_bridge_sample(true, 0.25f, false);
    scheduler_tick();
    SchedulerSnapshot resident_tick_2 = scheduler_snapshot();
    LockDebugState locks_after_mic_tick = lock_debug_state();
    assert(resident_tick_2.frame_index == 4);
    assert(resident_tick_2.p2_queue.size() == 4);
    assert(current_render_data().camera_layer.update_tick == 1);
    assert(current_render_data().wind_layer.sample_tick == 1);
    assert(current_render_data().wind_layer.tick_count == 0);
    assert(current_render_data().camera_layer.status == "camera-bridge-active");
    assert(current_render_data().wind_layer.status == "mic-bridge-active");
    assert(current_render_data().ui.microphone_task_status.find("MIC bridge active") != std::string::npos);
    assert(has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL));
    const TaskRecord& microphone_after_execute =
        find_task_record(resident_tick_2.p2_queue, TaskType::MICROPHONE);
    assert(microphone_after_execute.dispatch_count == 1);
    assert(microphone_after_execute.execute_count == 1);
    assert(microphone_after_execute.resume_count == 0);
    assert(microphone_after_execute.interrupt_count == 1);
    assert(microphone_after_execute.requeue_count == 1);
    assert(!microphone_after_execute.last_run_used_resume);
    assert(microphone_after_execute.last_queue_action == "execute-tail");
    assert(microphone_after_execute.last_transition == "requeued");
    assert(locks_after_mic_tick.power_lock_count == locks_after_camera_tick.power_lock_count + 1);
    assert(locks_after_mic_tick.last_ordered_write_sequence == "power->render");

    scheduler_tick();
    SchedulerSnapshot resident_tick_3 = scheduler_snapshot();
    LockDebugState locks_after_batch_tick = lock_debug_state();
    assert(resident_tick_3.frame_index == 5);
    assert(resident_tick_3.p2_queue.size() == 4);
    assert(current_render_data().camera_layer.update_tick == 1);
    assert(current_render_data().wind_layer.sample_tick == 1);
    assert(current_render_data().wind_layer.tick_count == 1);
    assert(current_render_data().wind_layer.status == "resident-execute");
    assert(current_render_data().ui.batch_task_status == "BATCH resident tick 1");
    const TaskRecord& batch_after_execute =
        find_task_record(resident_tick_3.p2_queue, TaskType::BATCH_PARTICLE_EXECUTION);
    assert(batch_after_execute.dispatch_count == 1);
    assert(batch_after_execute.execute_count == 1);
    assert(batch_after_execute.resume_count == 0);
    assert(batch_after_execute.interrupt_count == 1);
    assert(batch_after_execute.requeue_count == 1);
    assert(batch_after_execute.last_queue_action == "execute-tail");
    assert(locks_after_batch_tick.particle_lock_count == locks_after_mic_tick.particle_lock_count + 1);
    assert(locks_after_batch_tick.power_lock_count == locks_after_mic_tick.power_lock_count + 1);
    assert(locks_after_batch_tick.last_ordered_write_sequence == "particle->power->render");

    scheduler_tick();
    SchedulerSnapshot resident_tick_4 = scheduler_snapshot();
    assert(resident_tick_4.frame_index == 6);
    assert(resident_tick_4.p2_queue.size() == 4);
    assert(current_render_data().camera_layer.update_tick == 2);
    assert(current_render_data().wind_layer.tick_count == 1);
    assert(current_render_data().camera_layer.status == "camera-bridge-resume");
    const TaskRecord& camera_after_resume =
        find_task_record(resident_tick_4.p2_queue, TaskType::CAMERA);
    assert(camera_after_resume.dispatch_count == 2);
    assert(camera_after_resume.execute_count == 1);
    assert(camera_after_resume.resume_count == 1);
    assert(camera_after_resume.interrupt_count == 2);
    assert(camera_after_resume.requeue_count == 2);
    assert(camera_after_resume.last_run_used_resume);
    assert(camera_after_resume.last_queue_action == "resume-tail");
    assert(camera_after_resume.last_transition == "requeued");
    assert(resident_tick_4.threads[0].state == ThreadState::WAITING);

    scheduler_tick();
    SchedulerSnapshot generate_tick = scheduler_snapshot();
    LockDebugState locks_after_generate = lock_debug_state();
    assert(generate_tick.frame_index == 7);
    assert(generate_tick.p2_queue.size() == 3);
    assert(!has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL));
    assert(!generate_tick.p3_queue.empty());
    assert(current_render_data().ui.generate_task_status == "GENERATE created 2 particle tasks");
    assert(current_render_data().ui.particle_task_status == "P3 queue populated");
    assert(current_render_data().ui.queued_particle_tasks == 2);
    assert(current_render_data().ui.remaining_particles == 98);
    assert(current_render_data().particles[0].status == "queued");
    assert(current_render_data().particles[1].status == "queued");
    const TaskRecord& camera_after_second_resume =
        find_task_record(generate_tick.p2_queue, TaskType::CAMERA);
    assert(camera_after_second_resume.dispatch_count == 2);
    const TaskRecord& generate_record = find_task_record(generate_tick.p3_queue, TaskType::SINGLE_PARTICLE);
    assert(generate_record.dispatch_count == 0);
    assert(generate_record.execute_count == 0);
    assert(generate_record.resume_count == 0);
    assert(generate_record.last_transition == "created");
    assert(locks_after_generate.particle_lock_count == locks_after_batch_tick.particle_lock_count + 1);
    assert(locks_after_generate.power_lock_count == locks_after_batch_tick.power_lock_count + 1);
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
    SchedulerSnapshot particle_tick_1 = scheduler_snapshot();
    LockDebugState locks_after_particle_1 = lock_debug_state();
    const float particle_x_after_tick_1 = current_render_data().particles[0].x;
    const float particle_y_after_tick_1 = current_render_data().particles[0].y;
    assert(particle_tick_1.p3_queue.size() == 1);
    assert(current_render_data().particles[0].status == "particle-execute");
    assert(current_render_data().particles[0].source_task_id > 0);
    assert(current_render_data().ui.particle_task_status.find("cycle 1") != std::string::npos);
    assert(current_render_data().ui.queued_particle_tasks == 1);
    const TaskRecord& particle_after_execute =
        particle_tick_1.p3_queue.front();
    assert(particle_after_execute.dispatch_count == 1);
    assert(particle_after_execute.execute_count == 1);
    assert(particle_after_execute.resume_count == 0);
    assert(particle_after_execute.interrupt_count == 1);
    assert(particle_after_execute.requeue_count == 1);
    assert(!particle_after_execute.last_run_used_resume);
    assert(particle_after_execute.last_queue_action == "execute-tail");
    assert(particle_after_execute.last_interrupt_reason == "timeslice-expired");
    assert(particle_tick_1.threads[0].state == ThreadState::WAITING);
    assert(locks_after_particle_1.last_ordered_write_sequence == "particle->render");

    scheduler_tick();
    SchedulerSnapshot particle_tick_2 = scheduler_snapshot();
    LockDebugState locks_after_particle_2 = lock_debug_state();
    assert(particle_tick_2.p3_queue.size() == 1);
    assert(current_render_data().particles[0].status == "particle-resume");
    assert(current_render_data().particles[0].x > particle_x_after_tick_1);
    assert(current_render_data().particles[0].y > particle_y_after_tick_1);
    assert(current_render_data().ui.particle_task_status.find("cycle 2") != std::string::npos);
    assert(current_render_data().ui.queued_particle_tasks == 1);
    const TaskRecord& particle_after_resume =
        particle_tick_2.p3_queue.front();
    assert(particle_after_resume.dispatch_count == 2);
    assert(particle_after_resume.execute_count == 1);
    assert(particle_after_resume.resume_count == 1);
    assert(particle_after_resume.interrupt_count == 2);
    assert(particle_after_resume.requeue_count == 2);
    assert(particle_after_resume.last_run_used_resume);
    assert(particle_after_resume.last_queue_action == "resume-tail");
    assert(locks_after_particle_2.last_ordered_write_sequence == "particle->render");

    scheduler_tick();
    SchedulerSnapshot particle_tick_3 = scheduler_snapshot();
    LockDebugState locks_after_particle_3 = lock_debug_state();
    assert(particle_tick_3.p3_queue.empty());
    assert(current_render_data().particles[0].status == "particle-finished");
    assert(!current_render_data().particles[0].active);
    assert(current_render_data().particles[0].source_task_id == -1);
    assert(current_render_data().ui.particle_task_status.find("finished cleanup") !=
        std::string::npos);
    assert(current_render_data().ui.queued_particle_tasks == 0);
    assert(locks_after_particle_3.last_ordered_write_sequence == "particle->render");
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
    SchedulerSnapshot particle_boundary_stop = scheduler_snapshot();
    assert(particle_boundary_stop.p3_queue.empty());
    assert(current_render_data().particles[0].status == "particle-boundary-stop");
    assert(!current_render_data().particles[0].active);
    assert(!current_render_data().particles[0].attached);
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
        state_ref.world.remaining_particles = 77;
        state_ref.world.queued_particle_tasks = 9;
        data.ui.remaining_particles = 77;
        data.ui.queued_particle_tasks = 9;
    });
    submit_task(make_batch_particle_execution_task());
    scheduler_tick();
    assert(current_runtime_state().world.remaining_particles == 98);
    assert(current_runtime_state().world.queued_particle_tasks == 1);
    assert(current_render_data().ui.remaining_particles == 98);
    assert(current_render_data().ui.queued_particle_tasks == 1);

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    with_shared_state_write(true, true, true, []() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        data.particles[0].attached = false;
        data.particles[0].active = false;
        data.particles[0].ownership_token = 4;
        data.particles[0].status = "particle-cleaned";
        data.particles[1].attached = true;
        data.particles[2].attached = true;
        data.particles[3].attached = true;
        state_ref.world.remaining_particles = 99;
        data.ui.remaining_particles = 99;
        state_ref.world.power = 0.32f;
        data.ui.power = 0.32f;
    });
    submit_task(make_generate_particle_task());
    scheduler_tick();
    SchedulerSnapshot cleaned_slot_skip = scheduler_snapshot();
    assert(cleaned_slot_skip.p3_queue.size() == 3);
    assert(current_render_data().particles[0].status == "particle-cleaned");
    assert(current_render_data().particles[0].ownership_token == 4);
    assert(current_render_data().particles[1].status == "queued");
    assert(current_render_data().particles[2].status == "queued");
    assert(current_render_data().particles[3].status == "queued");

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    with_shared_state_write(true, true, true, []() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        data.particles[0].attached = false;
        data.particles[0].active = true;
        data.particles[0].ownership_token = 2;
        data.particles[0].status = "queued";
        state_ref.world.remaining_particles = 99;
        state_ref.world.queued_particle_tasks = 1;
        data.ui.remaining_particles = 99;
        data.ui.queued_particle_tasks = 1;
    });
    submit_task(make_single_particle_task(0, 1, 1));
    submit_task(make_single_particle_task(0, 1, 2));
    scheduler_tick();
    assert(scheduler_snapshot().p3_queue.size() == 1);
    assert(current_render_data().particles[0].ownership_token == 2);
    assert(current_render_data().particles[0].status == "queued");
    assert(current_runtime_state().world.queued_particle_tasks == 1);
    assert(current_render_data().ui.queued_particle_tasks == 1);
    assert(current_render_data().ui.particle_task_status.find("lost slot ownership") !=
        std::string::npos);

    reset_runtime();
    set_thread_mode(1);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    submit_task(make_camera_task());
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
    SchedulerSnapshot mixed_single_tick_1 = scheduler_snapshot();
    assert(mixed_single_tick_1.p2_queue.size() == 1);
    assert(mixed_single_tick_1.p3_queue.size() == 1);
    assert(current_render_data().ui.queued_particle_tasks == 1);
    assert(mixed_single_tick_1.p2_queue.front().type == TaskType::CAMERA);
    assert(mixed_single_tick_1.p2_queue.front().dispatch_count == 1);
    assert(mixed_single_tick_1.p3_queue.front().dispatch_count == 0);
    scheduler_tick();
    SchedulerSnapshot mixed_single_tick_2 = scheduler_snapshot();
    assert(current_render_data().particles[0].status == "particle-execute");
    assert(current_render_data().ui.queued_particle_tasks == 1);
    assert(mixed_single_tick_2.p2_queue.front().dispatch_count == 1);
    assert(mixed_single_tick_2.p3_queue.front().dispatch_count == 1);
    assert(mixed_single_tick_2.p3_queue.front().execute_count == 1);
    assert(mixed_single_tick_2.p3_queue.front().resume_count == 0);
    scheduler_tick();
    SchedulerSnapshot mixed_single_tick_3 = scheduler_snapshot();
    assert(current_render_data().ui.queued_particle_tasks == 1);
    assert(mixed_single_tick_3.p2_queue.front().dispatch_count == 2);
    assert(mixed_single_tick_3.p3_queue.front().dispatch_count == 1);

    reset_runtime();
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    submit_task(make_camera_task());
    submit_task(make_microphone_task());
    submit_task(make_batch_particle_execution_task());
    with_shared_state_write(true, false, true, []() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        state_ref.world.queued_particle_tasks = 1;
        data.ui.queued_particle_tasks = 1;
        data.particles[0].active = true;
        data.particles[0].attached = false;
        data.particles[0].status = "queued";
    });
    submit_task(make_single_particle_task(0, 1));
    scheduler_tick();
    SchedulerSnapshot mixed_three_thread = scheduler_snapshot();
    assert(mixed_three_thread.p2_queue.size() == 3);
    assert(mixed_three_thread.p3_queue.size() == 1);
    assert(current_render_data().ui.queued_particle_tasks == 1);
    assert(mixed_three_thread.p3_queue.front().dispatch_count == 1);
    assert(mixed_three_thread.p3_queue.front().execute_count == 1);
    assert(mixed_three_thread.p3_queue.front().resume_count == 0);
    assert(mixed_three_thread.threads[0].state == ThreadState::WAITING);
    assert(mixed_three_thread.threads[1].state == ThreadState::WAITING);
    assert(mixed_three_thread.threads[2].state == ThreadState::WAITING);

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    set_microphone_bridge_sample(false, 5.0f, true);
    submit_task(make_breeze_task());
    scheduler_tick();
    SchedulerSnapshot breeze_snapshot = scheduler_snapshot();
    assert(breeze_snapshot.p2_queue.empty());
    assert(breeze_snapshot.p3_queue.size() == 1);
    assert(current_render_data().wind_layer.status == "breeze-fallback");
    assert(current_render_data().ui.power == 5.0f);
    assert(current_render_data().ui.remaining_particles == 99);
    assert(current_render_data().ui.queued_particle_tasks == 1);
    assert(
        current_render_data().particles[0].status == "breeze-queued" ||
        current_render_data().particles[0].status == "particle-execute" ||
        current_render_data().particles[0].status == "particle-resume" ||
        current_render_data().particles[0].status == "particle-finished");
    assert(current_render_data().ui.generate_task_status ==
        "BREEZE fallback created 1 particle task");

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    set_camera_bridge_sample(true, false, true, 0.42f, 0.52f);
    with_shared_state_write(true, true, true, []() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        state_ref.world.power = 3.5f;
        state_ref.world.remaining_particles = 12;
        state_ref.world.queued_particle_tasks = 4;
        data.ui.power = 3.5f;
        data.ui.remaining_particles = 12;
        data.ui.queued_particle_tasks = 4;
    });
    submit_task(make_change_dandelion_task());
    scheduler_tick();
    SchedulerSnapshot change_snapshot = scheduler_snapshot();
    assert(change_snapshot.p2_queue.empty() ||
        has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL));
    assert(current_runtime_state().world.remaining_particles == 100);
    assert(current_runtime_state().world.queued_particle_tasks == 0);
    assert(current_runtime_state().world.power == 0.1f);
    assert(current_render_data().ui.remaining_particles == 100);
    assert(current_render_data().ui.queued_particle_tasks == 0);
    assert(current_render_data().ui.power == 0.1f);
    assert(current_render_data().ui.generate_task_status == "CHANGE rebuilt dandelion");
    assert(current_render_data().ui.particle_task_status == "CHANGE reset particle ring");
    assert(current_render_data().particles.size() == 100);
    assert(current_runtime_state().world.dandelion_x == 0.42f);
    assert(current_runtime_state().world.dandelion_y == 0.52f);
    assert(current_render_data().camera_layer.mouth_x == 0.42f);
    assert(current_render_data().camera_layer.mouth_y == 0.52f);

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    submit_task(make_camera_task());
    submit_task(make_microphone_task());
    set_camera_bridge_sample(true, false, true, 0.5f, 0.5f);
    set_microphone_bridge_sample(true, 0.45f, false);
    scheduler_tick();
    scheduler_tick();
    SchedulerSnapshot closed_mouth_gate = scheduler_snapshot();
    assert(!current_runtime_state().camera_gate_open);
    assert(current_render_data().camera_layer.status == "camera-bridge-face-idle");
    assert(current_render_data().wind_layer.status == "mic-waiting-for-camera");
    assert(current_render_data().ui.microphone_task_status == "MIC waiting for camera gate");
    assert(!has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL));
    assert(closed_mouth_gate.p2_queue.size() == 2);

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    submit_task(make_microphone_task());
    set_microphone_bridge_sample(false, 5.0f, true);
    scheduler_tick();
    SchedulerSnapshot no_camera_gate = scheduler_snapshot();
    assert(!current_runtime_state().camera_gate_open);
    assert(current_render_data().wind_layer.status == "mic-waiting-for-camera");
    assert(current_render_data().ui.power == 0.1f);
    assert(!has_queued_task(TaskType::BREEZE, PriorityLevel::P2_FUNCTIONAL));
    assert(no_camera_gate.p2_queue.size() == 1);

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    submit_task(make_camera_task());
    set_camera_bridge_sample(true, true, true, 0.49f, 0.51f, 0.9f, now_ms() - 5000);
    scheduler_tick();
    assert(!current_runtime_state().camera_gate_open);
    assert(current_render_data().camera_layer.status == "camera-bridge-stale");
    assert(current_render_data().ui.camera_task_status == "CAM bridge sample stale");

    reset_runtime();
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    submit_task(make_camera_task());
    submit_task(make_microphone_task());
    set_camera_bridge_sample(true, true, true, 0.51f, 0.49f);
    set_microphone_bridge_sample(true, 0.4f, false, 0.8f, now_ms() - 5000);
    scheduler_tick();
    scheduler_tick();
    assert(current_runtime_state().camera_gate_open);
    assert(current_render_data().wind_layer.status == "mic-bridge-stale");
    assert(current_render_data().ui.microphone_task_status == "MIC bridge sample stale");
    assert(!has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL));

    reset_runtime();
    set_thread_mode(1);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    const int p3_preempt_id = submit_task(std::make_unique<TriggerPreemptionTask>(
        PriorityLevel::P3_PARTICLE,
        "P3PreemptProbe",
        std::make_unique<FinishingTestTask>(PriorityLevel::P2_FUNCTIONAL, "InjectedP2")));

    scheduler_tick();
    SchedulerSnapshot after_p2_preemption = scheduler_snapshot();
    assert(after_p2_preemption.p2_queue.size() == 1);
    assert(after_p2_preemption.p3_queue.size() == 1);
    const TaskRecord& p3_preempted =
        find_task_record_by_id(after_p2_preemption.p3_queue, p3_preempt_id);
    const TaskRecord& injected_p2 =
        after_p2_preemption.p2_queue.front();
    assert(p3_preempted.interrupt_count == 1);
    assert(p3_preempted.requeue_count == 1);
    assert(p3_preempted.last_interrupt_reason == "preempted-by-p2");
    assert(p3_preempted.last_queue_action == "preempted-tail");
    assert(p3_preempted.last_transition == "requeued");
    assert(injected_p2.priority == PriorityLevel::P2_FUNCTIONAL);
    assert(injected_p2.last_transition == "created");
    assert(after_p2_preemption.threads[0].state == ThreadState::WAITING);
    assert(after_p2_preemption.threads[0].last_task_event == "preempted-requeued");

    scheduler_tick();
    SchedulerSnapshot after_injected_p2 = scheduler_snapshot();
    assert(after_injected_p2.p2_queue.empty());
    assert(after_injected_p2.p3_queue.size() == 1);
    const TaskRecord& p3_after_resume_window =
        find_task_record_by_id(after_injected_p2.p3_queue, p3_preempt_id);
    assert(p3_after_resume_window.dispatch_count == 1);
    assert(after_injected_p2.threads[0].last_completed_task_id == injected_p2.id);
    assert(after_injected_p2.threads[0].last_task_event == "finished");

    reset_runtime();
    set_thread_mode(2);
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
    const int first_p3_id = submit_task(make_single_particle_task(0, 1, 1));
    const int second_p3_id = submit_task(std::make_unique<TriggerPreemptionTask>(
        PriorityLevel::P3_PARTICLE,
        "P3TieProbe",
        std::make_unique<FinishingTestTask>(PriorityLevel::P2_FUNCTIONAL, "InjectedP2Tie")));
    scheduler_tick();
    SchedulerSnapshot after_p2_tie_preemption = scheduler_snapshot();
    assert(after_p2_tie_preemption.p2_queue.size() == 1);
    assert(after_p2_tie_preemption.p3_queue.size() == 2);
    const TaskRecord& first_p3_after_tie =
        find_task_record_by_id(after_p2_tie_preemption.p3_queue, first_p3_id);
    const TaskRecord& second_p3_after_tie =
        find_task_record_by_id(after_p2_tie_preemption.p3_queue, second_p3_id);
    assert(first_p3_after_tie.last_interrupt_reason == "preempted-by-p2");
    assert(first_p3_after_tie.last_queue_action == "preempted-tail");
    assert(second_p3_after_tie.last_interrupt_reason == "timeslice-expired");
    assert(second_p3_after_tie.last_queue_action == "execute-tail");
    assert(after_p2_tie_preemption.threads[0].last_task_event == "preempted-requeued");
    assert(after_p2_tie_preemption.threads[1].last_task_event == "interrupted-requeued");

    reset_runtime();
    set_thread_mode(2);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    submit_task(make_camera_task());
    const int p3_vs_p2_id = submit_task(std::make_unique<TriggerPreemptionTask>(
        PriorityLevel::P3_PARTICLE,
        "P3PreemptedBeforeP2",
        std::make_unique<FinishingTestTask>(PriorityLevel::P1_SYSTEM, "InjectedP1Tie")));
    scheduler_tick();
    SchedulerSnapshot after_lowest_priority_preemption = scheduler_snapshot();
    assert(after_lowest_priority_preemption.p1_queue.size() == 1);
    assert(after_lowest_priority_preemption.p2_queue.size() == 1);
    assert(after_lowest_priority_preemption.p3_queue.size() == 1);
    const TaskRecord& camera_after_lowest_priority =
        find_task_record(after_lowest_priority_preemption.p2_queue, TaskType::CAMERA);
    const TaskRecord& p3_after_lowest_priority =
        find_task_record_by_id(after_lowest_priority_preemption.p3_queue, p3_vs_p2_id);
    assert(camera_after_lowest_priority.last_interrupt_reason == "timeslice-expired");
    assert(camera_after_lowest_priority.last_queue_action == "execute-tail");
    assert(p3_after_lowest_priority.last_interrupt_reason == "preempted-by-p1");
    assert(p3_after_lowest_priority.last_queue_action == "preempted-tail");
    assert(after_lowest_priority_preemption.threads[0].last_task_event == "interrupted-requeued");
    assert(after_lowest_priority_preemption.threads[1].last_task_event == "preempted-requeued");

    reset_runtime();
    set_thread_mode(1);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    const int p2_preempt_id = submit_task(std::make_unique<TriggerPreemptionTask>(
        PriorityLevel::P2_FUNCTIONAL,
        "P2PreemptProbe",
        std::make_unique<FinishingTestTask>(PriorityLevel::P1_SYSTEM, "InjectedP1")));

    scheduler_tick();
    SchedulerSnapshot after_p1_preemption = scheduler_snapshot();
    assert(after_p1_preemption.p1_queue.size() == 1);
    assert(after_p1_preemption.p2_queue.size() == 1);
    const TaskRecord& p2_preempted =
        find_task_record_by_id(after_p1_preemption.p2_queue, p2_preempt_id);
    const TaskRecord& injected_p1 =
        after_p1_preemption.p1_queue.front();
    assert(p2_preempted.interrupt_count == 1);
    assert(p2_preempted.requeue_count == 1);
    assert(p2_preempted.last_interrupt_reason == "preempted-by-p1");
    assert(p2_preempted.last_queue_action == "preempted-tail");
    assert(injected_p1.priority == PriorityLevel::P1_SYSTEM);
    assert(after_p1_preemption.threads[0].state == ThreadState::WAITING);
    assert(after_p1_preemption.threads[0].last_task_event == "preempted-requeued");

    scheduler_tick();
    SchedulerSnapshot after_injected_p1 = scheduler_snapshot();
    assert(after_injected_p1.p1_queue.empty());
    assert(after_injected_p1.p2_queue.size() == 1);
    assert(after_injected_p1.threads[0].last_completed_task_id == injected_p1.id);
    assert(after_injected_p1.threads[0].last_task_event == "finished");

    reset_runtime();
    set_thread_mode(3);
    submit_task(make_reset_task());
    scheduler_tick();
    SchedulerSnapshot reset_waiting = scheduler_snapshot();
    assert(reset_waiting.p2_queue.size() == 3);
    assert(reset_waiting.threads[0].state == ThreadState::IDLE);
    assert(reset_waiting.threads[1].state == ThreadState::WAITING);
    assert(reset_waiting.threads[2].state == ThreadState::WAITING);
    assert(reset_waiting.threads[1].last_task_event == "reset-waiting");
    assert(reset_waiting.threads[2].last_task_event == "reset-waiting");

    set_thread_mode(1);
    SchedulerSnapshot single_mode = scheduler_snapshot();
    assert(single_mode.thread_mode == 1);
    assert(single_mode.threads[0].state == ThreadState::IDLE);
    assert(single_mode.threads[1].state == ThreadState::SLEEPING);
    assert(single_mode.threads[2].state == ThreadState::SLEEPING);
    assert(single_mode.p2_queue.size() == 1);
    assert(single_mode.p2_queue.front().type == TaskType::CAMERA);
    assert(single_mode.threads[1].last_task_event == "mode-sleep");
    assert(single_mode.threads[2].last_task_event == "mode-sleep");
    assert(current_render_data().ui.microphone_task_status == "MIC waiting for thread mode");
    assert(current_render_data().ui.batch_task_status == "BATCH waiting for thread mode");

    set_thread_mode(2);
    SchedulerSnapshot dual_mode = scheduler_snapshot();
    assert(dual_mode.thread_mode == 2);
    assert(dual_mode.threads[0].state == ThreadState::IDLE);
    assert(dual_mode.threads[1].state == ThreadState::IDLE);
    assert(dual_mode.threads[2].state == ThreadState::SLEEPING);
    assert(dual_mode.p2_queue.size() == 2);
    assert(has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL));
    assert(has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL));
    assert(!has_queued_task(TaskType::BATCH_PARTICLE_EXECUTION, PriorityLevel::P2_FUNCTIONAL));
    assert(dual_mode.threads[1].last_task_event == "mode-wake");
    assert(current_render_data().ui.microphone_task_status == "offline");
    assert(current_render_data().ui.batch_task_status == "BATCH waiting for thread mode");

    set_thread_mode(3);
    SchedulerSnapshot threaded = scheduler_snapshot();
    assert(threaded.thread_mode == 3);
    assert(threaded.threads[0].state == ThreadState::IDLE);
    assert(threaded.threads[1].state == ThreadState::IDLE);
    assert(threaded.threads[2].state == ThreadState::IDLE);
    assert(threaded.p2_queue.size() == 3);
    assert(has_queued_task(TaskType::BATCH_PARTICLE_EXECUTION, PriorityLevel::P2_FUNCTIONAL));
    assert(threaded.threads[2].last_task_event == "mode-wake");
    assert(current_render_data().ui.batch_task_status == "idle");

    reset_runtime();
    SchedulerSnapshot reset = scheduler_snapshot();
    assert(reset.frame_index == 0);
    assert(reset.p1_queue.empty());
    assert(reset.remaining_particles == 100);
    assert(current_render_data().ui.banner == "DandelionOS Runtime Scaffold");
    assert(current_runtime_state().phase == RuntimePhase::BOOTSTRAP);
    assert(current_render_data().camera_layer.update_tick == 0);
    assert(current_render_data().wind_layer.tick_count == 0);
    assert(current_render_data().ui.queued_particle_tasks == 0);

    submit_task(make_exit_task());
    scheduler_tick();
    assert(shutdown_requested());

    std::cout << "logic test passed\n";
    return 0;
}
