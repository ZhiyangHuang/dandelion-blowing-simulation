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

struct P3FairnessMetrics {
    int move_queued = 0;
    int fade_queued = 0;
    int move_running = 0;
    int fade_running = 0;

    int total() const {
        return move_queued + fade_queued + move_running + fade_running;
    }
};

struct SingleThreadChainDiagnostics {
    bool active = false;
    std::string current = "n/a";
    std::string next = "n/a";
    std::string status = "n/a";
};

struct L1RealtimeDiagnostics {
    bool active = false;
    std::string camera_lane = "n/a";
    std::string microphone_lane = "n/a";
    std::string gate_lane = "n/a";
    std::string blocked_reason = "n/a";
};

constexpr long long kCameraHeartbeatTimeoutMs = 2000;
constexpr long long kMicrophoneHeartbeatTimeoutMs = 1600;

int g_next_task_id = 1;
int g_frame_index = 0;
int g_thread_mode = 1;
bool g_visualization_enabled = false;
bool g_single_thread_mixed_turn_prefers_p3 = false;
int g_single_thread_p2_phase = 0;
int g_last_multithread_camera_reseed_note_frame = -1000;
int g_last_multithread_microphone_reseed_note_frame = -1000;
TaskType g_last_multithread_realtime_slice_type = TaskType::NONE;
std::vector<RuntimeThread> g_threads;
std::deque<StoredTask> g_p1_queue;
std::deque<StoredTask> g_p2_realtime_queue;
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

bool microphone_window_is_fresh(const RuntimeState& state) {
    return state.microphone_focus_locked &&
        state.microphone_focus_until_ms > 0 &&
        current_time_ms() <= state.microphone_focus_until_ms;
}

std::string build_input_focus_status(const RuntimeState& state) {
    if (g_thread_mode != 1) {
        if (state.camera_focus_locked) {
            return "CAMERA WINDOW";
        }
        if (state.microphone_focus_locked) {
            return "MIC WINDOW";
        }
        if (state.camera_gate_open) {
            return "GATE OPEN";
        }
        return "FREE WINDOW";
    }

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

const char* scheduler_queue_label_impl(PriorityLevel priority) {
    switch (priority) {
    case PriorityLevel::P1_SYSTEM:
        return "P1";
    case PriorityLevel::P2_REALTIME:
        return "P2";
    case PriorityLevel::P2_FUNCTIONAL:
        return "P3";
    case PriorityLevel::P3_PARTICLE:
        return "P4";
    default:
        return "P?";
    }
}

const char* latency_class_label_impl(TaskType type, PriorityLevel priority) {
    if (priority == PriorityLevel::P2_REALTIME) {
        return "L1 Persistent Listen";
    }

    switch (type) {
    case TaskType::START:
    case TaskType::RESET:
    case TaskType::EXIT_APP:
        return "L0 System";
    case TaskType::CAMERA:
    case TaskType::MICROPHONE:
    case TaskType::GENERATE_PARTICLE:
    case TaskType::BREEZE:
    case TaskType::CHANGE_DANDELION:
        return "L2 Interactive";
    case TaskType::BATCH_PARTICLE_EXECUTION:
    case TaskType::SINGLE_PARTICLE:
    case TaskType::FADE_PARTICLE:
        return "L3 Throughput";
    case TaskType::NONE:
        return "Idle";
    case TaskType::PLACEHOLDER:
    default:
        break;
    }

    switch (priority) {
    case PriorityLevel::P1_SYSTEM:
        return "L0 System";
    case PriorityLevel::P2_REALTIME:
        return "L1 Persistent Listen";
    case PriorityLevel::P2_FUNCTIONAL:
        return "L2 Interactive";
    case PriorityLevel::P3_PARTICLE:
        return "L3 Throughput";
    default:
        return "L?";
    }
}

const char* task_tree_node_label_impl(TaskType type) {
    switch (type) {
    case TaskType::START:
        return "System.StartTask";
    case TaskType::RESET:
        return "System.ResetTask";
    case TaskType::CAMERA_LISTENER_SERVICE:
        return "HumanBehavior.CameraListenerServiceTask";
    case TaskType::MICROPHONE_LISTENER_SERVICE:
        return "HumanBehavior.MicrophoneListenerServiceTask";
    case TaskType::CAMERA_LISTENER_BURST:
        return "HumanBehavior.CameraListenerBurstTask";
    case TaskType::MICROPHONE_LISTENER_BURST:
        return "HumanBehavior.MicrophoneListenerBurstTask";
    case TaskType::CAMERA:
        return "HumanBehavior.CameraDetectTask";
    case TaskType::MICROPHONE:
        return "HumanBehavior.BlowDetectTask";
    case TaskType::GENERATE_PARTICLE:
        return "ParticleRoot.ParticleGenerateTask";
    case TaskType::BREEZE:
        return "HumanBehavior.BlowFallbackTask";
    case TaskType::CHANGE_DANDELION:
        return "ParticleRoot.ChangeDandelionTask";
    case TaskType::BATCH_PARTICLE_EXECUTION:
        return "ParticleRoot.ParticleBatchTask";
    case TaskType::SINGLE_PARTICLE:
        return "ParticleRoot.ParticleMoveTask";
    case TaskType::FADE_PARTICLE:
        return "ParticleRoot.ParticleFadeTask";
    case TaskType::EXIT_APP:
        return "System.ExitTask";
    case TaskType::PLACEHOLDER:
        return "System.PlaceholderTask";
    case TaskType::NONE:
    default:
        return "Idle.None";
    }
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
        thread.bound_task_type = TaskType::NONE;
        thread.bound_task_priority = PriorityLevel::P2_FUNCTIONAL;
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
                thread.bound_task_type = TaskType::NONE;
                thread.bound_task_priority = PriorityLevel::P2_FUNCTIONAL;
                thread.last_task_event = "mode-wake";
            }
            continue;
        }

        const bool retiring_active_lane =
            thread.state == ThreadState::WAITING ||
            thread.state == ThreadState::RUNNING ||
            thread.state == ThreadState::IDLE;
        thread.state = ThreadState::SLEEPING;
        thread.bound_task_id = -1;
        thread.bound_task_name = "none";
        thread.bound_task_type = TaskType::NONE;
        thread.bound_task_priority = PriorityLevel::P2_FUNCTIONAL;
        thread.last_task_event = retiring_active_lane ? "mode-retire" : "mode-sleep";
    }
}

std::string task_debug_label(const Task& task) {
    return task.name + "#" + std::to_string(task.id) +
        "[" + std::string(scheduler_queue_label_impl(task.priority)) +
        "/" + latency_class_label_impl(task.type, task.priority) +
        "/" + task_tree_node_label_impl(task.type) + "]";
}

std::deque<StoredTask>& queue_for_priority(PriorityLevel priority) {
    if (priority == PriorityLevel::P1_SYSTEM) {
        return g_p1_queue;
    }
    if (priority == PriorityLevel::P2_REALTIME) {
        return g_p2_realtime_queue;
    }
    if (priority == PriorityLevel::P2_FUNCTIONAL) {
        return g_p2_queue;
    }
    return g_p3_queue;
}

Task* first_queued_task_by_type(std::deque<StoredTask>& queue, TaskType type) {
    for (StoredTask& entry : queue) {
        if (entry.task && entry.task->type == type) {
            return entry.task.get();
        }
    }
    return nullptr;
}

ListenerServiceDiagnostics build_listener_service_diagnostics(
    const CameraListenerState& listener) {
    ListenerServiceDiagnostics diagnostics;
    diagnostics.service_task_alive = listener.service_task_alive;
    diagnostics.service_task_id = listener.service_task_id;
    diagnostics.consumer_task_queued = listener.consumer_task_queued;
    diagnostics.consumer_task_id = listener.consumer_task_id;
    diagnostics.short_lease_active = listener.short_lease_active;
    diagnostics.short_lease_started_at_ms = listener.short_lease_started_at_ms;
    diagnostics.short_lease_until_ms = listener.short_lease_until_ms;
    diagnostics.short_detect_ready_at_ms = listener.short_detect_ready_at_ms;
    diagnostics.last_heartbeat_ms = listener.last_heartbeat_ms;
    diagnostics.last_seen_sample_ms = listener.last_seen_sample_ms;
    diagnostics.last_seeded_sample_ms = listener.last_seeded_sample_ms;
    diagnostics.last_consumed_sample_ms = listener.last_consumed_sample_ms;
    return diagnostics;
}

ListenerServiceDiagnostics build_listener_service_diagnostics(
    const MicrophoneListenerState& listener) {
    ListenerServiceDiagnostics diagnostics;
    diagnostics.service_task_alive = listener.service_task_alive;
    diagnostics.service_task_id = listener.service_task_id;
    diagnostics.consumer_task_queued = listener.consumer_task_queued;
    diagnostics.consumer_task_id = listener.consumer_task_id;
    diagnostics.short_lease_active = listener.short_lease_active;
    diagnostics.short_lease_started_at_ms = listener.short_lease_started_at_ms;
    diagnostics.short_lease_until_ms = listener.short_lease_until_ms;
    diagnostics.short_detect_ready_at_ms = listener.short_detect_ready_at_ms;
    diagnostics.last_heartbeat_ms = listener.last_heartbeat_ms;
    diagnostics.last_seen_sample_ms = listener.last_seen_sample_ms;
    diagnostics.last_seeded_sample_ms = listener.last_seeded_sample_ms;
    diagnostics.last_consumed_sample_ms = listener.last_consumed_sample_ms;
    return diagnostics;
}

void reconcile_listener_queue_bookkeeping() {
    RuntimeState& state_ref = runtime_state();

    if (Task* service_task =
            first_queued_task_by_type(
                g_p2_realtime_queue,
                TaskType::CAMERA_LISTENER_SERVICE)) {
        state_ref.camera_listener.service_task_alive = true;
        state_ref.camera_listener.service_task_id = service_task->id;
    } else {
        state_ref.camera_listener.service_task_alive = false;
        state_ref.camera_listener.service_task_id = -1;
    }

    if (!state_ref.camera_listener.enabled) {
        state_ref.camera_listener.consumer_task_queued = false;
        state_ref.camera_listener.consumer_task_id = -1;
    } else {
        if (Task* consumer_task =
                first_queued_task_by_type(g_p2_queue, TaskType::CAMERA)) {
            state_ref.camera_listener.consumer_task_queued = true;
            state_ref.camera_listener.consumer_task_id = consumer_task->id;
        } else {
            state_ref.camera_listener.consumer_task_queued = false;
            state_ref.camera_listener.consumer_task_id = -1;
        }
    }

    if (Task* service_task =
            first_queued_task_by_type(
                g_p2_realtime_queue,
                TaskType::MICROPHONE_LISTENER_SERVICE)) {
        state_ref.microphone_listener.service_task_alive = true;
        state_ref.microphone_listener.service_task_id = service_task->id;
    } else {
        state_ref.microphone_listener.service_task_alive = false;
        state_ref.microphone_listener.service_task_id = -1;
    }

    if (!state_ref.microphone_listener.enabled) {
        state_ref.microphone_listener.consumer_task_queued = false;
        state_ref.microphone_listener.consumer_task_id = -1;
    } else {
        if (Task* consumer_task =
                first_queued_task_by_type(g_p2_queue, TaskType::MICROPHONE)) {
            state_ref.microphone_listener.consumer_task_queued = true;
            state_ref.microphone_listener.consumer_task_id = consumer_task->id;
        } else {
            state_ref.microphone_listener.consumer_task_queued = false;
            state_ref.microphone_listener.consumer_task_id = -1;
        }
    }
}

void mark_listener_consumer_seeded(TaskType type) {
    RuntimeState& state_ref = runtime_state();
    if (type == TaskType::CAMERA) {
        if (state_ref.camera_bridge.timestamp_ms > 0) {
            state_ref.camera_listener.last_seeded_sample_ms =
                state_ref.camera_bridge.timestamp_ms;
        }
        return;
    }
    if (type == TaskType::MICROPHONE &&
        state_ref.microphone_bridge.timestamp_ms > 0) {
        state_ref.microphone_listener.last_seeded_sample_ms =
            state_ref.microphone_bridge.timestamp_ms;
    }
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
    if (Task* task = find_in_queue(g_p2_realtime_queue)) {
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
int realtime_order_for_task_type(TaskType type);

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
        reconcile_listener_queue_bookkeeping();
        return;
    }

    if (task->priority == PriorityLevel::P2_REALTIME) {
        task->realtime_order = realtime_order_for_task_type(task->type);
        auto existing = std::find_if(
            queue.begin(),
            queue.end(),
            [&task](const StoredTask& entry) {
                return entry.task && entry.task->type == task->type;
            });
        if (existing != queue.end()) {
            queue.erase(existing);
        } else if (queue.size() >= 2) {
            queue.pop_front();
        }
        StoredTask stored{std::move(task)};
        auto insert_at = std::find_if(
            queue.begin(),
            queue.end(),
            [&stored](const StoredTask& entry) {
                return entry.task &&
                    entry.task->realtime_order < stored.task->realtime_order;
            });
        queue.insert(insert_at, std::move(stored));
        reconcile_listener_queue_bookkeeping();
        return;
    }

    if (task->priority == PriorityLevel::P2_FUNCTIONAL) {
        if (!queue.empty() && queue.front().task->type == task->type) {
            queue.pop_front();
        } else if (queue.size() >= 5) {
            queue.pop_back();
        }
        queue.push_back({std::move(task)});
        reconcile_listener_queue_bookkeeping();
        return;
    }

    if (queue.size() >= 100) {
        queue.pop_front();
    }
    queue.push_back({std::move(task)});
    if (priority == PriorityLevel::P3_PARTICLE) {
        reconcile_runtime_particle_bookkeeping();
    }
    reconcile_listener_queue_bookkeeping();
}

void erase_p2_tasks_by_type(TaskType type) {
    auto it = std::remove_if(
        g_p2_queue.begin(),
        g_p2_queue.end(),
        [type](const StoredTask& entry) {
            return entry.task->type == type;
        });
    g_p2_queue.erase(it, g_p2_queue.end());
    reconcile_listener_queue_bookkeeping();
}

void reconcile_runtime_particle_bookkeeping();
bool seed_input_entry_task_if_idle_impl(const char* reason);
bool requeue_interrupted_task(StoredTask& entry, bool resume_task, const std::string& reason);

bool is_camera_task_family(TaskType type) {
    return type == TaskType::CAMERA ||
        type == TaskType::CAMERA_LISTENER_SERVICE ||
        type == TaskType::CAMERA_LISTENER_BURST;
}

bool is_microphone_task_family(TaskType type) {
    return type == TaskType::MICROPHONE ||
        type == TaskType::MICROPHONE_LISTENER_SERVICE ||
        type == TaskType::MICROPHONE_LISTENER_BURST;
}

bool is_persistent_realtime_listener_task(TaskType type) {
    return type == TaskType::CAMERA_LISTENER_SERVICE ||
        type == TaskType::MICROPHONE_LISTENER_SERVICE;
}

bool is_burst_realtime_listener_task(TaskType type) {
    return type == TaskType::CAMERA_LISTENER_BURST ||
        type == TaskType::MICROPHONE_LISTENER_BURST;
}

bool camera_should_use_long_listener_mode() {
    return runtime_state().camera_listener.enabled && g_thread_mode >= 2;
}

bool microphone_should_use_long_listener_mode() {
    if (!runtime_state().microphone_listener.enabled || g_thread_mode < 2) {
        return false;
    }
    if (g_thread_mode >= 3) {
        return true;
    }
    return !runtime_state().camera_listener.enabled;
}

bool camera_should_use_short_listener_mode() {
    if (!runtime_state().camera_listener.enabled) {
        return false;
    }
    if (g_thread_mode == 1) {
        return true;
    }
    return g_thread_mode == 2 && !camera_should_use_long_listener_mode();
}

bool microphone_should_use_short_listener_mode() {
    if (!runtime_state().microphone_listener.enabled) {
        return false;
    }
    if (g_thread_mode == 1) {
        return true;
    }
    return g_thread_mode == 2 && !microphone_should_use_long_listener_mode();
}

int realtime_order_for_task_type(TaskType type) {
    switch (type) {
    case TaskType::CAMERA_LISTENER_SERVICE:
        return 400;
    case TaskType::MICROPHONE_LISTENER_SERVICE:
        return 300;
    case TaskType::CAMERA_LISTENER_BURST:
        return 200;
    case TaskType::MICROPHONE_LISTENER_BURST:
        return 100;
    default:
        return 0;
    }
}

bool short_listener_lease_is_active(const CameraListenerState& listener) {
    return listener.short_lease_active &&
        listener.short_lease_until_ms > current_time_ms();
}

bool short_listener_lease_is_active(const MicrophoneListenerState& listener) {
    return listener.short_lease_active &&
        listener.short_lease_until_ms > current_time_ms();
}

void begin_short_listener_lease(CameraListenerState& listener) {
    const long long now_ms = current_time_ms();
    listener.short_lease_active = true;
    listener.short_lease_started_at_ms = now_ms;
    listener.short_lease_until_ms = now_ms + scheduler_task_support::kShortListenerLeaseMs;
    listener.short_warmup_until_ms = now_ms + scheduler_task_support::kShortListenerWarmupMs;
    listener.short_detect_ready_at_ms = now_ms + scheduler_task_support::kShortListenerDetectReadyMs;
}

void begin_short_listener_lease(MicrophoneListenerState& listener) {
    const long long now_ms = current_time_ms();
    listener.short_lease_active = true;
    listener.short_lease_started_at_ms = now_ms;
    listener.short_lease_until_ms = now_ms + scheduler_task_support::kShortListenerLeaseMs;
    listener.short_warmup_until_ms = now_ms + scheduler_task_support::kShortListenerWarmupMs;
    listener.short_detect_ready_at_ms = now_ms + scheduler_task_support::kShortListenerDetectReadyMs;
}

void clear_short_listener_lease(CameraListenerState& listener) {
    listener.short_lease_active = false;
    listener.short_lease_started_at_ms = 0;
    listener.short_lease_until_ms = 0;
    listener.short_warmup_until_ms = 0;
    listener.short_detect_ready_at_ms = 0;
}

void clear_short_listener_lease(MicrophoneListenerState& listener) {
    listener.short_lease_active = false;
    listener.short_lease_started_at_ms = 0;
    listener.short_lease_until_ms = 0;
    listener.short_warmup_until_ms = 0;
    listener.short_detect_ready_at_ms = 0;
}

void record_camera_reseed() {
    RuntimeCounters& counters = runtime_state().counters;
    counters.camera_reseed_count++;
}

void record_microphone_reseed() {
    RuntimeCounters& counters = runtime_state().counters;
    counters.microphone_reseed_count++;
}

void record_realtime_slice() {
    RuntimeCounters& counters = runtime_state().counters;
    counters.realtime_slices_this_frame++;
}

void finalize_frame_counters() {
    RuntimeCounters& counters = runtime_state().counters;
    counters.realtime_slices_last_frame = counters.realtime_slices_this_frame;
    counters.max_realtime_slices_per_frame = std::max(
        counters.max_realtime_slices_per_frame,
        counters.realtime_slices_last_frame);
}

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

bool pop_next_eligible_task_of_type(std::deque<StoredTask>& queue,
                                    TaskType type,
                                    StoredTask& out) {
    for (std::size_t index = 0; index < queue.size(); ++index) {
        if (!task_eligible_this_frame(queue[index]) || !queue[index].task) {
            continue;
        }
        if (queue[index].task->type != type) {
            continue;
        }
        out = std::move(queue[index]);
        queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }
    return false;
}

int erase_realtime_tasks_if(const std::function<bool(const StoredTask&)>& predicate,
                            const std::string& note_reason) {
    int removed = 0;
    auto it = std::remove_if(
        g_p2_realtime_queue.begin(),
        g_p2_realtime_queue.end(),
        [&predicate, &removed](const StoredTask& entry) {
            if (!predicate(entry)) {
                return false;
            }
            ++removed;
            return true;
        });
    g_p2_realtime_queue.erase(it, g_p2_realtime_queue.end());
    if (removed > 0) {
        push_runtime_note(
            "Scheduler: hard-interrupted " + std::to_string(removed) +
            " realtime listener task(s) during " + note_reason + ".");
        reconcile_listener_queue_bookkeeping();
    }
    return removed;
}

bool queue_contains_task_type(const std::deque<StoredTask>& queue, TaskType type) {
    return std::any_of(
        queue.begin(),
        queue.end(),
        [type](const StoredTask& entry) {
            return entry.task && entry.task->type == type;
        });
}

bool p2_queue_contains_generate_stage() {
    return std::any_of(
        g_p2_queue.begin(),
        g_p2_queue.end(),
        [](const StoredTask& entry) {
            if (!entry.task) {
                return false;
            }
            return entry.task->type == TaskType::GENERATE_PARTICLE ||
                entry.task->type == TaskType::BREEZE ||
                entry.task->type == TaskType::CHANGE_DANDELION;
        });
}

bool single_thread_has_downstream_phase_work() {
    if (g_thread_mode != 1) {
        return false;
    }

    return p2_queue_contains_generate_stage() ||
        queue_contains_task_type(g_p2_queue, TaskType::BATCH_PARTICLE_EXECUTION) ||
        !g_p3_queue.empty();
}

bool single_thread_allows_listener_entry_restore() {
    if (g_thread_mode != 1 || current_runtime_state().phase != RuntimePhase::READY) {
        return false;
    }

    return !single_thread_has_downstream_phase_work();
}

void align_single_thread_phase_to_pending_work() {
    if (g_thread_mode != 1) {
        return;
    }

    if (p2_queue_contains_generate_stage()) {
        g_single_thread_p2_phase = 2;
        return;
    }

    if (queue_contains_task_type(g_p2_queue, TaskType::BATCH_PARTICLE_EXECUTION) ||
        !g_p3_queue.empty()) {
        g_single_thread_p2_phase = 3;
        return;
    }

    if (queue_contains_task_type(g_p2_queue, TaskType::MICROPHONE)) {
        g_single_thread_p2_phase = 1;
        return;
    }

    if (queue_contains_task_type(g_p2_queue, TaskType::CAMERA)) {
        g_single_thread_p2_phase = 0;
    }
}

const char* single_thread_phase_label(int phase) {
    switch (phase) {
    case 0:
        return "Camera";
    case 1:
        return "Microphone";
    case 2:
        return "Generate/Breeze";
    case 3:
    default:
        return "Batch -> P3 RR";
    }
}

SingleThreadChainDiagnostics build_single_thread_chain_diagnostics(
    const RuntimeState& state,
    const P3FairnessMetrics& p3_metrics) {
    SingleThreadChainDiagnostics diagnostics;
    if (g_thread_mode != 1) {
        diagnostics.status = "multi-thread mode";
        return diagnostics;
    }

    diagnostics.active = true;

    const bool system_override = !g_p1_queue.empty();
    const bool has_l1_service = !g_p2_realtime_queue.empty();
    const bool has_camera = queue_contains_task_type(g_p2_queue, TaskType::CAMERA);
    const bool has_microphone = queue_contains_task_type(g_p2_queue, TaskType::MICROPHONE);
    const bool has_generate_stage = p2_queue_contains_generate_stage();
    const bool has_batch = queue_contains_task_type(g_p2_queue, TaskType::BATCH_PARTICLE_EXECUTION);
    const bool has_p3_work = p3_metrics.total() > 0;

    if (system_override) {
        diagnostics.current = "L0 override";
        diagnostics.next = single_thread_phase_label(g_single_thread_p2_phase);
        diagnostics.status = "P1 system task ahead of single-thread chain";
        return diagnostics;
    }

    if (has_l1_service) {
        diagnostics.current = "L1 burst/service";
        diagnostics.next = single_thread_phase_label(g_single_thread_p2_phase);
        diagnostics.status = "P2 realtime slice ahead of interactive chain";
        return diagnostics;
    }

    if (state.camera_focus_locked || has_camera) {
        diagnostics.current = "Camera";
        diagnostics.next = "Microphone";
    } else if (state.microphone_focus_locked || has_microphone) {
        diagnostics.current = "Microphone";
        diagnostics.next = "Generate/Breeze";
    } else if (has_generate_stage) {
        diagnostics.current = "Generate/Breeze";
        diagnostics.next = "Batch";
    } else if (has_batch && has_p3_work) {
        diagnostics.current = "Batch -> P3 RR";
        diagnostics.next = "drain -> Camera";
    } else if (has_batch) {
        diagnostics.current = "Batch";
        diagnostics.next = "drain -> Camera";
    } else if (has_p3_work) {
        diagnostics.current = "P3 RR";
        diagnostics.next = "drain -> Camera";
    } else if (state.camera_listener.enabled) {
        diagnostics.current = "drain -> Camera";
        diagnostics.next = "Camera";
    } else if (state.microphone_listener.enabled) {
        diagnostics.current = "drain -> Microphone";
        diagnostics.next = "Microphone";
    } else {
        diagnostics.current = "idle";
        diagnostics.next = "idle";
    }

    diagnostics.status =
        std::string("phase ") + single_thread_phase_label(g_single_thread_p2_phase) +
        " / focus " + build_input_focus_status(state);

    if (has_p3_work) {
        diagnostics.status +=
            " / P3 move " + std::to_string(p3_metrics.move_queued + p3_metrics.move_running) +
            " fade " + std::to_string(p3_metrics.fade_queued + p3_metrics.fade_running);
    } else {
        diagnostics.status += " / P3 drained";
    }

    return diagnostics;
}

L1RealtimeDiagnostics build_l1_realtime_diagnostics(const RuntimeState& state) {
    L1RealtimeDiagnostics diagnostics;
    diagnostics.active = true;

    const bool camera_bridge_fresh = camera_bridge_is_fresh(state.camera_bridge);
    const bool microphone_bridge_fresh = microphone_bridge_is_fresh(state.microphone_bridge);
    const bool gate_fresh = camera_gate_is_fresh(state);
    const bool microphone_focus_fresh =
        state.microphone_focus_locked &&
        state.microphone_focus_until_ms > 0 &&
        current_time_ms() <= state.microphone_focus_until_ms;

    const CameraListenerState& camera_listener = state.camera_listener;
    diagnostics.camera_lane =
        std::string(camera_listener.bridge_running ? "CAM bridge-on" : "CAM bridge-off") +
        " / " + (camera_listener.unavailable ? "device-down"
                                             : (camera_listener.device_available ? "device-up"
                                                                                 : "device-wait")) +
        " / " + (camera_listener.sample_ready
                     ? (camera_listener.stale || !camera_bridge_fresh ? "sample-stale"
                                                                      : "sample-hot")
                     : "sample-empty") +
        " / " + (state.camera_available ? "mouth-ready"
                                         : (state.camera_device_available ? "scan-mouth"
                                                                          : "boot"));

    const MicrophoneListenerState& microphone_listener = state.microphone_listener;
    diagnostics.microphone_lane =
        std::string(microphone_listener.bridge_running ? "MIC bridge-on" : "MIC bridge-off") +
        " / " + (microphone_listener.unavailable ? "device-down"
                                                  : (microphone_listener.device_available ? "device-up"
                                                                                         : "device-wait")) +
        " / " + (microphone_listener.sample_ready
                     ? (microphone_listener.stale || !microphone_bridge_fresh ? "sample-stale"
                                                                              : "sample-hot")
                     : "sample-empty") +
        " / " + (state.microphone_bridge.voice_detected ? "voice-live"
                                                         : (state.microphone_bridge.fallback_requested
                                                                ? "fallback-live"
                                                                : "listen"));

    diagnostics.gate_lane =
        std::string(gate_fresh ? "GATE hot" : (state.camera_gate_open ? "GATE open-stale" : "GATE closed")) +
        " / " + (state.camera_focus_locked ? "camera-focus"
                                            : (state.microphone_focus_locked ? "microphone-focus"
                                                                             : "free-focus")) +
        " / " + (microphone_focus_fresh ? "mic-window-hot"
                                         : (state.microphone_focus_locked ? "mic-window-stale"
                                                                          : "mic-window-idle"));

    if (!camera_listener.enabled && !microphone_listener.enabled) {
        diagnostics.blocked_reason = "listeners-disabled";
    } else if (camera_listener.enabled && camera_listener.unavailable) {
        diagnostics.blocked_reason = "camera-device-unavailable";
    } else if (microphone_listener.enabled && microphone_listener.unavailable) {
        diagnostics.blocked_reason = "microphone-device-unavailable";
    } else if (camera_listener.enabled && !camera_listener.device_available) {
        diagnostics.blocked_reason = "camera-device-wait";
    } else if (camera_listener.enabled && !state.camera_bridge.bridge_connected) {
        diagnostics.blocked_reason = "camera-bridge-disconnected";
    } else if (camera_listener.enabled && !camera_listener.sample_ready) {
        diagnostics.blocked_reason = "camera-sample-wait";
    } else if (camera_listener.enabled && (camera_listener.stale || !camera_bridge_fresh)) {
        diagnostics.blocked_reason = "camera-sample-stale";
    } else if (camera_listener.enabled && !state.camera_available) {
        diagnostics.blocked_reason = "camera-mouth-wait";
    } else if (microphone_listener.enabled && !microphone_listener.device_available) {
        diagnostics.blocked_reason = "microphone-device-wait";
    } else if (microphone_listener.enabled && !state.microphone_bridge.bridge_connected) {
        diagnostics.blocked_reason = "microphone-bridge-disconnected";
    } else if (microphone_listener.enabled && !microphone_listener.sample_ready) {
        diagnostics.blocked_reason = "microphone-sample-wait";
    } else if (microphone_listener.enabled &&
               (microphone_listener.stale || !microphone_bridge_fresh)) {
        diagnostics.blocked_reason = "microphone-sample-stale";
    } else if (microphone_listener.enabled &&
               !state.microphone_bridge.voice_detected &&
               !state.microphone_bridge.fallback_requested) {
        diagnostics.blocked_reason = "voice-wait";
    } else if (state.camera_device_available && !gate_fresh && !microphone_focus_fresh) {
        diagnostics.blocked_reason = "camera-gate-wait";
    } else {
        diagnostics.blocked_reason = "realtime-ready";
    }

    return diagnostics;
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
    auto move_type_to_front_or_create = [](TaskType desired_type) {
        auto move_existing_to_front = [desired_type]() -> bool {
            for (std::size_t index = 0; index < g_p2_queue.size(); ++index) {
                if (!g_p2_queue[index].task || g_p2_queue[index].task->type != desired_type) {
                    continue;
                }
                if (index == 0) {
                    return true;
                }
                StoredTask entry = std::move(g_p2_queue[index]);
                g_p2_queue.erase(g_p2_queue.begin() + static_cast<std::ptrdiff_t>(index));
                g_p2_queue.push_front(std::move(entry));
                return true;
            }
            return false;
        };

        if (move_existing_to_front()) {
            return;
        }

        if (desired_type == TaskType::CAMERA && runtime_state().camera_listener.enabled) {
            submit_task(make_camera_task());
            move_existing_to_front();
            return;
        }
        if (desired_type == TaskType::MICROPHONE && runtime_state().microphone_listener.enabled) {
            submit_task(make_microphone_task());
            move_existing_to_front();
            return;
        }
    };

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
    const bool allow_locked_sensor_phase = !single_thread_has_downstream_phase_work();
    if (allow_locked_sensor_phase && state_ref.camera_focus_locked) {
        move_type_to_front_or_create(TaskType::CAMERA);
    }
    if (allow_locked_sensor_phase && state_ref.microphone_focus_locked) {
        move_type_to_front_or_create(TaskType::MICROPHONE);
    }

    if (allow_locked_sensor_phase &&
        state_ref.camera_focus_locked &&
        pop_locked_type(TaskType::CAMERA)) {
        return true;
    }
    if (allow_locked_sensor_phase &&
        state_ref.microphone_focus_locked &&
        pop_locked_type(TaskType::MICROPHONE)) {
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

bool task_matches_thread(const Task& task, const RuntimeThread& thread) {
    if (!thread.is_pinned) {
        return true;
    }
    if (thread.pinned_task_type == TaskType::NONE) {
        if (is_camera_task_family(task.type)) {
            return false;
        }
        if (is_microphone_task_family(task.type) && g_thread_mode >= 3) {
            return false;
        }
        return true;
    }
    if (thread.pinned_task_type == TaskType::CAMERA) {
        return is_camera_task_family(task.type);
    }
    if (thread.pinned_task_type == TaskType::MICROPHONE) {
        return is_microphone_task_family(task.type);
    }
    return task.type == thread.pinned_task_type;
}

void assign_thread_roles(int active_threads) {
    for (auto& t : g_threads) {
        t.is_pinned = false;
        t.pinned_task_type = TaskType::NONE;
    }

    if (active_threads <= 1) {
        return;
    }

    for (int index = 0; index < active_threads && index < static_cast<int>(g_threads.size()); ++index) {
        g_threads[static_cast<std::size_t>(index)].is_pinned = true;
        g_threads[static_cast<std::size_t>(index)].pinned_task_type = TaskType::NONE;
    }

    if (active_threads == 2) {
        if (runtime_state().camera_listener.enabled) {
            g_threads[0].pinned_task_type = TaskType::CAMERA;
        } else if (runtime_state().microphone_listener.enabled) {
            g_threads[0].pinned_task_type = TaskType::MICROPHONE;
        }
        return;
    }

    if (active_threads >= 3) {
        if (runtime_state().camera_listener.enabled) {
            g_threads[0].pinned_task_type = TaskType::CAMERA;
        }
        if (runtime_state().microphone_listener.enabled) {
            g_threads[1].pinned_task_type = TaskType::MICROPHONE;
        }
    }
}

bool take_next_task_for_dispatch(int dispatch_slot, int active_threads, const RuntimeThread& thread, StoredTask& out) {
    (void)dispatch_slot;
    assign_thread_roles(active_threads);

    if (active_threads > 1) {
        // `L0/P1` is a runtime-wide override, not a general-lane-only workload.
        // Any active worker may claim it before lane-specific `P2` selection.
        if (pop_next_eligible_task(g_p1_queue, true, out)) {
            return true;
        }

        for (std::size_t index = 0; index < g_p2_queue.size(); ++index) {
            const StoredTask& entry = g_p2_queue[index];
            if (!task_eligible_this_frame(entry) || !entry.task) {
                continue;
            }
            if (!task_matches_thread(*entry.task, thread)) {
                continue;
            }
            out = std::move(g_p2_queue[index]);
            g_p2_queue.erase(g_p2_queue.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
        return false;
    }

    if (pop_next_eligible_task(g_p1_queue, true, out)) {
        return true;
    }

    return pop_single_thread_p2_task(out);
}

bool has_pending_tasks() {
    return !g_p1_queue.empty() ||
        !g_p2_realtime_queue.empty() ||
        !g_p2_queue.empty() ||
        !g_p3_queue.empty();
}

int priority_rank(PriorityLevel priority) {
    switch (priority) {
    case PriorityLevel::P1_SYSTEM:
        return 4;
    case PriorityLevel::P2_REALTIME:
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

bool queues_all_empty() {
    return g_p1_queue.empty() &&
        g_p2_realtime_queue.empty() &&
        g_p2_queue.empty() &&
        g_p3_queue.empty();
}

void queue_microphone_stage_if_needed() {
    if (!runtime_state().microphone_listener.enabled) {
        return;
    }
    if (!has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL)) {
        mark_listener_consumer_seeded(TaskType::MICROPHONE);
        submit_task(make_microphone_task());
    }
}

void queue_camera_stage_if_needed() {
    if (!runtime_state().camera_listener.enabled) {
        return;
    }
    if (!has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL)) {
        mark_listener_consumer_seeded(TaskType::CAMERA);
        submit_task(make_camera_task());
    }
}

void queue_batch_stage_if_needed() {
    if (!has_queued_task(TaskType::BATCH_PARTICLE_EXECUTION, PriorityLevel::P2_FUNCTIONAL)) {
        submit_task(make_batch_particle_execution_task());
    }
}

void submit_camera_listener_burst_task_with_lease() {
    RuntimeState& state_ref = runtime_state();
    if (!short_listener_lease_is_active(state_ref.camera_listener)) {
        begin_short_listener_lease(state_ref.camera_listener);
    }
    record_camera_reseed();
    submit_task(make_camera_listener_burst_task());
}

void submit_microphone_listener_burst_task_with_lease() {
    RuntimeState& state_ref = runtime_state();
    if (!short_listener_lease_is_active(state_ref.microphone_listener)) {
        begin_short_listener_lease(state_ref.microphone_listener);
    }
    record_microphone_reseed();
    submit_task(make_microphone_listener_burst_task());
}

bool seed_mode_specific_short_listener_if_needed_impl(const char* reason) {
    if (current_runtime_state().phase != RuntimePhase::READY || g_thread_mode <= 1) {
        return false;
    }

    bool seeded = false;
    RuntimeState& state_ref = runtime_state();
    if (camera_should_use_short_listener_mode() &&
        !short_listener_lease_is_active(state_ref.camera_listener) &&
        !has_queued_task(
            TaskType::CAMERA_LISTENER_BURST,
            PriorityLevel::P2_REALTIME)) {
        submit_camera_listener_burst_task_with_lease();
        seeded = true;
    }

    if (microphone_should_use_short_listener_mode() &&
        !short_listener_lease_is_active(state_ref.microphone_listener) &&
        !has_queued_task(
            TaskType::MICROPHONE_LISTENER_BURST,
            PriorityLevel::P2_REALTIME)) {
        submit_microphone_listener_burst_task_with_lease();
        seeded = true;
    }

    if (seeded) {
        push_runtime_note(
            std::string("Scheduler: seeded mode-specific short listener lease") +
            (reason ? std::string(" (") + reason + ")" : std::string()));
    }
    return seeded;
}

void normalize_realtime_listener_tasks_for_current_mode(const std::string& reason) {
    erase_realtime_tasks_if(
        [](const StoredTask& entry) {
            if (!entry.task) {
                return false;
            }
            switch (entry.task->type) {
            case TaskType::CAMERA_LISTENER_SERVICE:
                return !camera_should_use_long_listener_mode();
            case TaskType::MICROPHONE_LISTENER_SERVICE:
                return !microphone_should_use_long_listener_mode();
            case TaskType::CAMERA_LISTENER_BURST:
                return !camera_should_use_short_listener_mode();
            case TaskType::MICROPHONE_LISTENER_BURST:
                return !microphone_should_use_short_listener_mode();
            default:
                return false;
            }
        },
        reason);
}

void ensure_realtime_listener_service_tasks() {
    if (current_runtime_state().phase != RuntimePhase::READY || g_thread_mode <= 1) {
        return;
    }

    RuntimeState& state_ref = runtime_state();
    erase_realtime_tasks_if(
        [](const StoredTask& entry) {
            if (!entry.task || !is_persistent_realtime_listener_task(entry.task->type)) {
                return false;
            }
            if (entry.task->type == TaskType::CAMERA_LISTENER_SERVICE) {
                return !camera_should_use_long_listener_mode();
            }
            if (entry.task->type == TaskType::MICROPHONE_LISTENER_SERVICE) {
                return !microphone_should_use_long_listener_mode();
            }
            return false;
        },
        "mode-specific long-listener normalization");

    if (camera_should_use_long_listener_mode() &&
        !has_queued_task(
            TaskType::CAMERA_LISTENER_SERVICE,
            PriorityLevel::P2_REALTIME)) {
        record_camera_reseed();
        submit_task(make_camera_listener_service_task());
    }

    if (microphone_should_use_long_listener_mode() &&
        !has_queued_task(
            TaskType::MICROPHONE_LISTENER_SERVICE,
            PriorityLevel::P2_REALTIME)) {
        record_microphone_reseed();
        submit_task(make_microphone_listener_service_task());
    }

    if (camera_should_use_long_listener_mode()) {
        clear_short_listener_lease(state_ref.camera_listener);
    }
    if (microphone_should_use_long_listener_mode()) {
        clear_short_listener_lease(state_ref.microphone_listener);
    }
}

void ensure_single_thread_listener_burst_tasks() {
    if (current_runtime_state().phase != RuntimePhase::READY ||
        (g_thread_mode != 1 && g_thread_mode != 2)) {
        return;
    }

    RuntimeState& state_ref = runtime_state();
    if (state_ref.camera_listener.short_lease_active &&
        !short_listener_lease_is_active(state_ref.camera_listener)) {
        clear_short_listener_lease(state_ref.camera_listener);
    }
    if (state_ref.microphone_listener.short_lease_active &&
        !short_listener_lease_is_active(state_ref.microphone_listener)) {
        clear_short_listener_lease(state_ref.microphone_listener);
    }

    const bool allow_listener_entry_restore =
        g_thread_mode == 1 ? single_thread_allows_listener_entry_restore() : true;
    const bool want_camera_burst =
        g_thread_mode == 2
            ? (camera_should_use_short_listener_mode() &&
               short_listener_lease_is_active(state_ref.camera_listener))
            : (camera_should_use_short_listener_mode() &&
               (short_listener_lease_is_active(state_ref.camera_listener) ||
                (allow_listener_entry_restore &&
                 (state_ref.camera_focus_locked ||
                  (!state_ref.microphone_focus_locked && !state_ref.camera_gate_open)))));
    const bool want_microphone_burst =
        g_thread_mode == 2
            ? (microphone_should_use_short_listener_mode() &&
               short_listener_lease_is_active(state_ref.microphone_listener))
            : (microphone_should_use_short_listener_mode() &&
               (short_listener_lease_is_active(state_ref.microphone_listener) ||
                (allow_listener_entry_restore &&
                 (state_ref.microphone_focus_locked || state_ref.camera_gate_open))));

    erase_realtime_tasks_if(
        [](const StoredTask& entry) {
            if (!entry.task || !is_burst_realtime_listener_task(entry.task->type)) {
                return false;
            }
            if (entry.task->type == TaskType::CAMERA_LISTENER_BURST) {
                return !camera_should_use_short_listener_mode();
            }
            if (entry.task->type == TaskType::MICROPHONE_LISTENER_BURST) {
                return !microphone_should_use_short_listener_mode();
            }
            return false;
        },
        "mode-specific short-listener normalization");

    if (want_camera_burst &&
        !has_queued_task(
            TaskType::CAMERA_LISTENER_BURST,
            PriorityLevel::P2_REALTIME)) {
        submit_camera_listener_burst_task_with_lease();
    }

    if (want_microphone_burst &&
        !has_queued_task(
            TaskType::MICROPHONE_LISTENER_BURST,
            PriorityLevel::P2_REALTIME)) {
        submit_microphone_listener_burst_task_with_lease();
    }
}

void recover_camera_listener_from_watchdog() {
    erase_p2_tasks_by_type(TaskType::CAMERA);
    erase_realtime_tasks_if(
        [](const StoredTask& entry) {
            return entry.task && is_camera_task_family(entry.task->type);
        },
        "camera watchdog recovery");

    with_shared_state_write(false, false, true, []() {
        RuntimeState& state_ref = runtime_state();
        const long long now_ms = current_time_ms();
        state_ref.camera_listener.sample_ready = false;
        state_ref.camera_listener.stale = true;
        state_ref.camera_listener.service_task_alive = false;
        state_ref.camera_listener.service_task_id = -1;
        state_ref.camera_listener.consumer_task_queued = false;
        state_ref.camera_listener.consumer_task_id = -1;
        state_ref.camera_listener.last_heartbeat_ms = now_ms;
        state_ref.camera_listener.last_seen_sample_ms = 0;
        state_ref.camera_listener.last_seeded_sample_ms = 0;
        state_ref.camera_bridge.sample_ready = false;
        state_ref.camera_bridge.timestamp_ms = 0;
        state_ref.camera_bridge.status_text = "camera watchdog recovery pending refresh";
        state_ref.camera_available = false;
        state_ref.counters.watchdog_recovery_count++;
    });

    push_runtime_note(
        "Watchdog: camera listener heartbeat timed out; cleared stale state and reseeded listener.");

    if (camera_should_use_long_listener_mode()) {
        record_camera_reseed();
        submit_task(make_camera_listener_service_task());
    } else if (camera_should_use_short_listener_mode()) {
        submit_camera_listener_burst_task_with_lease();
    }
}

void recover_microphone_listener_from_watchdog() {
    erase_p2_tasks_by_type(TaskType::MICROPHONE);
    erase_realtime_tasks_if(
        [](const StoredTask& entry) {
            return entry.task && is_microphone_task_family(entry.task->type);
        },
        "microphone watchdog recovery");

    with_shared_state_write(false, false, true, []() {
        RuntimeState& state_ref = runtime_state();
        const long long now_ms = current_time_ms();
        state_ref.microphone_listener.sample_ready = false;
        state_ref.microphone_listener.stale = true;
        state_ref.microphone_listener.service_task_alive = false;
        state_ref.microphone_listener.service_task_id = -1;
        state_ref.microphone_listener.consumer_task_queued = false;
        state_ref.microphone_listener.consumer_task_id = -1;
        state_ref.microphone_listener.last_heartbeat_ms = now_ms;
        state_ref.microphone_listener.last_seen_sample_ms = 0;
        state_ref.microphone_listener.last_seeded_sample_ms = 0;
        state_ref.microphone_bridge.sample_ready = false;
        state_ref.microphone_bridge.timestamp_ms = 0;
        state_ref.microphone_bridge.status_text =
            "microphone watchdog recovery pending refresh";
        state_ref.microphone_available = false;
        state_ref.counters.watchdog_recovery_count++;
    });

    push_runtime_note(
        "Watchdog: microphone listener heartbeat timed out; cleared stale state and reseeded listener.");

    if (microphone_should_use_long_listener_mode()) {
        record_microphone_reseed();
        submit_task(make_microphone_listener_service_task());
    } else if (microphone_should_use_short_listener_mode()) {
        submit_microphone_listener_burst_task_with_lease();
    }
}

void run_listener_watchdog_recovery_if_needed() {
    const RuntimeState& state_ref = current_runtime_state();
    if (state_ref.phase != RuntimePhase::READY) {
        return;
    }

    const long long now_ms = current_time_ms();
    if (state_ref.camera_listener.enabled &&
        state_ref.camera_listener.last_heartbeat_ms > 0 &&
        now_ms - state_ref.camera_listener.last_heartbeat_ms >
            kCameraHeartbeatTimeoutMs) {
        recover_camera_listener_from_watchdog();
    }

    if (state_ref.microphone_listener.enabled &&
        state_ref.microphone_listener.last_heartbeat_ms > 0 &&
        now_ms - state_ref.microphone_listener.last_heartbeat_ms >
            kMicrophoneHeartbeatTimeoutMs) {
        recover_microphone_listener_from_watchdog();
    }
}

void run_realtime_service_slice(StoredTask& entry, const char* slice_reason) {
    if (!entry.task) {
        return;
    }

    Task& task = *entry.task;
    record_realtime_slice();
    const bool resume_task =
        task.state == TaskState::REQUEUED || task.state == TaskState::INTERRUPTED;
    mark_task_dispatched(task, g_frame_index, resume_task);
    task.state = TaskState::RUNNING;
    if (resume_task) {
        task.resume();
    } else {
        task.execute();
    }

    if (task.state == TaskState::INTERRUPTED ||
        (task.state == TaskState::RUNNING && task.support_resume)) {
        requeue_interrupted_task(entry, resume_task, slice_reason);
        push_runtime_note(
            std::string("Realtime slice yielded ") + task_debug_label(task));
        return;
    }

    mark_task_finished(task, g_frame_index, slice_reason);
}

void run_realtime_service_slices_if_needed() {
    if (!g_p1_queue.empty() || g_p2_realtime_queue.empty()) {
        return;
    }

    StoredTask entry;
    if (g_thread_mode > 1) {
        entry = StoredTask{};
        if (pop_next_eligible_task(g_p2_realtime_queue, false, entry)) {
            const char* slice_reason = "realtime-generic-slice";
            if (entry.task) {
                switch (entry.task->type) {
                case TaskType::CAMERA_LISTENER_SERVICE:
                    slice_reason = "realtime-camera-service-slice";
                    break;
                case TaskType::MICROPHONE_LISTENER_SERVICE:
                    slice_reason = "realtime-microphone-service-slice";
                    break;
                case TaskType::CAMERA_LISTENER_BURST:
                    slice_reason = "realtime-camera-burst-slice";
                    break;
                case TaskType::MICROPHONE_LISTENER_BURST:
                    slice_reason = "realtime-microphone-burst-slice";
                    break;
                default:
                    break;
                }
                g_last_multithread_realtime_slice_type = entry.task->type;
            }
            run_realtime_service_slice(entry, slice_reason);
            return;
        }
    } else {
        bool ran_single_thread_burst = false;
        const RuntimeState& state_ref = current_runtime_state();
        const bool prefer_camera_burst =
            state_ref.camera_focus_locked ||
            (!state_ref.microphone_focus_locked && state_ref.camera_listener.enabled);
        const bool prefer_microphone_burst =
            state_ref.microphone_focus_locked ||
            (!state_ref.camera_focus_locked && state_ref.microphone_listener.enabled);

        auto run_single_burst = [&entry](TaskType type, const char* slice_reason) -> bool {
            entry = StoredTask{};
            if (!pop_next_eligible_task_of_type(g_p2_realtime_queue, type, entry)) {
                return false;
            }
            run_realtime_service_slice(entry, slice_reason);
            return true;
        };

        if (prefer_camera_burst) {
            ran_single_thread_burst = run_single_burst(
                    TaskType::CAMERA_LISTENER_BURST,
                    "realtime-camera-burst-slice");
            if (!ran_single_thread_burst) {
                ran_single_thread_burst = run_single_burst(
                    TaskType::MICROPHONE_LISTENER_BURST,
                    "realtime-microphone-burst-slice");
            }
        } else if (prefer_microphone_burst) {
            ran_single_thread_burst = run_single_burst(
                    TaskType::MICROPHONE_LISTENER_BURST,
                    "realtime-microphone-burst-slice");
            if (!ran_single_thread_burst) {
                ran_single_thread_burst = run_single_burst(
                    TaskType::CAMERA_LISTENER_BURST,
                    "realtime-camera-burst-slice");
            }
        } else {
            ran_single_thread_burst = run_single_burst(
                    TaskType::CAMERA_LISTENER_BURST,
                    "realtime-camera-burst-slice");
            if (!ran_single_thread_burst) {
                ran_single_thread_burst = run_single_burst(
                    TaskType::MICROPHONE_LISTENER_BURST,
                    "realtime-microphone-burst-slice");
            }
        }

        if (ran_single_thread_burst) {
            return;
        }
    }

    if (!g_p2_realtime_queue.empty()) {
        entry = StoredTask{};
        if (pop_next_eligible_task(g_p2_realtime_queue, false, entry)) {
            run_realtime_service_slice(entry, "realtime-generic-slice");
        }
    }
}

void ensure_multithread_camera_entry_task() {
    if (g_thread_mode <= 1 || current_runtime_state().phase != RuntimePhase::READY) {
        return;
    }

    if (!camera_should_use_long_listener_mode()) {
        return;
    }
    if (has_queued_task(
            TaskType::CAMERA_LISTENER_SERVICE,
            PriorityLevel::P2_REALTIME)) {
        return;
    }
    if (has_queued_task(TaskType::CAMERA, PriorityLevel::P2_FUNCTIONAL)) {
        return;
    }

    const bool queues_were_active = has_pending_tasks();
    mark_listener_consumer_seeded(TaskType::CAMERA);
    submit_task(make_camera_task());
    if (queues_were_active &&
        (g_frame_index - g_last_multithread_camera_reseed_note_frame) >= 15) {
        push_runtime_note(
            "Scheduler: multi-thread camera reseed from live listener while queues remain active.");
        g_last_multithread_camera_reseed_note_frame = g_frame_index;
    }
}

void ensure_multithread_microphone_entry_task() {
    if (g_thread_mode <= 1 || current_runtime_state().phase != RuntimePhase::READY) {
        return;
    }

    RuntimeState& state_ref = runtime_state();
    if (!microphone_should_use_long_listener_mode()) {
        return;
    }
    if (has_queued_task(
            TaskType::MICROPHONE_LISTENER_SERVICE,
            PriorityLevel::P2_REALTIME)) {
        return;
    }
    if (has_queued_task(TaskType::MICROPHONE, PriorityLevel::P2_FUNCTIONAL)) {
        return;
    }

    const bool input_window_ready =
        !state_ref.camera_device_available ||
        camera_gate_is_fresh(state_ref) ||
        microphone_window_is_fresh(state_ref);
    const bool fresh_microphone_sample =
        state_ref.microphone_listener.sample_ready &&
        state_ref.microphone_bridge.bridge_connected &&
        microphone_bridge_is_fresh(state_ref.microphone_bridge);
    const bool unconsumed_sample =
        state_ref.microphone_bridge.timestamp_ms > 0 &&
        state_ref.microphone_bridge.timestamp_ms !=
            state_ref.last_consumed_microphone_sample_ms;

    if (!input_window_ready || !fresh_microphone_sample || !unconsumed_sample) {
        return;
    }

    const bool queues_were_active = has_pending_tasks();
    mark_listener_consumer_seeded(TaskType::MICROPHONE);
    submit_task(make_microphone_task());
    if (queues_were_active &&
        (g_frame_index - g_last_multithread_microphone_reseed_note_frame) >= 15) {
        push_runtime_note(
            "Scheduler: multi-thread microphone reseed from live listener while queues remain active.");
        g_last_multithread_microphone_reseed_note_frame = g_frame_index;
    }
}

void ensure_single_thread_focus_entry_task() {
    if (g_thread_mode != 1 || current_runtime_state().phase != RuntimePhase::READY) {
        return;
    }

    if (!single_thread_allows_listener_entry_restore()) {
        return;
    }

    RuntimeState& state_ref = runtime_state();
    if (state_ref.camera_focus_locked) {
        if (!has_queued_task(
                TaskType::CAMERA_LISTENER_BURST,
                PriorityLevel::P2_REALTIME)) {
            if (state_ref.camera_listener.enabled) {
                submit_camera_listener_burst_task_with_lease();
                push_runtime_note("Scheduler: camera focus restored CameraListenerBurstTask.");
            } else if (!state_ref.camera_listener.enabled) {
                push_runtime_note("Scheduler: camera focus requested CameraListenerBurstTask, but camera listener is disabled.");
            }
        }
    }

    if (state_ref.microphone_focus_locked) {
        if (!has_queued_task(
                TaskType::MICROPHONE_LISTENER_BURST,
                PriorityLevel::P2_REALTIME)) {
            if (state_ref.microphone_listener.enabled) {
                submit_microphone_listener_burst_task_with_lease();
                push_runtime_note("Scheduler: microphone focus restored MicrophoneListenerBurstTask.");
            } else if (!state_ref.microphone_listener.enabled) {
                push_runtime_note("Scheduler: microphone focus requested MicrophoneListenerBurstTask, but microphone listener is disabled.");
            }
        }
    }
}

bool seed_input_entry_task_if_idle_impl(const char* reason) {
    if (current_runtime_state().phase != RuntimePhase::READY || !queues_all_empty()) {
        return false;
    }

    RuntimeState& state_ref = runtime_state();
    state_ref.camera_focus_locked = state_ref.camera_listener.enabled;
    state_ref.microphone_focus_locked = false;
    state_ref.microphone_focus_consumed_for_gate = false;
    state_ref.camera_gate_open = false;
    state_ref.camera_gate_frame = -1;
    state_ref.camera_gate_until_ms = 0;
    state_ref.microphone_focus_until_ms = 0;
    g_single_thread_p2_phase = 0;

    if (g_thread_mode == 1) {
        if (state_ref.camera_listener.enabled) {
            submit_camera_listener_burst_task_with_lease();
            push_runtime_note(
                std::string("Scheduler: single-thread cycle reseeded camera burst entry") +
                (reason ? std::string(" (") + reason + ")" : std::string()));
            return true;
        } else if (state_ref.microphone_listener.enabled) {
            state_ref.camera_focus_locked = false;
            state_ref.microphone_focus_locked = true;
            state_ref.microphone_focus_until_ms = current_time_ms() +
                scheduler_task_support::kCameraGateHoldMs;
            g_single_thread_p2_phase = 1;
            submit_microphone_listener_burst_task_with_lease();
            push_runtime_note(
                std::string("Scheduler: single-thread cycle reseeded microphone burst entry") +
                (reason ? std::string(" (") + reason + ")" : std::string()));
            return true;
        }
    } else {
        if (camera_should_use_long_listener_mode()) {
            record_camera_reseed();
            submit_task(make_camera_listener_service_task());
        }
        if (microphone_should_use_long_listener_mode()) {
            record_microphone_reseed();
            submit_task(make_microphone_listener_service_task());
        }
        if (state_ref.camera_listener.enabled || state_ref.microphone_listener.enabled) {
            push_runtime_note(
                std::string("Scheduler: multi-thread cycle reseeded mode-specific realtime listeners") +
                (reason ? std::string(" (") + reason + ")" : std::string()));
            return true;
        }
    }

    push_runtime_note("Scheduler: single-thread cycle drained with no listener entry available.");
    return false;
}

void seed_input_entry_task_if_idle() {
    seed_input_entry_task_if_idle_impl("idle");
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
        particle.visual_alpha = 1.0f;
        particle.attached = true;
        particle.active = false;
        particle.ownership_token = 0;
        particle.fade_steps_remaining = 0;
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

bool particle_slot_counts_as_move_running_work(const ParticleRenderData& particle) {
    return !particle.attached &&
        particle.source_task_id > 0 &&
        (particle.status == "particle-execute" ||
         particle.status == "particle-resume");
}

bool particle_slot_counts_as_fade_running_work(const ParticleRenderData& particle) {
    return !particle.attached &&
        particle.source_task_id > 0 &&
        (particle.status == "particle-fade-execute" ||
         particle.status == "particle-fade-resume");
}

bool p3_queue_contains_task_id(int task_id) {
    return std::any_of(
        g_p3_queue.begin(),
        g_p3_queue.end(),
        [task_id](const StoredTask& entry) {
            return entry.task && entry.task->id == task_id;
        });
}

P3FairnessMetrics count_p3_fairness_metrics(const RenderData& data) {
    P3FairnessMetrics metrics;
    metrics.move_queued = static_cast<int>(std::count_if(
        g_p3_queue.begin(),
        g_p3_queue.end(),
        [](const StoredTask& entry) {
            return entry.task && entry.task->type == TaskType::SINGLE_PARTICLE;
        }));
    metrics.fade_queued = static_cast<int>(std::count_if(
        g_p3_queue.begin(),
        g_p3_queue.end(),
        [](const StoredTask& entry) {
            return entry.task && entry.task->type == TaskType::FADE_PARTICLE;
        }));
    metrics.move_running = static_cast<int>(std::count_if(
        data.particles.begin(),
        data.particles.end(),
        [](const ParticleRenderData& particle) {
            return particle_slot_counts_as_move_running_work(particle) &&
                !p3_queue_contains_task_id(particle.source_task_id);
        }));
    metrics.fade_running = static_cast<int>(std::count_if(
        data.particles.begin(),
        data.particles.end(),
        [](const ParticleRenderData& particle) {
            return particle_slot_counts_as_fade_running_work(particle) &&
                !p3_queue_contains_task_id(particle.source_task_id);
        }));
    return metrics;
}

void reconcile_particle_bookkeeping(RuntimeState& state_ref, RenderData& data) {
    const P3FairnessMetrics p3_metrics = count_p3_fairness_metrics(data);
    state_ref.world.remaining_particles = count_attached_particles(data);
    state_ref.world.queued_particle_tasks = p3_metrics.total();
    data.ui.remaining_particles = state_ref.world.remaining_particles;
    data.ui.queued_particle_tasks = state_ref.world.queued_particle_tasks;
}

void reconcile_runtime_particle_bookkeeping() {
    reconcile_particle_bookkeeping(runtime_state(), render_data());
}

bool requeue_interrupted_task(StoredTask& entry, bool resume_task, const std::string& reason) {
    if (!entry.task) {
        return false;
    }

    std::unique_ptr<Task> requeued = entry.task->clone_for_requeue();
    if (!requeued) {
        return false;
    }

    requeued->id = entry.task->id;
    requeued->last_completed_frame = g_frame_index;
    requeued->last_interrupt_reason = reason;
    mark_task_requeued(*requeued, requeue_action_for(resume_task, false));
    enqueue_with_policy(std::move(requeued));
    return true;
}

bool run_particle_rr_slice_from_batch() {
    StoredTask entry;
    if (!pop_next_eligible_task(g_p3_queue, false, entry)) {
        return false;
    }

    Task& task = *entry.task;
    const bool resume_task =
        task.state == TaskState::REQUEUED || task.state == TaskState::INTERRUPTED;
    mark_task_dispatched(task, g_frame_index, resume_task);
    task.state = TaskState::RUNNING;
    if (resume_task) {
        task.resume();
    } else {
        task.execute();
    }

    if (task.state == TaskState::FINISHED) {
        mark_task_finished(task, g_frame_index, "batch-rr-finished");
        push_runtime_note("Batch RR finished " + task_debug_label(task));
        return true;
    }

    if (task.state == TaskState::INTERRUPTED ||
        (task.state == TaskState::RUNNING && task.support_resume)) {
        requeue_interrupted_task(entry, resume_task, "batch-rr-timeslice");
        push_runtime_note("Batch RR requeued " + task_debug_label(task));
        return true;
    }

    mark_task_finished(task, g_frame_index, "batch-rr-finished-no-resume");
    push_runtime_note("Batch RR closed " + task_debug_label(task));
    return true;
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

bool microphone_window_is_fresh(const RuntimeState& state) {
    return ::microphone_window_is_fresh(state);
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

void seed_mode_specific_short_listener_if_needed(const char* reason) {
    ::seed_mode_specific_short_listener_if_needed_impl(reason);
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

bool run_particle_rr_slice_from_batch() {
    return ::run_particle_rr_slice_from_batch();
}

}  // namespace scheduler_task_support

void bootstrap_runtime() {
    g_next_task_id = 1;
    g_frame_index = 0;
    g_thread_mode = 1;
    g_single_thread_mixed_turn_prefers_p3 = false;
    g_single_thread_p2_phase = 0;
    g_last_multithread_camera_reseed_note_frame = -1000;
    g_last_multithread_microphone_reseed_note_frame = -1000;
    g_last_multithread_realtime_slice_type = TaskType::NONE;
    g_p1_queue.clear();
    g_p2_realtime_queue.clear();
    g_p2_queue.clear();
    g_p3_queue.clear();
    rebuild_thread_pool();
    runtime_state() = RuntimeState{};
    set_runtime_phase(RuntimePhase::BOOTSTRAP);
    reset_simulation_world();
    reconcile_listener_queue_bookkeeping();
}

void reset_runtime() {
    g_frame_index = 0;
    g_single_thread_mixed_turn_prefers_p3 = false;
    g_single_thread_p2_phase = 0;
    g_last_multithread_camera_reseed_note_frame = -1000;
    g_last_multithread_microphone_reseed_note_frame = -1000;
    g_last_multithread_realtime_slice_type = TaskType::NONE;
    g_p1_queue.clear();
    g_p2_realtime_queue.clear();
    g_p2_queue.clear();
    g_p3_queue.clear();
    rebuild_thread_pool();
    runtime_state().shutdown_requested = false;
    set_runtime_phase(RuntimePhase::BOOTSTRAP);
    reset_simulation_world();
    reconcile_listener_queue_bookkeeping();
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
    const int previous_mode = g_thread_mode;
    g_thread_mode = std::clamp(mode, 1, 3);
    if (g_thread_mode == 1) {
        g_single_thread_p2_phase = 0;
    }
    if (g_threads.empty()) {
        rebuild_thread_pool();
    }
    apply_thread_mode_state(g_thread_mode);
    if (g_thread_mode < previous_mode) {
        push_runtime_note(
            "Scheduler: collapsed worker mode " +
            std::to_string(previous_mode) + " -> " + std::to_string(g_thread_mode));
        if (g_thread_mode == 1) {
            erase_realtime_tasks_if(
                [](const StoredTask& entry) {
                    return entry.task &&
                        is_persistent_realtime_listener_task(entry.task->type);
                },
                "multi-thread -> single-thread fallback");
            align_single_thread_phase_to_pending_work();
            ensure_single_thread_listener_burst_tasks();
        } else {
            normalize_realtime_listener_tasks_for_current_mode(
                "multi-thread mode collapse normalization");
            ensure_realtime_listener_service_tasks();
            seed_mode_specific_short_listener_if_needed_impl("mode-collapse");
            ensure_single_thread_listener_burst_tasks();
        }
    } else if (g_thread_mode > previous_mode) {
        push_runtime_note(
            "Scheduler: expanded worker mode " +
            std::to_string(previous_mode) + " -> " + std::to_string(g_thread_mode));
        normalize_realtime_listener_tasks_for_current_mode(
            previous_mode == 1
                ? "single-thread -> multi-thread expansion"
                : "multi-thread mode expansion normalization");
        ensure_realtime_listener_service_tasks();
        seed_mode_specific_short_listener_if_needed_impl("mode-expansion");
        ensure_single_thread_listener_burst_tasks();
    }
    with_shared_state_write(false, false, true, []() {
        render_data().ui.thread_mode = current_thread_mode();
    });
}

int current_thread_mode() {
    return g_thread_mode;
}

void scheduler_tick() {
    ++g_frame_index;
    runtime_state().counters.realtime_slices_this_frame = 0;
    const int active_threads = std::clamp(g_thread_mode, 1, 3);
    int dispatch_slot = 0;
    std::vector<FrameDispatchEntry> frame_dispatches;
    int selected_preemption_victim_index = -1;

    for (RuntimeThread& thread : g_threads) {
        if (thread.state == ThreadState::WAITING) {
            thread.state = ThreadState::IDLE;
            thread.bound_task_id = -1;
            thread.bound_task_name = "none";
            thread.bound_task_type = TaskType::NONE;
            thread.bound_task_priority = PriorityLevel::P2_FUNCTIONAL;
            thread.last_task_event = "waiting-ready";
        } else if (thread.state != ThreadState::SLEEPING && thread.state != ThreadState::CLOSED) {
            thread.state = ThreadState::IDLE;
            thread.bound_task_id = -1;
            thread.bound_task_name = "none";
            thread.bound_task_type = TaskType::NONE;
            thread.bound_task_priority = PriorityLevel::P2_FUNCTIONAL;
            thread.last_task_event = "idle";
        }
    }

    run_listener_watchdog_recovery_if_needed();
    ensure_realtime_listener_service_tasks();
    ensure_single_thread_listener_burst_tasks();
    if (g_thread_mode == 1) {
        run_realtime_service_slices_if_needed();
    }
    ensure_single_thread_focus_entry_task();
    ensure_multithread_camera_entry_task();
    ensure_multithread_microphone_entry_task();

    if (!has_pending_tasks()) {
        seed_input_entry_task_if_idle();
    }

    if (!g_p3_queue.empty() &&
        !has_queued_task(TaskType::BATCH_PARTICLE_EXECUTION, PriorityLevel::P2_FUNCTIONAL)) {
        submit_task(make_batch_particle_execution_task());
    }

    if (!has_pending_tasks()) {
        reconcile_runtime_particle_bookkeeping();
        reconcile_listener_queue_bookkeeping();
        finalize_frame_counters();
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
        if (active_threads > 1) {
            run_realtime_service_slices_if_needed();
        }

        StoredTask entry;
        if (!take_next_task_for_dispatch(dispatch_slot, active_threads, thread, entry)) {
            thread.last_task_event = active_threads > 1 ? "lane-no-match" : "idle";
            thread.last_task_transition = "idle";
            if (active_threads > 1) {
                continue;
            }
            break;
        }
        Task& task = *entry.task;
        const bool resume_task =
            task.state == TaskState::REQUEUED || task.state == TaskState::INTERRUPTED;
        thread.state = ThreadState::RUNNING;
        thread.bound_task_id = task.id;
        thread.bound_task_name = task_debug_label(task);
        thread.bound_task_type = task.type;
        thread.bound_task_priority = task.priority;
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

        if (task.state == TaskState::INTERRUPTED ||
            (task.state == TaskState::RUNNING && task.support_resume)) {
            requeue_interrupted_task(entry, resume_task, "timeslice-expired");
            frame_dispatches.push_back({
                task.id,
                thread.id - 1,
                dispatch_slot,
                task.priority,
                resume_task,
            });
            thread.state = ThreadState::WAITING;
            thread.last_task_event = "interrupted-requeued";
            thread.last_task_transition = "requeued";
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
        thread.last_completed_task_type = task.type;
        thread.last_completed_task_priority = task.priority;
        dispatch_slot++;

        if (task.type == TaskType::RESET || task.type == TaskType::EXIT_APP) {
            mark_noncurrent_runnable_threads_waiting(
                thread.id,
                task.type == TaskType::RESET ? "reset-waiting" : "exit-waiting");
            break;
        }
    }

    if (g_thread_mode == 1) {
        ensure_single_thread_listener_burst_tasks();
    }

    if (g_thread_mode == 1 && !has_pending_tasks()) {
        seed_input_entry_task_if_idle_impl("cycle-drained");
    }

    reconcile_runtime_particle_bookkeeping();
    reconcile_listener_queue_bookkeeping();
    finalize_frame_counters();
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

const std::vector<TaskRecord>& queued_p2_realtime_tasks() {
    static std::vector<TaskRecord> snapshot;
    snapshot = snapshot_queue(g_p2_realtime_queue);
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
    const P3FairnessMetrics p3_metrics = count_p3_fairness_metrics(current_render_data());
    const SingleThreadChainDiagnostics chain_diagnostics =
        build_single_thread_chain_diagnostics(current_runtime_state(), p3_metrics);
    const L1RealtimeDiagnostics l1_diagnostics =
        build_l1_realtime_diagnostics(current_runtime_state());
    snapshot.frame_index = g_frame_index;
    snapshot.thread_mode = g_thread_mode;
    snapshot.visualization_enabled = g_visualization_enabled;
    snapshot.shutdown_requested = current_runtime_state().shutdown_requested;
    snapshot.single_thread_chain_active = chain_diagnostics.active;
    snapshot.l1_realtime_active = l1_diagnostics.active;
    snapshot.remaining_particles = current_render_data().ui.remaining_particles;
    snapshot.power = current_render_data().ui.power;
    snapshot.p3_move_queued = p3_metrics.move_queued;
    snapshot.p3_fade_queued = p3_metrics.fade_queued;
    snapshot.p3_move_running = p3_metrics.move_running;
    snapshot.p3_fade_running = p3_metrics.fade_running;
    snapshot.single_thread_chain_current = chain_diagnostics.current;
    snapshot.single_thread_chain_next = chain_diagnostics.next;
    snapshot.single_thread_chain_status = chain_diagnostics.status;
    snapshot.l1_camera_lane = l1_diagnostics.camera_lane;
    snapshot.l1_microphone_lane = l1_diagnostics.microphone_lane;
    snapshot.l1_gate_lane = l1_diagnostics.gate_lane;
    snapshot.l1_blocked_reason = l1_diagnostics.blocked_reason;
    snapshot.counters = current_runtime_state().counters;
    snapshot.camera_listener_runtime =
        build_listener_service_diagnostics(current_runtime_state().camera_listener);
    snapshot.microphone_listener_runtime =
        build_listener_service_diagnostics(current_runtime_state().microphone_listener);
    snapshot.human_behavior_flow = current_runtime_state().human_behavior_flow;
    snapshot.particle_root_flow = current_runtime_state().particle_root_flow;
    snapshot.threads = g_threads;
    snapshot.p1_queue = snapshot_queue(g_p1_queue);
    snapshot.p2_realtime_queue = snapshot_queue(g_p2_realtime_queue);
    snapshot.p2_queue = snapshot_queue(g_p2_queue);
    snapshot.p3_queue = snapshot_queue(g_p3_queue);
    return snapshot;
}

const char* scheduler_queue_label(PriorityLevel priority) {
    return scheduler_queue_label_impl(priority);
}

const char* latency_class_label_for_task(TaskType type, PriorityLevel priority) {
    return latency_class_label_impl(type, priority);
}

const char* task_tree_node_label_for_task(TaskType type) {
    return task_tree_node_label_impl(type);
}

void set_visualization_running(bool running) {
    g_visualization_enabled = running;
}

void clear_task_queue(PriorityLevel priority) {
    queue_for_priority(priority).clear();
    if (priority == PriorityLevel::P3_PARTICLE) {
        reconcile_runtime_particle_bookkeeping();
    }
    reconcile_listener_queue_bookkeeping();
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
