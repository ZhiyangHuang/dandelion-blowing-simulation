#include "thread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr long long kCameraGateHoldMs = 450;
constexpr int kRrQuantumMs = 16;
constexpr float kVelocityPerPowerPixelsPerSecond = 100.0f;
constexpr float kNormalizedWorldPixels = 50.0f;

struct StoredTask {
    std::unique_ptr<Task> task;
};

struct FrameDispatchEntry {
    int task_id = -1;
    int thread_index = -1;
    int dispatch_slot = -1;
    PriorityLevel priority = PriorityLevel::P3_PARTICLE;
    bool used_resume = false;
};

int g_next_task_id = 1;
int g_frame_index = 0;
int g_thread_mode = 1;
bool g_visualization_enabled = false;
bool g_single_thread_mixed_turn_prefers_p3 = false;
int g_single_thread_p2_phase = 0;
std::vector<RuntimeThread> g_threads;
std::deque<StoredTask> g_p1_queue;
std::deque<StoredTask> g_p2_queue;
std::deque<StoredTask> g_p3_queue;
int g_dandelion_layout_revision = 0;

long long current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool bridge_sample_is_fresh(long long timestamp_ms, long long freshness_window_ms) {
    if (timestamp_ms <= 0) {
        return false;
    }
    const long long age_ms = current_time_ms() - timestamp_ms;
    return age_ms >= 0 && age_ms <= freshness_window_ms;
}

bool camera_bridge_is_fresh(const CameraBridgeState& bridge) {
    return bridge_sample_is_fresh(bridge.timestamp_ms, 1500);
}

bool microphone_bridge_is_fresh(const MicrophoneBridgeState& bridge) {
    return bridge_sample_is_fresh(bridge.timestamp_ms, 1200);
}

bool camera_gate_is_fresh(const RuntimeState& state) {
    if (!state.camera_gate_open || state.camera_gate_frame < 0) {
        return false;
    }
    if (state.camera_gate_until_ms <= 0) {
        return (g_frame_index - state.camera_gate_frame) <= 1;
    }
    return current_time_ms() <= state.camera_gate_until_ms;
}

std::string build_input_focus_status(const RuntimeState& state) {
    if (state.camera_focus_locked) {
        return "CAMERA FOCUS";
    }
    if (state.microphone_focus_locked) {
        return "MIC FOCUS";
    }
    if (state.camera_gate_open) {
        return "GATE OPEN";
    }
    return "FREE FOCUS";
}

std::vector<TaskRecord> snapshot_queue(const std::deque<StoredTask>& queue) {
    std::vector<TaskRecord> records;
    records.reserve(queue.size());
    for (const StoredTask& entry : queue) {
        records.push_back({
            entry.task->id,
            entry.task->type,
            entry.task->priority,
            entry.task->state,
            entry.task->support_resume,
            entry.task->name,
            entry.task->dispatch_count,
            entry.task->execute_count,
            entry.task->resume_count,
            entry.task->interrupt_count,
            entry.task->requeue_count,
            entry.task->last_scheduled_frame,
            entry.task->last_completed_frame,
            entry.task->last_run_used_resume,
            entry.task->last_queue_action,
            entry.task->last_interrupt_reason,
            entry.task->last_transition,
        });
    }
    return records;
}

ThreadState thread_state_for_slot(int index, int active_threads) {
    return index < active_threads ? ThreadState::IDLE : ThreadState::SLEEPING;
}

void rebuild_thread_pool() {
    int active_threads = std::clamp(g_thread_mode, 1, 3);
    g_threads.clear();
    g_threads.reserve(3);
    for (int index = 0; index < 3; ++index) {
        RuntimeThread thread;
        thread.id = index + 1;
        thread.state = thread_state_for_slot(index, active_threads);
        thread.label = "Thread" + std::to_string(index + 1);
        g_threads.push_back(thread);
    }
}

void mark_noncurrent_runnable_threads_waiting(int current_thread_id,
                                              const std::string& event_name) {
    for (RuntimeThread& thread : g_threads) {
        if (thread.id == current_thread_id) {
            continue;
        }
        if (thread.state == ThreadState::SLEEPING || thread.state == ThreadState::CLOSED) {
            continue;
        }
        thread.state = ThreadState::WAITING;
        thread.bound_task_id = -1;
        thread.bound_task_name = "none";
        thread.last_task_event = event_name;
    }
}

void apply_thread_mode_state(int active_threads) {
    for (int index = 0; index < static_cast<int>(g_threads.size()); ++index) {
        RuntimeThread& thread = g_threads[index];
        if (index < active_threads) {
            if (thread.state == ThreadState::SLEEPING || thread.state == ThreadState::CLOSED) {
                thread.state = ThreadState::IDLE;
                thread.bound_task_id = -1;
                thread.bound_task_name = "none";
                thread.last_task_event = "mode-wake";
            }
            continue;
        }

        thread.state = ThreadState::SLEEPING;
        thread.bound_task_id = -1;
        thread.bound_task_name = "none";
        thread.last_task_event = "mode-sleep";
    }
}

std::string task_debug_label(const Task& task) {
    return task.name + "#" + std::to_string(task.id);
}

std::deque<StoredTask>& queue_for_priority(PriorityLevel priority) {
    if (priority == PriorityLevel::P1_SYSTEM) {
        return g_p1_queue;
    }
    if (priority == PriorityLevel::P2_FUNCTIONAL) {
        return g_p2_queue;
    }
    return g_p3_queue;
}

bool has_same_type(const std::deque<StoredTask>& queue, TaskType type) {
    return !queue.empty() && queue.back().task->type == type;
}

void record_queue_action(Task& task, const std::string& action) {
    task.last_queue_action = action;
}

void mark_task_created(Task& task) {
    task.state = TaskState::CREATED;
    task.last_transition = "created";
    task.last_interrupt_reason = "none";
    record_queue_action(task, "submitted");
}

void mark_task_interrupted(Task& task, const std::string& reason, int frame_index) {
    task.state = TaskState::INTERRUPTED;
    task.interrupt_count++;
    task.last_completed_frame = frame_index;
    task.last_interrupt_reason = reason;
    task.last_transition = "interrupted";
}

void mark_task_requeued(Task& task, const std::string& action) {
    task.state = TaskState::REQUEUED;
    task.requeue_count++;
    task.last_transition = "requeued";
    record_queue_action(task, action);
}

void mark_task_finished(Task& task, int frame_index, const std::string& action) {
    task.state = TaskState::FINISHED;
    task.last_completed_frame = frame_index;
    task.last_transition = "finished";
    task.last_interrupt_reason = "none";
    record_queue_action(task, action);
}

void mark_task_dispatched(Task& task, int frame_index, bool resume_task) {
    task.dispatch_count++;
    task.last_scheduled_frame = frame_index;
    task.last_run_used_resume = resume_task;
    task.last_transition = resume_task ? "resumed" : "executing";
    task.last_interrupt_reason = "none";
    if (resume_task) {
        task.resume_count++;
    } else {
        task.execute_count++;
    }
}

Task* find_queued_task_by_id(int task_id) {
    auto find_in_queue = [task_id](std::deque<StoredTask>& queue) -> Task* {
        for (StoredTask& entry : queue) {
            if (entry.task && entry.task->id == task_id) {
                return entry.task.get();
            }
        }
        return nullptr;
    };

    if (Task* task = find_in_queue(g_p1_queue)) {
        return task;
    }
    if (Task* task = find_in_queue(g_p2_queue)) {
        return task;
    }
    return find_in_queue(g_p3_queue);
}

void update_queued_task_preemption_metadata(int task_id,
                                            const std::string& reason,
                                            const std::string& queue_action) {
    Task* task = find_queued_task_by_id(task_id);
    if (!task) {
        return;
    }
    task->last_interrupt_reason = reason;
    task->last_queue_action = queue_action;
}

void reconcile_runtime_particle_bookkeeping();

void enqueue_with_policy(std::unique_ptr<Task> task) {
    const PriorityLevel priority = task->priority;
    std::deque<StoredTask>& queue = queue_for_priority(task->priority);

    if (task->priority == PriorityLevel::P1_SYSTEM) {
        if (has_same_type(queue, task->type)) {
            queue.pop_back();
        } else if (queue.size() >= 2) {
            queue.pop_front();
        }
        queue.push_back({std::move(task)});
        return;
    }

    if (task->priority == PriorityLevel::P2_FUNCTIONAL) {
        if (!queue.empty() && queue.front().task->type == task->type) {
            queue.pop_front();
        } else if (queue.size() >= 5) {
            queue.pop_back();
        }
        queue.push_back({std::move(task)});
        return;
    }

    if (queue.size() >= 100) {
        queue.pop_front();
    }
    queue.push_back({std::move(task)});
    if (priority == PriorityLevel::P3_PARTICLE) {
        reconcile_runtime_particle_bookkeeping();
    }
}

void erase_p2_tasks_by_type(TaskType type) {
    auto it = std::remove_if(
        g_p2_queue.begin(),
        g_p2_queue.end(),
        [type](const StoredTask& entry) {
            return entry.task->type == type;
        });
    g_p2_queue.erase(it, g_p2_queue.end());
}

bool p2_queue_contains_only_batch_followup_tasks();
bool p2_queue_contains_preemptive_tasks();
void reconcile_runtime_particle_bookkeeping();
void seed_input_entry_task_if_idle();

bool task_eligible_this_frame(const StoredTask& entry) {
    return entry.task && entry.task->last_scheduled_frame != g_frame_index;
}

bool pop_next_eligible_task(std::deque<StoredTask>& queue,
                            bool from_back,
                            StoredTask& out) {
    if (from_back) {
        for (int index = static_cast<int>(queue.size()) - 1; index >= 0; --index) {
            if (!task_eligible_this_frame(queue[static_cast<std::size_t>(index)])) {
                continue;
            }
            out = std::move(queue[static_cast<std::size_t>(index)]);
            queue.erase(queue.begin() + index);
            return true;
        }
        return false;
    }

    for (std::size_t index = 0; index < queue.size(); ++index) {
        if (!task_eligible_this_frame(queue[index])) {
            continue;
        }
        out = std::move(queue[index]);
        queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }
    return false;
}

int single_thread_p2_phase_for(TaskType type) {
    switch (type) {
    case TaskType::CAMERA:
        return 0;
    case TaskType::MICROPHONE:
        return 1;
    case TaskType::GENERATE_PARTICLE:
    case TaskType::BREEZE:
    case TaskType::CHANGE_DANDELION:
        return 2;
    case TaskType::BATCH_PARTICLE_EXECUTION:
        return 3;
    default:
        return 3;
    }
}

bool pop_single_thread_p2_task(StoredTask& out) {
    auto pop_locked_type = [&out](TaskType locked_type) {
        for (int index = 0; index < static_cast<int>(g_p2_queue.size()); ++index) {
            const StoredTask& entry = g_p2_queue[static_cast<std::size_t>(index)];
            if (!task_eligible_this_frame(entry)) {
                continue;
            }
            if (entry.task->type != locked_type) {
                continue;
            }
            out = std::move(g_p2_queue[static_cast<std::size_t>(index)]);
            g_p2_queue.erase(g_p2_queue.begin() + index);
            g_single_thread_p2_phase = (single_thread_p2_phase_for(locked_type) + 1) % 4;
            return true;
        }
        return false;
    };

    RuntimeState& state_ref = runtime_state();
    if (state_ref.camera_focus_locked && pop_locked_type(TaskType::CAMERA)) {
        return true;
    }
    if (state_ref.microphone_focus_locked && pop_locked_type(TaskType::MICROPHONE)) {
        return true;
    }

    for (int phase_offset = 0; phase_offset < 4; ++phase_offset) {
        const int desired_phase = (g_single_thread_p2_phase + phase_offset) % 4;
        for (int index = 0; index < static_cast<int>(g_p2_queue.size()); ++index) {
            const StoredTask& entry = g_p2_queue[static_cast<std::size_t>(index)];
            if (!task_eligible_this_frame(entry)) {
                continue;
            }
            if (single_thread_p2_phase_for(entry.task->type) != desired_phase) {
                continue;
            }
            out = std::move(g_p2_queue[static_cast<std::size_t>(index)]);
            g_p2_queue.erase(g_p2_queue.begin() + index);
            g_single_thread_p2_phase = (desired_phase + 1) % 4;
            return true;
        }
    }
    return false;
}

bool take_next_task_for_dispatch(int dispatch_slot, int active_threads, StoredTask& out) {
    if (pop_next_eligible_task(g_p1_queue, true, out)) {
        return true;
    }

    const bool mixed_batch_and_p3 =
        !g_p3_queue.empty() && p2_queue_contains_only_batch_followup_tasks();
    if (mixed_batch_and_p3) {
        if (active_threads <= 1) {
            const bool prefer_p3 = g_single_thread_mixed_turn_prefers_p3;
            const bool selected = prefer_p3
                ? (pop_next_eligible_task(g_p3_queue, false, out) ||
                   pop_next_eligible_task(g_p2_queue, false, out))
                : (pop_next_eligible_task(g_p2_queue, false, out) ||
                   pop_next_eligible_task(g_p3_queue, false, out));
            if (selected) {
                g_single_thread_mixed_turn_prefers_p3 = !prefer_p3;
            }
            return selected;
        }

        if (dispatch_slot == 0) {
            if (pop_next_eligible_task(g_p2_queue, false, out)) {
                return true;
            }
            return pop_next_eligible_task(g_p3_queue, false, out);
        }

        if (pop_next_eligible_task(g_p3_queue, false, out)) {
            return true;
        }
        return pop_next_eligible_task(g_p2_queue, false, out);
    }

    if (active_threads <= 1) {
        if (pop_single_thread_p2_task(out)) {
            return true;
        }
        return pop_next_eligible_task(g_p3_queue, false, out);
    }

    if (pop_next_eligible_task(g_p2_queue, false, out)) {
        return true;
    }
    return pop_next_eligible_task(g_p3_queue, false, out);
}

bool has_pending_tasks() {
    return !g_p1_queue.empty() || !g_p2_queue.empty() || !g_p3_queue.empty();
}

int priority_rank(PriorityLevel priority) {
    switch (priority) {
    case PriorityLevel::P1_SYSTEM:
        return 3;
    case PriorityLevel::P2_FUNCTIONAL:
        return 2;
    case PriorityLevel::P3_PARTICLE:
        return 1;
    default:
        return 0;
    }
}

bool highest_pending_preemptor_priority(PriorityLevel running_priority,
                                        PriorityLevel& out_priority) {
    if (running_priority != PriorityLevel::P1_SYSTEM && !g_p1_queue.empty()) {
        out_priority = PriorityLevel::P1_SYSTEM;
        return true;
    }
    if (running_priority == PriorityLevel::P3_PARTICLE && p2_queue_contains_preemptive_tasks()) {
        out_priority = PriorityLevel::P2_FUNCTIONAL;
        return true;
    }
    return false;
}

std::string requeue_action_for(bool resumed, bool preempted) {
    if (preempted) {
        return resumed ? "resume-preempted-tail" : "preempted-tail";
    }
    return resumed ? "resume-tail" : "execute-tail";
}

int priority_rank_for_pending_preemption(const FrameDispatchEntry& entry) {
    return priority_rank(entry.priority);
}

int select_preemption_victim_index(const std::vector<FrameDispatchEntry>& frame_dispatches,
                                   PriorityLevel pending_priority) {
    const int pending_rank = priority_rank(pending_priority);
    int best_index = -1;
    int best_priority_rank = 1000;
    int best_dispatch_slot = 1000;

    for (int index = 0; index < static_cast<int>(frame_dispatches.size()); ++index) {
        const FrameDispatchEntry& entry = frame_dispatches[static_cast<std::size_t>(index)];
        const int running_rank = priority_rank_for_pending_preemption(entry);
        if (running_rank >= pending_rank) {
            continue;
        }
        if (running_rank < best_priority_rank ||
            (running_rank == best_priority_rank && entry.dispatch_slot < best_dispatch_slot)) {
            best_index = index;
            best_priority_rank = running_rank;
            best_dispatch_slot = entry.dispatch_slot;
        }
    }

    return best_index;
}

bool is_batch_followup_task_type(TaskType type) {
    return type == TaskType::BATCH_PARTICLE_EXECUTION;
}

bool p2_queue_contains_only_batch_followup_tasks() {
    if (g_p2_queue.empty()) {
        return false;
    }

    return std::all_of(
        g_p2_queue.begin(),
        g_p2_queue.end(),
        [](const StoredTask& entry) {
            return entry.task && is_batch_followup_task_type(entry.task->type);
        });
}

bool p2_queue_contains_preemptive_tasks() {
    return std::any_of(
        g_p2_queue.begin(),
        g_p2_queue.end(),
        [](const StoredTask& entry) {
            return entry.task && !is_batch_followup_task_type(entry.task->type);
        });
}

void refresh_device_availability_from_bridges() {
    refresh_bridge_inputs();
    RuntimeState& state_ref = runtime_state();
    state_ref.camera_device_available = state_ref.camera_bridge.bridge_connected;
    state_ref.microphone_device_available = state_ref.microphone_bridge.bridge_connected;
}

bool queues_all_empty() {
    return g_p1_queue.empty() && g_p2_queue.empty() && g_p3_queue.empty();
}

void queue_microphone_stage_if_needed() {
    if (!runtime_state().microphone_device_available) {
        return;
    }
    if (!has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL)) {
        submit_task(make_microphone_task());
    }
}

void queue_batch_stage_if_needed() {
    if (!has_queued_task(TaskType::BATCH_PARTICLE_EXECUTION, PriorityLevel::P2_FUNCTIONAL)) {
        submit_task(make_batch_particle_execution_task());
    }
}

void seed_input_entry_task_if_idle() {
    if (current_runtime_state().phase != RuntimePhase::READY || !queues_all_empty()) {
        return;
    }

    RuntimeState& state_ref = runtime_state();
    if (state_ref.camera_device_available) {
        submit_task(make_camera_task());
    } else if (state_ref.microphone_device_available) {
        submit_task(make_microphone_task());
    }
}

void rebuild_particle_ring_for_world(float center_x, float center_y, int count) {
    RenderData& data = render_data();
    data.particles.clear();
    data.particles.reserve(count);

    const float radius = 0.12f;
    for (int index = 0; index < count; ++index) {
        const float angle = static_cast<float>(index) * 6.2831853f /
            static_cast<float>(count == 0 ? 1 : count);
        ParticleRenderData particle;
        particle.id = index + 1;
        particle.x = center_x + std::cos(angle) * radius;
        particle.y = center_y + std::sin(angle) * radius;
        particle.attached = true;
        particle.active = false;
        particle.ownership_token = 0;
        particle.status = "idle";
        data.particles.push_back(particle);
    }
}

std::vector<int> collect_spawnable_particle_slots(int limit) {
    std::vector<int> slots;
    if (limit <= 0) {
        return slots;
    }

    const RenderData& data = current_render_data();
    slots.reserve(static_cast<std::size_t>(limit));
    for (int index = 0; index < static_cast<int>(data.particles.size()); ++index) {
        const ParticleRenderData& particle = data.particles[static_cast<std::size_t>(index)];
        if (!particle.attached || particle.active) {
            continue;
        }
        slots.push_back(index);
        if (static_cast<int>(slots.size()) >= limit) {
            break;
        }
    }
    return slots;
}

int count_attached_particles(const RenderData& data) {
    return static_cast<int>(std::count_if(
        data.particles.begin(),
        data.particles.end(),
        [](const ParticleRenderData& particle) {
            return particle.attached;
        }));
}

bool particle_slot_counts_as_queued_work(const ParticleRenderData& particle) {
    return !particle.attached &&
        particle.active &&
        particle.source_task_id > 0 &&
        (particle.status == "particle-execute" ||
         particle.status == "particle-resume");
}

bool p3_queue_contains_task_id(int task_id) {
    return std::any_of(
        g_p3_queue.begin(),
        g_p3_queue.end(),
        [task_id](const StoredTask& entry) {
            return entry.task && entry.task->id == task_id;
        });
}

int count_queued_particle_work(const RenderData& data) {
    const int active_running_slots = static_cast<int>(std::count_if(
        data.particles.begin(),
        data.particles.end(),
        [](const ParticleRenderData& particle) {
            return particle_slot_counts_as_queued_work(particle) &&
                !p3_queue_contains_task_id(particle.source_task_id);
        }));
    return static_cast<int>(g_p3_queue.size()) + active_running_slots;
}

void reconcile_particle_bookkeeping(RuntimeState& state_ref, RenderData& data) {
    state_ref.world.remaining_particles = count_attached_particles(data);
    state_ref.world.queued_particle_tasks = count_queued_particle_work(data);
    data.ui.remaining_particles = state_ref.world.remaining_particles;
    data.ui.queued_particle_tasks = state_ref.world.queued_particle_tasks;
}

void reconcile_runtime_particle_bookkeeping() {
    reconcile_particle_bookkeeping(runtime_state(), render_data());
}

}  // namespace

PlaceholderTask::PlaceholderTask(TaskType task_type,
                                 PriorityLevel task_priority,
                                 std::string task_name,
                                 bool resumable) {
    type = task_type;
    priority = task_priority;
    name = std::move(task_name);
    support_resume = resumable;
}

void PlaceholderTask::execute() {
    state = TaskState::FINISHED;
}

StartTask::StartTask() {
    type = TaskType::START;
    priority = PriorityLevel::P1_SYSTEM;
    name = "StartTask";
    support_resume = false;
}

void StartTask::execute() {
    set_runtime_phase(RuntimePhase::STARTING);
    push_runtime_note("StartTask: runtime bootstrap started.");
    submit_task(make_reset_task());
    state = TaskState::FINISHED;
}

ResetTask::ResetTask() {
    type = TaskType::RESET;
    priority = PriorityLevel::P1_SYSTEM;
    name = "ResetTask";
    support_resume = false;
}

void ResetTask::execute() {
    set_runtime_phase(RuntimePhase::RESETTING);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    reset_simulation_world();
    refresh_device_availability_from_bridges();
    set_runtime_phase(RuntimePhase::READY);
    seed_input_entry_task_if_idle();
    push_runtime_note("ResetTask: world reset and input entry reseeded.");
    state = TaskState::FINISHED;
}

ExitTask::ExitTask() {
    type = TaskType::EXIT_APP;
    priority = PriorityLevel::P1_SYSTEM;
    name = "ExitTask";
    support_resume = false;
}

void ExitTask::execute() {
    request_shutdown();
    set_visualization_running(false);
    push_runtime_note("ExitTask: shutdown requested.");
    state = TaskState::FINISHED;
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

void CameraTask::run_cycle(bool resumed) {
    ++cycle_count_;
    bool should_cancel_pending_blow_tasks = false;
    bool should_queue_microphone = false;
    bool stage_finished = false;
    with_shared_state_write(false, false, true, [this, resumed, &should_cancel_pending_blow_tasks, &should_queue_microphone, &stage_finished]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        const CameraBridgeState& bridge = state_ref.camera_bridge;
        const bool bridge_fresh = camera_bridge_is_fresh(bridge);
        const bool device_available = state_ref.camera_device_available;

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
        data.camera_layer.mouth_detected =
            state_ref.camera_available &&
            bridge.mouth_open_state &&
            bridge.looking_forward;
        if (!device_available) {
            state_ref.camera_gate_open = false;
            state_ref.camera_gate_frame = -1;
            state_ref.camera_gate_until_ms = 0;
            state_ref.camera_focus_locked = false;
            state_ref.microphone_focus_locked = state_ref.microphone_device_available;
            state_ref.microphone_focus_until_ms =
                state_ref.microphone_device_available ? current_time_ms() + kCameraGateHoldMs : 0;
            state_ref.microphone_focus_consumed_for_gate = false;
            data.camera_layer.status = "camera-device-unavailable";
            data.ui.camera_task_status = "CAM unavailable, handoff";
            should_queue_microphone = state_ref.microphone_device_available;
            stage_finished = true;
        } else if (data.camera_layer.mouth_detected) {
            state_ref.camera_gate_open = true;
            state_ref.camera_gate_frame = g_frame_index;
            state_ref.camera_gate_until_ms = current_time_ms() + kCameraGateHoldMs;
            state_ref.microphone_focus_consumed_for_gate = false;
            state_ref.camera_focus_locked = false;
            state_ref.microphone_focus_locked = true;
            state_ref.microphone_focus_until_ms = current_time_ms() + kCameraGateHoldMs;
            should_queue_microphone = state_ref.microphone_device_available;
            stage_finished = true;
        } else if (state_ref.camera_gate_until_ms > 0 &&
                   current_time_ms() <= state_ref.camera_gate_until_ms &&
                   bridge.bridge_connected &&
                   bridge.sample_ready &&
                   bridge_fresh &&
                   bridge.face_detected) {
            state_ref.camera_gate_open = true;
            state_ref.camera_focus_locked = false;
            if (!state_ref.microphone_focus_consumed_for_gate) {
                state_ref.microphone_focus_locked = true;
                state_ref.microphone_focus_until_ms =
                    std::max(state_ref.microphone_focus_until_ms, current_time_ms() + kCameraGateHoldMs);
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
        if (!device_available) {
            data.camera_layer.status = "camera-device-unavailable";
            data.ui.camera_task_status = "CAM unavailable, handoff";
        } else if (!bridge.bridge_connected) {
            data.camera_layer.status = "camera-bridge-disconnected";
            data.ui.camera_task_status = bridge.status_text.empty()
                ? "CAM bridge disconnected"
                : "CAM " + bridge.status_text;
            state_ref.camera_focus_locked = false;
            state_ref.microphone_focus_locked = true;
            state_ref.microphone_focus_until_ms = current_time_ms() + kCameraGateHoldMs;
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
            data.camera_layer.status = "camera-bridge-no-face";
            data.ui.camera_task_status = bridge.status_text.empty()
                ? "CAM sample ready, no face"
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
        data.ui.input_focus_status = build_input_focus_status(state_ref);
    });

    if (should_queue_microphone) {
        queue_microphone_stage_if_needed();
    }

    if (should_cancel_pending_blow_tasks) {
        const bool had_generate =
            has_queued_task(TaskType::GENERATE_PARTICLE, PriorityLevel::P2_FUNCTIONAL);
        const bool had_breeze =
            has_queued_task(TaskType::BREEZE, PriorityLevel::P2_FUNCTIONAL);
        erase_p2_tasks_by_type(TaskType::GENERATE_PARTICLE);
        erase_p2_tasks_by_type(TaskType::BREEZE);
        if (had_generate || had_breeze) {
            push_runtime_note("CameraTask: mouth closed, cancelled pending blow tasks.");
        }
    }

    if (stage_finished) {
        push_runtime_note("CameraTask: stage completed.");
    }

    state = stage_finished ? TaskState::FINISHED : TaskState::RUNNING;
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

void MicrophoneTask::run_cycle(bool resumed) {
    ++sample_count_;
    bool should_queue_generate = false;
    bool should_queue_breeze = false;
    bool stage_finished = false;
    with_shared_state_write(false, true, true, [this, resumed, &should_queue_generate, &should_queue_breeze, &stage_finished]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        const MicrophoneBridgeState& bridge = state_ref.microphone_bridge;
        const bool camera_gate_ready = camera_gate_is_fresh(state_ref);
        const bool microphone_focus_ready =
            state_ref.microphone_focus_locked &&
            state_ref.microphone_focus_until_ms > 0 &&
            current_time_ms() <= state_ref.microphone_focus_until_ms;
        const bool input_window_ready =
            !state_ref.camera_device_available || camera_gate_ready || microphone_focus_ready;
        const bool bridge_fresh = microphone_bridge_is_fresh(bridge);
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
        if (!state_ref.microphone_device_available) {
            data.wind_layer.status = "mic-device-unavailable";
            data.ui.microphone_task_status = "MIC unavailable";
            stage_finished = true;
        } else if (!input_window_ready) {
            data.wind_layer.status = "mic-waiting-for-camera";
            data.ui.microphone_task_status = "MIC waiting for camera gate";
        } else if (!bridge.bridge_connected) {
            data.wind_layer.status = "mic-bridge-disconnected";
            data.ui.microphone_task_status = "MIC bridge disconnected";
        } else if (!bridge.sample_ready) {
            data.wind_layer.status = "mic-bridge-waiting";
            data.ui.microphone_task_status = "MIC waiting for bridge sample";
        } else if (!bridge_fresh) {
            data.wind_layer.status = "mic-bridge-stale";
            data.ui.microphone_task_status = "MIC bridge sample stale";
        } else if (bridge.fallback_requested) {
            data.wind_layer.status = "mic-bridge-fallback";
            data.ui.microphone_task_status = "MIC requested breeze fallback";
        } else if (!bridge.voice_detected) {
            data.wind_layer.status = "mic-bridge-silent";
            data.ui.microphone_task_status = "MIC sample ready, no voice";
        } else {
            data.wind_layer.status = resumed ? "mic-bridge-resume" : "mic-bridge-active";
            data.ui.microphone_task_status =
                "MIC bridge active " + bridge.backend +
                " power " + std::to_string(static_cast<int>(state_ref.world.power * 100.0f) / 100.0f);
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
            state_ref.microphone_focus_until_ms = current_time_ms() + kCameraGateHoldMs;
        }
        data.ui.input_focus_status = build_input_focus_status(state_ref);
    });

    if (should_queue_generate) {
        submit_task(make_generate_particle_task());
    }
    if (should_queue_breeze) {
        submit_task(make_breeze_task());
    }

    if (stage_finished) {
        push_runtime_note("MicrophoneTask: stage completed.");
    }

    state = stage_finished ? TaskState::FINISHED : TaskState::RUNNING;
}

GenerateParticleTask::GenerateParticleTask() {
    type = TaskType::GENERATE_PARTICLE;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "GenerateParticleTask";
    support_resume = false;
}

int GenerateParticleTask::compute_spawn_count(float power) const {
    if (power < 0.20f) {
        return 1;
    }
    if (power < 0.30f) {
        return 2;
    }
    if (power < 0.40f) {
        return 3;
    }
    return 4;
}

void GenerateParticleTask::execute() {
    int created = 0;
    with_shared_state_write(true, true, true, [this, &created]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        const int spawn_target = compute_spawn_count(state_ref.world.power);
        const std::vector<int> spawn_slots = collect_spawnable_particle_slots(
            std::min(spawn_target, state_ref.world.remaining_particles));

        for (int index = 0; index < static_cast<int>(spawn_slots.size()); ++index) {
            const int slot = spawn_slots[static_cast<std::size_t>(index)];
            ParticleRenderData& particle = data.particles[static_cast<std::size_t>(slot)];
            particle.ownership_token++;
            submit_task(make_single_particle_task(slot, index + 1, particle.ownership_token));
            particle.active = true;
            particle.attached = false;
            particle.status = "queued";
            created++;
        }

        reconcile_particle_bookkeeping(state_ref, data);
        data.ui.generate_task_status =
            "GENERATE created " + std::to_string(created) + " particle tasks";
        data.ui.particle_task_status =
            created > 0 ? "P3 queue populated" : "P3 queue unchanged";
    });

    if (created > 0) {
        queue_batch_stage_if_needed();
    }
    push_runtime_note(
        "GenerateParticleTask: queued " + std::to_string(created) + " particle task records.");
    state = TaskState::FINISHED;
}

BreezeTask::BreezeTask() {
    type = TaskType::BREEZE;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "BreezeTask";
    support_resume = false;
}

void BreezeTask::execute() {
    bool created_particle = false;
    with_shared_state_write(true, true, true, [&created_particle]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        state_ref.world.power = 5.0f;
        data.wind_layer.active = true;
        data.wind_layer.power = state_ref.world.power;
        data.wind_layer.status = "breeze-fallback";
        data.ui.power = state_ref.world.power;

        const std::vector<int> spawn_slots = collect_spawnable_particle_slots(1);
        if (state_ref.world.remaining_particles > 0 &&
            !spawn_slots.empty() &&
            !has_queued_task(TaskType::SINGLE_PARTICLE, PriorityLevel::P3_PARTICLE)) {
            const int slot = spawn_slots.front();
            ParticleRenderData& particle = data.particles[static_cast<std::size_t>(slot)];
            particle.ownership_token++;
            submit_task(make_single_particle_task(slot, 1, particle.ownership_token));
            particle.active = true;
            particle.attached = false;
            particle.status = "breeze-queued";
            reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status = "BREEZE queued fallback particle";
            created_particle = true;
        } else {
            reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status = "BREEZE had no particle slot";
        }

        data.ui.generate_task_status =
            created_particle ? "BREEZE fallback created 1 particle task"
                             : "BREEZE fallback created 0 particle tasks";
    });

    if (created_particle) {
        queue_batch_stage_if_needed();
    }
    push_runtime_note(created_particle
        ? "BreezeTask: fallback wind queued one particle task."
        : "BreezeTask: fallback wind ran without queuing a particle.");
    state = TaskState::FINISHED;
}

ChangeDandelionTask::ChangeDandelionTask() {
    type = TaskType::CHANGE_DANDELION;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "ChangeDandelionTask";
    support_resume = false;
}

void ChangeDandelionTask::execute() {
    ++g_dandelion_layout_revision;
    const int revision = g_dandelion_layout_revision % 3;

    with_shared_state_write(true, true, true, [revision]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        float next_x = 0.5f;
        float next_y = 0.5f;
        if (revision == 1) {
            next_x = 0.42f;
            next_y = 0.52f;
        } else if (revision == 2) {
            next_x = 0.58f;
            next_y = 0.48f;
        }

        state_ref.world.dandelion_x = next_x;
        state_ref.world.dandelion_y = next_y;
        state_ref.world.mouth_x = next_x;
        state_ref.world.mouth_y = next_y;
        state_ref.world.remaining_particles = 100;
        state_ref.world.queued_particle_tasks = 0;
        state_ref.world.power = 0.1f;

        data.camera_layer.mouth_x = next_x;
        data.camera_layer.mouth_y = next_y;
        data.wind_layer.active = false;
        data.wind_layer.power = state_ref.world.power;
        data.wind_layer.status = "idle";
        data.ui.power = state_ref.world.power;
        data.ui.generate_task_status = "CHANGE rebuilt dandelion";
        data.ui.particle_task_status = "CHANGE reset particle ring";

        rebuild_particle_ring_for_world(next_x, next_y, 100);
        reconcile_particle_bookkeeping(state_ref, data);
    });

    clear_task_queue(PriorityLevel::P3_PARTICLE);
    push_runtime_note("ChangeDandelionTask: rebuilt dandelion cluster and reset particle state.");
    state = TaskState::FINISHED;
}

BatchParticleExecutionTask::BatchParticleExecutionTask() {
    type = TaskType::BATCH_PARTICLE_EXECUTION;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "BatchParticleExecutionTask";
    support_resume = true;
}

void BatchParticleExecutionTask::execute() {
    run_cycle(false);
}

void BatchParticleExecutionTask::resume() {
    run_cycle(true);
}

void BatchParticleExecutionTask::run_cycle(bool resumed) {
    ++tick_count_;
    if (!started_) {
        started_ = true;
    }
    decay_accumulator_ms_ += kRrQuantumMs;
    bool stage_finished = false;
    with_shared_state_write(true, true, true, [this, resumed, &stage_finished]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        bool power_decayed = false;
        int cleaned_particles = 0;

        if (decay_accumulator_ms_ >= 1000 && state_ref.world.power > 0.1f) {
            state_ref.world.power = std::max(0.1f, state_ref.world.power - 0.01f);
            decay_accumulator_ms_ -= 1000;
            power_decayed = true;
        }

        for (ParticleRenderData& particle : data.particles) {
            const bool finished_detached =
                !particle.active &&
                !particle.attached &&
                particle.source_task_id == -1 &&
                (particle.status == "particle-finished" ||
                 particle.status == "particle-boundary-stop");
            if (!finished_detached) {
                continue;
            }

            particle.x = -1.0f;
            particle.y = -1.0f;
            particle.status = "particle-cleaned";
            cleaned_particles++;
        }

        reconcile_particle_bookkeeping(state_ref, data);

        const bool has_cleanup_pending = std::any_of(
            data.particles.begin(),
            data.particles.end(),
            [](const ParticleRenderData& particle) {
                return !particle.active &&
                    !particle.attached &&
                    particle.source_task_id == -1 &&
                    (particle.status == "particle-finished" ||
                     particle.status == "particle-boundary-stop");
            });
        const bool has_active_particle_motion = std::any_of(
            data.particles.begin(),
            data.particles.end(),
            [](const ParticleRenderData& particle) {
                return !particle.attached && particle.active;
            });
        stage_finished =
            !has_cleanup_pending &&
            !has_active_particle_motion &&
            state_ref.world.queued_particle_tasks == 0;

        data.wind_layer.active = false;
        data.wind_layer.power = state_ref.world.power;
        data.wind_layer.tick_count = tick_count_;
        if (cleaned_particles > 0) {
            data.wind_layer.status = "world-tick-cleanup";
        } else if (power_decayed) {
            data.wind_layer.status = "world-tick-decay";
        } else if (tick_count_ == 1) {
            data.wind_layer.status = "world-tick-start";
        } else {
            data.wind_layer.status = resumed ? "batch-resume" : "batch-execute";
        }
        data.ui.power = state_ref.world.power;
        if (cleaned_particles > 0) {
            data.ui.batch_task_status =
                "BATCH cleaned " + std::to_string(cleaned_particles) + " particle render records";
        } else if (power_decayed) {
            data.ui.batch_task_status =
                "BATCH decayed power to " +
                std::to_string(static_cast<int>(state_ref.world.power * 100.0f) / 100.0f);
        } else if (tick_count_ == 1) {
            data.ui.batch_task_status = "BATCH recorded initial world tick";
        } else if (stage_finished) {
            data.ui.batch_task_status = "BATCH finished world pass";
        } else {
            data.ui.batch_task_status = "BATCH tick " + std::to_string(tick_count_);
        }
    });

    if (stage_finished) {
        push_runtime_note("BatchParticleExecutionTask: stage completed.");
    }

    state = stage_finished ? TaskState::FINISHED : TaskState::RUNNING;
}

SingleParticleTask::SingleParticleTask(int particle_slot,
                                       int generation_index,
                                       int ownership_token)
    : particle_slot_(particle_slot),
      generation_index_(generation_index),
      ownership_token_(ownership_token) {
    type = TaskType::SINGLE_PARTICLE;
    priority = PriorityLevel::P3_PARTICLE;
    name = "SingleParticleTask";
    support_resume = true;
}

void SingleParticleTask::execute() {
    run_cycle(false);
}

void SingleParticleTask::resume() {
    run_cycle(true);
}

void SingleParticleTask::run_cycle(bool resumed) {
    ++cycle_count_;
    completed_ = false;
    with_shared_state_write(true, false, true, [this, resumed]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        if (particle_slot_ >= 0 &&
            particle_slot_ < static_cast<int>(data.particles.size())) {
            ParticleRenderData& particle = data.particles[particle_slot_];
            if (particle.ownership_token != ownership_token_) {
                completed_ = true;
                reconcile_particle_bookkeeping(state_ref, data);
                data.ui.particle_task_status =
                    "P3 task " + std::to_string(id) + " lost slot ownership";
                return;
            }
            if (!initialized_) {
                x_ = particle.x;
                y_ = particle.y;
                const float from_center_x = x_ - state_ref.world.dandelion_x;
                const float from_center_y = y_ - state_ref.world.dandelion_y;
                const float from_mouth_x = x_ - state_ref.world.mouth_x;
                const float from_mouth_y = y_ - state_ref.world.mouth_y;
                float base_x = from_center_x;
                float base_y = from_center_y;
                if (std::abs(base_x) + std::abs(base_y) < 0.001f) {
                    base_x = from_mouth_x;
                    base_y = from_mouth_y;
                }
                if (std::abs(base_x) + std::abs(base_y) < 0.001f) {
                    base_x = 1.0f;
                    base_y = 0.0f;
                }

                float length = std::sqrt(base_x * base_x + base_y * base_y);
                if (length <= 0.0001f) {
                    length = 1.0f;
                }
                base_x /= length;
                base_y /= length;

                const float spread = (static_cast<float>(generation_index_) - 2.0f) * 0.18f;
                dir_x_ = base_x + (-base_y * spread);
                dir_y_ = base_y + (base_x * spread);

                float dir_length = std::sqrt(dir_x_ * dir_x_ + dir_y_ * dir_y_);
                if (dir_length <= 0.0001f) {
                    dir_x_ = base_x;
                    dir_y_ = base_y;
                    dir_length = 1.0f;
                }
                dir_x_ /= dir_length;
                dir_y_ /= dir_length;

                const float velocity_pixels_per_second =
                    std::clamp(state_ref.world.power, 0.1f, 10.0f) *
                    kVelocityPerPowerPixelsPerSecond;
                distance_per_tick_ =
                    velocity_pixels_per_second *
                    (static_cast<float>(kRrQuantumMs) / 1000.0f) /
                    kNormalizedWorldPixels;
                initialized_ = true;
            }

            x_ += dir_x_ * distance_per_tick_;
            y_ += dir_y_ * distance_per_tick_;
            particle.source_task_id = id;
            particle.x = x_;
            particle.y = y_;
            particle.active = true;
            particle.status = resumed ? "particle-resume" : "particle-execute";

            const bool boundary_stop =
                x_ < 0.0f || x_ > 1.0f || y_ < 0.0f || y_ > 1.0f;

            if (boundary_stop) {
                completed_ = true;
                particle.active = false;
                particle.attached = false;
                particle.source_task_id = -1;
                particle.status = "particle-boundary-stop";
                reconcile_particle_bookkeeping(state_ref, data);
                data.ui.particle_task_status =
                    "P3 task " + std::to_string(id) + " boundary stop cleanup";
            } else {
                reconcile_particle_bookkeeping(state_ref, data);
                data.ui.particle_task_status =
                    "P3 task " + std::to_string(id) +
                    " step " + std::to_string(cycle_count_);
            }
        } else {
            reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status =
                "P3 task " + std::to_string(id) + " lost particle slot";
        }
    });

    state = completed_ ? TaskState::FINISHED : TaskState::RUNNING;
}

void bootstrap_runtime() {
    g_next_task_id = 1;
    g_frame_index = 0;
    g_thread_mode = 1;
    g_single_thread_mixed_turn_prefers_p3 = false;
    g_single_thread_p2_phase = 0;
    g_p1_queue.clear();
    g_p2_queue.clear();
    g_p3_queue.clear();
    rebuild_thread_pool();
    runtime_state() = RuntimeState{};
    set_runtime_phase(RuntimePhase::BOOTSTRAP);
    reset_simulation_world();
}

void reset_runtime() {
    g_frame_index = 0;
    g_single_thread_mixed_turn_prefers_p3 = false;
    g_single_thread_p2_phase = 0;
    g_p1_queue.clear();
    g_p2_queue.clear();
    g_p3_queue.clear();
    rebuild_thread_pool();
    runtime_state().shutdown_requested = false;
    set_runtime_phase(RuntimePhase::BOOTSTRAP);
    reset_simulation_world();
}

void seed_startup_flow() {
    submit_task(make_start_task());
}

int submit_task(std::unique_ptr<Task> task) {
    if (!task) {
        return -1;
    }

    task->id = g_next_task_id++;
    mark_task_created(*task);
    enqueue_with_policy(std::move(task));
    return g_next_task_id - 1;
}

std::unique_ptr<Task> make_placeholder_task(TaskType type,
                                            PriorityLevel priority,
                                            const std::string& name,
                                            bool support_resume) {
    return std::make_unique<PlaceholderTask>(type, priority, name, support_resume);
}

std::unique_ptr<Task> make_start_task() {
    return std::make_unique<StartTask>();
}

std::unique_ptr<Task> make_reset_task() {
    return std::make_unique<ResetTask>();
}

std::unique_ptr<Task> make_exit_task() {
    return std::make_unique<ExitTask>();
}

std::unique_ptr<Task> make_camera_task() {
    return std::make_unique<CameraTask>();
}

std::unique_ptr<Task> make_microphone_task() {
    return std::make_unique<MicrophoneTask>();
}

std::unique_ptr<Task> make_generate_particle_task() {
    return std::make_unique<GenerateParticleTask>();
}

std::unique_ptr<Task> make_breeze_task() {
    return std::make_unique<BreezeTask>();
}

std::unique_ptr<Task> make_change_dandelion_task() {
    return std::make_unique<ChangeDandelionTask>();
}

std::unique_ptr<Task> make_batch_particle_execution_task() {
    return std::make_unique<BatchParticleExecutionTask>();
}

std::unique_ptr<Task> make_single_particle_task(int particle_slot,
                                                int generation_index,
                                                int ownership_token) {
    return std::make_unique<SingleParticleTask>(
        particle_slot,
        generation_index,
        ownership_token);
}

void set_thread_mode(int mode) {
    g_thread_mode = std::clamp(mode, 1, 3);
    if (g_thread_mode == 1) {
        g_single_thread_p2_phase = 0;
    }
    if (g_threads.empty()) {
        rebuild_thread_pool();
    }
    apply_thread_mode_state(g_thread_mode);
    with_shared_state_write(false, false, true, []() {
        render_data().ui.thread_mode = current_thread_mode();
    });
}

int current_thread_mode() {
    return g_thread_mode;
}

void scheduler_tick() {
    ++g_frame_index;
    const int active_threads = std::clamp(g_thread_mode, 1, 3);
    int dispatch_slot = 0;
    std::vector<FrameDispatchEntry> frame_dispatches;
    int selected_preemption_victim_index = -1;

    for (RuntimeThread& thread : g_threads) {
        if (thread.state == ThreadState::WAITING) {
            thread.state = ThreadState::IDLE;
            thread.bound_task_id = -1;
            thread.bound_task_name = "none";
            thread.last_task_event = "waiting-ready";
        } else if (thread.state != ThreadState::SLEEPING && thread.state != ThreadState::CLOSED) {
            thread.state = ThreadState::IDLE;
            thread.bound_task_id = -1;
            thread.bound_task_name = "none";
            thread.last_task_event = "idle";
        }
    }

    if (!has_pending_tasks()) {
        seed_input_entry_task_if_idle();
    }

    if (!has_pending_tasks()) {
        reconcile_runtime_particle_bookkeeping();
        with_shared_state_write(false, false, true, []() {
            render_data().ui.scheduler_state = "Idle";
        });
        return;
    }

    for (RuntimeThread& thread : g_threads) {
        if (thread.state == ThreadState::SLEEPING || thread.state == ThreadState::CLOSED) {
            continue;
        }
        if (!has_pending_tasks()) {
            break;
        }

        StoredTask entry;
        if (!take_next_task_for_dispatch(dispatch_slot, active_threads, entry)) {
            break;
        }
        Task& task = *entry.task;
        const bool resume_task =
            task.state == TaskState::REQUEUED || task.state == TaskState::INTERRUPTED;
        thread.state = ThreadState::RUNNING;
        thread.bound_task_id = task.id;
        thread.bound_task_name = task_debug_label(task);
        thread.dispatch_count++;
        thread.last_task_event = resume_task ? "resume-dispatch" : "execute-dispatch";
        thread.last_task_transition = task.last_transition;
        push_runtime_note(
            "Thread " + std::to_string(thread.id) + " " +
            (resume_task ? "resume " : "execute ") + task_debug_label(task));
        mark_task_dispatched(task, g_frame_index, resume_task);
        task.state = TaskState::RUNNING;
        if (resume_task) {
            task.resume();
        } else {
            task.execute();
        }

        if (task.state != TaskState::FINISHED && task.support_resume) {
            mark_task_interrupted(task, "timeslice-expired", g_frame_index);
            mark_task_requeued(task, requeue_action_for(resume_task, false));
            enqueue_with_policy(std::move(entry.task));
            frame_dispatches.push_back({
                task.id,
                thread.id - 1,
                dispatch_slot,
                task.priority,
                resume_task,
            });
            thread.state = ThreadState::WAITING;
            thread.last_task_event = "interrupted-requeued";
            thread.last_task_transition = task.last_transition;
            push_runtime_note(
                "Thread " + std::to_string(thread.id) + " yielded " +
                task_debug_label(task) + " -> requeued");

            PriorityLevel pending_priority = PriorityLevel::P3_PARTICLE;
            if (highest_pending_preemptor_priority(task.priority, pending_priority)) {
                const int victim_index =
                    select_preemption_victim_index(frame_dispatches, pending_priority);
                if (victim_index >= 0) {
                    if (selected_preemption_victim_index >= 0 &&
                        selected_preemption_victim_index != victim_index) {
                        const FrameDispatchEntry& previous =
                            frame_dispatches[static_cast<std::size_t>(selected_preemption_victim_index)];
                        update_queued_task_preemption_metadata(
                            previous.task_id,
                            "timeslice-expired",
                            requeue_action_for(previous.used_resume, false));
                        if (previous.thread_index >= 0 &&
                            previous.thread_index < static_cast<int>(g_threads.size())) {
                            g_threads[static_cast<std::size_t>(previous.thread_index)].last_task_event =
                                "interrupted-requeued";
                            g_threads[static_cast<std::size_t>(previous.thread_index)].last_task_transition =
                                "requeued";
                        }
                    }

                    const FrameDispatchEntry& victim =
                        frame_dispatches[static_cast<std::size_t>(victim_index)];
                    update_queued_task_preemption_metadata(
                        victim.task_id,
                        pending_priority == PriorityLevel::P1_SYSTEM
                            ? "preempted-by-p1"
                            : "preempted-by-p2",
                        requeue_action_for(victim.used_resume, true));
                    if (victim.thread_index >= 0 &&
                        victim.thread_index < static_cast<int>(g_threads.size())) {
                        g_threads[static_cast<std::size_t>(victim.thread_index)].last_task_event =
                            "preempted-requeued";
                        g_threads[static_cast<std::size_t>(victim.thread_index)].last_task_transition =
                            "preempted";
                        if (Task* victim_task = find_queued_task_by_id(victim.task_id)) {
                            g_threads[static_cast<std::size_t>(victim.thread_index)].bound_task_name =
                                task_debug_label(*victim_task);
                            push_runtime_note(
                                "Thread " +
                                std::to_string(g_threads[static_cast<std::size_t>(victim.thread_index)].id) +
                                " preempted " + task_debug_label(*victim_task));
                        }
                    }
                    selected_preemption_victim_index = victim_index;
                }
            }
        } else if (task.state == TaskState::RUNNING) {
            mark_task_finished(task, g_frame_index, "finished-no-resume");
            thread.state = ThreadState::IDLE;
            thread.last_task_event = "finished";
            thread.last_task_transition = task.last_transition;
            push_runtime_note(
                "Thread " + std::to_string(thread.id) + " finished " + task_debug_label(task));
        } else if (task.state == TaskState::FINISHED) {
            mark_task_finished(task, g_frame_index, "finished");
            thread.state = ThreadState::IDLE;
            thread.last_task_event = "finished";
            thread.last_task_transition = task.last_transition;
            push_runtime_note(
                "Thread " + std::to_string(thread.id) + " finished " + task_debug_label(task));
        }
        thread.last_completed_task_id = task.id;
        thread.last_completed_task_name = task_debug_label(task);
        dispatch_slot++;

        if (task.type == TaskType::RESET) {
            mark_noncurrent_runnable_threads_waiting(thread.id, "reset-waiting");
            break;
        }
    }

    reconcile_runtime_particle_bookkeeping();
    with_shared_state_write(false, false, true, []() {
        render_data().ui.scheduler_state = has_pending_tasks() ? "Dispatching" : "Idle";
    });
}

const std::vector<RuntimeThread>& runtime_threads() {
    return g_threads;
}

const std::vector<TaskRecord>& queued_p1_tasks() {
    static std::vector<TaskRecord> snapshot;
    snapshot = snapshot_queue(g_p1_queue);
    return snapshot;
}

const std::vector<TaskRecord>& queued_p2_tasks() {
    static std::vector<TaskRecord> snapshot;
    snapshot = snapshot_queue(g_p2_queue);
    return snapshot;
}

const std::vector<TaskRecord>& queued_p3_tasks() {
    static std::vector<TaskRecord> snapshot;
    snapshot = snapshot_queue(g_p3_queue);
    return snapshot;
}

SchedulerSnapshot scheduler_snapshot() {
    SchedulerSnapshot snapshot;
    snapshot.frame_index = g_frame_index;
    snapshot.thread_mode = g_thread_mode;
    snapshot.visualization_enabled = g_visualization_enabled;
    snapshot.shutdown_requested = current_runtime_state().shutdown_requested;
    snapshot.remaining_particles = current_render_data().ui.remaining_particles;
    snapshot.power = current_render_data().ui.power;
    snapshot.threads = g_threads;
    snapshot.p1_queue = snapshot_queue(g_p1_queue);
    snapshot.p2_queue = snapshot_queue(g_p2_queue);
    snapshot.p3_queue = snapshot_queue(g_p3_queue);
    return snapshot;
}

void set_visualization_running(bool running) {
    g_visualization_enabled = running;
}

void clear_task_queue(PriorityLevel priority) {
    queue_for_priority(priority).clear();
    if (priority == PriorityLevel::P3_PARTICLE) {
        reconcile_runtime_particle_bookkeeping();
    }
}

bool has_queued_task(TaskType type, PriorityLevel priority) {
    const std::deque<StoredTask>& queue = queue_for_priority(priority);
    for (const StoredTask& entry : queue) {
        if (entry.task->type == type) {
            return true;
        }
    }
    return false;
}
