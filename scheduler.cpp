#include "thread.h"
#include "scheduler_task_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

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

bool queues_all_empty() {
    return g_p1_queue.empty() && g_p2_queue.empty() && g_p3_queue.empty();
}

void queue_microphone_stage_if_needed() {
    if (!runtime_state().microphone_device_available && !microphone_bridge_enabled()) {
        return;
    }
    if (!has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL)) {
        submit_task(make_microphone_task());
    }
}

void queue_camera_stage_if_needed() {
    if (!camera_bridge_enabled() && !runtime_state().camera_device_available) {
        return;
    }
    if (!has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL)) {
        submit_task(make_camera_task());
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
    if (camera_bridge_enabled() || state_ref.camera_device_available) {
        submit_task(make_camera_task());
    } else if (microphone_bridge_enabled() || state_ref.microphone_device_available) {
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

namespace scheduler_task_support {

long long current_time_ms() {
    return ::current_time_ms();
}

bool camera_bridge_is_fresh(const CameraBridgeState& bridge) {
    return ::camera_bridge_is_fresh(bridge);
}

bool microphone_bridge_is_fresh(const MicrophoneBridgeState& bridge) {
    return ::microphone_bridge_is_fresh(bridge);
}

bool camera_gate_is_fresh(const RuntimeState& state) {
    return ::camera_gate_is_fresh(state);
}

std::string build_input_focus_status(const RuntimeState& state) {
    return ::build_input_focus_status(state);
}

void erase_p2_tasks_by_type(TaskType type) {
    ::erase_p2_tasks_by_type(type);
}

void queue_microphone_stage_if_needed() {
    ::queue_microphone_stage_if_needed();
}

void queue_camera_stage_if_needed() {
    ::queue_camera_stage_if_needed();
}

void queue_batch_stage_if_needed() {
    ::queue_batch_stage_if_needed();
}

void seed_input_entry_task_if_idle() {
    ::seed_input_entry_task_if_idle();
}

int current_scheduler_frame_index() {
    return g_frame_index;
}

void rebuild_particle_ring_for_world(float center_x, float center_y, int count) {
    ::rebuild_particle_ring_for_world(center_x, center_y, count);
}

std::vector<int> collect_spawnable_particle_slots(int limit) {
    return ::collect_spawnable_particle_slots(limit);
}

void reconcile_particle_bookkeeping(RuntimeState& state_ref, RenderData& data) {
    ::reconcile_particle_bookkeeping(state_ref, data);
}

void reconcile_runtime_particle_bookkeeping() {
    ::reconcile_runtime_particle_bookkeeping();
}

}  // namespace scheduler_task_support

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
