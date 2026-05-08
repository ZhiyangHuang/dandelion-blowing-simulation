#include "thread.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct StoredTask {
    std::unique_ptr<Task> task;
};

int g_next_task_id = 1;
int g_frame_index = 0;
int g_thread_mode = 1;
bool g_visualization_enabled = false;
std::vector<RuntimeThread> g_threads;
std::deque<StoredTask> g_p1_queue;
std::deque<StoredTask> g_p2_queue;
std::deque<StoredTask> g_p3_queue;

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

void enqueue_with_policy(std::unique_ptr<Task> task) {
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
}

StoredTask take_next_task() {
    if (!g_p1_queue.empty()) {
        StoredTask entry = std::move(g_p1_queue.back());
        g_p1_queue.pop_back();
        return entry;
    }
    if (!g_p2_queue.empty()) {
        StoredTask entry = std::move(g_p2_queue.front());
        g_p2_queue.pop_front();
        return entry;
    }
    StoredTask entry = std::move(g_p3_queue.front());
    g_p3_queue.pop_front();
    return entry;
}

bool has_pending_tasks() {
    return !g_p1_queue.empty() || !g_p2_queue.empty() || !g_p3_queue.empty();
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

void bootstrap_runtime() {
    g_next_task_id = 1;
    g_frame_index = 0;
    g_thread_mode = 1;
    g_p1_queue.clear();
    g_p2_queue.clear();
    g_p3_queue.clear();
    rebuild_thread_pool();
    reset_simulation_world();
}

void reset_runtime() {
    g_frame_index = 0;
    g_p1_queue.clear();
    g_p2_queue.clear();
    g_p3_queue.clear();
    rebuild_thread_pool();
    reset_simulation_world();
}

void seed_startup_flow() {
    submit_task(make_placeholder_task(
        TaskType::START,
        PriorityLevel::P1_SYSTEM,
        "StartTask"));
    submit_task(make_placeholder_task(
        TaskType::RESET,
        PriorityLevel::P1_SYSTEM,
        "ResetTask"));
    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "CameraTask",
        true));
    submit_task(make_placeholder_task(
        TaskType::BATCH_PARTICLE_EXECUTION,
        PriorityLevel::P2_FUNCTIONAL,
        "BatchParticleExecutionTask",
        true));
}

int submit_task(std::unique_ptr<Task> task) {
    if (!task) {
        return -1;
    }

    task->id = g_next_task_id++;
    task->state = TaskState::CREATED;
    enqueue_with_policy(std::move(task));
    return g_next_task_id - 1;
}

std::unique_ptr<Task> make_placeholder_task(TaskType type,
                                            PriorityLevel priority,
                                            const std::string& name,
                                            bool support_resume) {
    return std::make_unique<PlaceholderTask>(type, priority, name, support_resume);
}

void set_thread_mode(int mode) {
    g_thread_mode = std::clamp(mode, 1, 3);
    rebuild_thread_pool();
    render_data().ui.thread_mode = g_thread_mode;
}

int current_thread_mode() {
    return g_thread_mode;
}

void scheduler_tick() {
    ++g_frame_index;

    for (RuntimeThread& thread : g_threads) {
        if (thread.state != ThreadState::SLEEPING && thread.state != ThreadState::CLOSED) {
            thread.state = ThreadState::IDLE;
            thread.bound_task_id = -1;
        }
    }

    if (!has_pending_tasks()) {
        render_data().ui.scheduler_state = "Idle";
        return;
    }

    for (RuntimeThread& thread : g_threads) {
        if (thread.state == ThreadState::SLEEPING || thread.state == ThreadState::CLOSED) {
            continue;
        }
        if (!has_pending_tasks()) {
            break;
        }

        StoredTask entry = take_next_task();
        Task& task = *entry.task;
        thread.state = ThreadState::RUNNING;
        thread.bound_task_id = task.id;
        task.state = task.support_resume ? TaskState::RUNNING : TaskState::FINISHED;
        task.execute();

        if (task.state != TaskState::FINISHED && task.support_resume) {
            task.state = TaskState::REQUEUED;
            enqueue_with_policy(std::move(entry.task));
        }
    }

    render_data().ui.scheduler_state = has_pending_tasks() ? "Dispatching" : "Idle";
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
