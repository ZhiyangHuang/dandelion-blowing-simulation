#ifndef THREAD_H
#define THREAD_H

#include <deque>
#include <memory>
#include <string>
#include <vector>

enum class ThreadState {
    IDLE,
    RUNNING,
    SLEEPING,
    CLOSED,
    WAITING
};

enum class TaskState {
    CREATED,
    RUNNING,
    INTERRUPTED,
    FINISHED,
    REQUEUED
};

enum class PriorityLevel {
    P1_SYSTEM,
    P2_FUNCTIONAL,
    P3_PARTICLE
};

enum class TaskType {
    START,
    RESET,
    CAMERA,
    MICROPHONE,
    GENERATE_PARTICLE,
    BREEZE,
    CHANGE_DANDELION,
    BATCH_PARTICLE_EXECUTION,
    SINGLE_PARTICLE,
    EXIT_APP,
    PLACEHOLDER
};

struct ParticleRenderData {
    int id = 0;
    float x = 0.0f;
    float y = 0.0f;
    bool attached = true;
    bool active = false;
};

struct BackgroundLayer {
    std::string theme = "meadow";
};

struct CameraLayer {
    bool enabled = false;
    bool mouth_detected = false;
    float mouth_x = 0.5f;
    float mouth_y = 0.5f;
};

struct WindLayer {
    bool active = false;
    float power = 0.1f;
};

struct UIRenderData {
    std::string banner = "Scaffold";
    std::string scheduler_state = "Idle";
    int thread_mode = 1;
    int remaining_particles = 100;
    float power = 0.1f;
};

struct RenderData {
    BackgroundLayer sky_grass_layer;
    CameraLayer camera_layer;
    WindLayer wind_layer;
    std::vector<ParticleRenderData> particles;
    UIRenderData ui;
};

struct RuntimeThread {
    int id = 0;
    ThreadState state = ThreadState::IDLE;
    std::string label;
    int bound_task_id = -1;
};

struct TaskRecord {
    int id = 0;
    TaskType type = TaskType::PLACEHOLDER;
    PriorityLevel priority = PriorityLevel::P2_FUNCTIONAL;
    TaskState state = TaskState::CREATED;
    bool support_resume = false;
    std::string name;
};

struct SchedulerSnapshot {
    int frame_index = 0;
    int thread_mode = 1;
    bool visualization_enabled = false;
    int remaining_particles = 100;
    float power = 0.1f;
    std::vector<RuntimeThread> threads;
    std::vector<TaskRecord> p1_queue;
    std::vector<TaskRecord> p2_queue;
    std::vector<TaskRecord> p3_queue;
};

struct Task {
    int id = 0;
    TaskType type = TaskType::PLACEHOLDER;
    PriorityLevel priority = PriorityLevel::P2_FUNCTIONAL;
    TaskState state = TaskState::CREATED;
    bool support_resume = false;
    std::string name;

    virtual ~Task() = default;
    virtual void execute() = 0;
    virtual void resume() { execute(); }
};

class PlaceholderTask final : public Task {
public:
    PlaceholderTask(TaskType task_type,
                    PriorityLevel task_priority,
                    std::string task_name,
                    bool resumable = false);

    void execute() override;
};

void bootstrap_runtime();
void reset_runtime();
void reset_simulation_world();
void seed_startup_flow();
void scheduler_tick();

int submit_task(std::unique_ptr<Task> task);
std::unique_ptr<Task> make_placeholder_task(TaskType type,
                                            PriorityLevel priority,
                                            const std::string& name,
                                            bool support_resume = false);

void set_thread_mode(int mode);
int current_thread_mode();

const std::vector<RuntimeThread>& runtime_threads();
const std::vector<TaskRecord>& queued_p1_tasks();
const std::vector<TaskRecord>& queued_p2_tasks();
const std::vector<TaskRecord>& queued_p3_tasks();
SchedulerSnapshot scheduler_snapshot();

RenderData& render_data();
const RenderData& current_render_data();

void push_runtime_note(const std::string& note);
std::vector<std::string> drain_runtime_notes();

bool init_visualization();
void shutdown_visualization();
void process_visual_input();
void render_visual_frame();
bool visualization_running();
void set_visualization_running(bool running);

#endif
