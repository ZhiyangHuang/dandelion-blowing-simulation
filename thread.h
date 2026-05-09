#ifndef THREAD_H
#define THREAD_H

#include <deque>
#include <functional>
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

enum class RuntimePhase {
    BOOTSTRAP,
    STARTING,
    RESETTING,
    READY,
    SHUTTING_DOWN
};

struct ParticleRenderData {
    int id = 0;
    float x = 0.0f;
    float y = 0.0f;
    bool attached = true;
    bool active = false;
    int ownership_token = 0;
    int source_task_id = -1;
    std::string status = "idle";
};

struct BackgroundLayer {
    std::string theme = "meadow";
};

struct CameraLayer {
    bool enabled = false;
    bool mouth_detected = false;
    float mouth_x = 0.5f;
    float mouth_y = 0.5f;
    int update_tick = 0;
    std::string status = "offline";
};

struct WindLayer {
    bool active = false;
    float power = 0.1f;
    int sample_tick = 0;
    int tick_count = 0;
    std::string status = "idle";
};

struct UIRenderData {
    std::string banner = "Scaffold";
    std::string scheduler_state = "Idle";
    std::string phase_label = "BOOTSTRAP";
    std::string camera_task_status = "offline";
    std::string microphone_task_status = "offline";
    std::string generate_task_status = "idle";
    std::string batch_task_status = "idle";
    std::string particle_task_status = "idle";
    int thread_mode = 1;
    int remaining_particles = 100;
    int queued_particle_tasks = 0;
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
    int dispatch_count = 0;
    int last_completed_task_id = -1;
    std::string last_task_event = "idle";
};

struct TaskRecord {
    int id = 0;
    TaskType type = TaskType::PLACEHOLDER;
    PriorityLevel priority = PriorityLevel::P2_FUNCTIONAL;
    TaskState state = TaskState::CREATED;
    bool support_resume = false;
    std::string name;
    int dispatch_count = 0;
    int execute_count = 0;
    int resume_count = 0;
    int interrupt_count = 0;
    int requeue_count = 0;
    int last_scheduled_frame = 0;
    int last_completed_frame = 0;
    bool last_run_used_resume = false;
    std::string last_queue_action = "none";
    std::string last_interrupt_reason = "none";
    std::string last_transition = "created";
};

struct SchedulerSnapshot {
    int frame_index = 0;
    int thread_mode = 1;
    bool visualization_enabled = false;
    bool shutdown_requested = false;
    int remaining_particles = 100;
    float power = 0.1f;
    std::vector<RuntimeThread> threads;
    std::vector<TaskRecord> p1_queue;
    std::vector<TaskRecord> p2_queue;
    std::vector<TaskRecord> p3_queue;
};

struct SharedWorldState {
    float dandelion_x = 0.5f;
    float dandelion_y = 0.5f;
    float mouth_x = 0.5f;
    float mouth_y = 0.5f;
    int remaining_particles = 100;
    int queued_particle_tasks = 0;
    float power = 0.1f;
};

struct CameraBridgeState {
    bool bridge_connected = false;
    bool sample_ready = false;
    bool face_detected = false;
    bool mouth_open_state = false;
    bool looking_forward = false;
    float mouth_center_x = 0.5f;
    float mouth_center_y = 0.5f;
    float confidence = 0.0f;
    float jaw_open_score = 0.0f;
    float mouth_open_ratio = 0.0f;
    long long timestamp_ms = 0;
    std::string backend = "camera-bridge-unset";
};

struct MicrophoneBridgeState {
    bool bridge_connected = false;
    bool sample_ready = false;
    bool voice_detected = false;
    bool fallback_requested = false;
    float suggested_power = 0.1f;
    float direction_x = 0.0f;
    float direction_y = -1.0f;
    float confidence = 0.0f;
    long long timestamp_ms = 0;
    std::string backend = "microphone-bridge-unset";
};

struct RuntimeState {
    RuntimePhase phase = RuntimePhase::BOOTSTRAP;
    bool shutdown_requested = false;
    bool camera_available = false;
    bool microphone_available = false;
    bool camera_gate_open = false;
    int camera_gate_frame = -1;
    long long last_consumed_microphone_sample_ms = 0;
    SharedWorldState world;
    CameraBridgeState camera_bridge;
    MicrophoneBridgeState microphone_bridge;
};

struct LockDebugState {
    int particle_lock_count = 0;
    int power_lock_count = 0;
    int render_lock_count = 0;
    std::string last_lock_sequence = "none";
    std::string last_ordered_write_sequence = "none";
};

struct Task {
    int id = 0;
    TaskType type = TaskType::PLACEHOLDER;
    PriorityLevel priority = PriorityLevel::P2_FUNCTIONAL;
    TaskState state = TaskState::CREATED;
    bool support_resume = false;
    std::string name;
    int dispatch_count = 0;
    int execute_count = 0;
    int resume_count = 0;
    int interrupt_count = 0;
    int requeue_count = 0;
    int last_scheduled_frame = 0;
    int last_completed_frame = 0;
    bool last_run_used_resume = false;
    std::string last_queue_action = "none";
    std::string last_interrupt_reason = "none";
    std::string last_transition = "created";

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

class StartTask final : public Task {
public:
    StartTask();
    void execute() override;
};

class ResetTask final : public Task {
public:
    ResetTask();
    void execute() override;
};

class ExitTask final : public Task {
public:
    ExitTask();
    void execute() override;
};

class CameraTask final : public Task {
public:
    CameraTask();
    void execute() override;
    void resume() override;

private:
    void run_cycle(bool resumed);

    int cycle_count_ = 0;
};

class MicrophoneTask final : public Task {
public:
    MicrophoneTask();
    void execute() override;
    void resume() override;

private:
    void run_cycle(bool resumed);

    int sample_count_ = 0;
};

class GenerateParticleTask final : public Task {
public:
    GenerateParticleTask();
    void execute() override;

private:
    int compute_spawn_count(float power) const;
};

class BreezeTask final : public Task {
public:
    BreezeTask();
    void execute() override;
};

class ChangeDandelionTask final : public Task {
public:
    ChangeDandelionTask();
    void execute() override;
};

class BatchParticleExecutionTask final : public Task {
public:
    BatchParticleExecutionTask();
    void execute() override;
    void resume() override;

private:
    void run_cycle(bool resumed);

    int tick_count_ = 0;
    int decay_accumulator_ = 0;
};

class SingleParticleTask final : public Task {
public:
    SingleParticleTask(int particle_slot, int generation_index, int ownership_token);
    void execute() override;
    void resume() override;

private:
    void run_cycle(bool resumed);

    int particle_slot_ = 0;
    int generation_index_ = 0;
    int ownership_token_ = 0;
    int cycle_count_ = 0;
    bool initialized_ = false;
    bool completed_ = false;
    float x_ = 0.0f;
    float y_ = 0.0f;
    float vx_ = 0.0f;
    float vy_ = 0.0f;
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
std::unique_ptr<Task> make_start_task();
std::unique_ptr<Task> make_reset_task();
std::unique_ptr<Task> make_exit_task();
std::unique_ptr<Task> make_camera_task();
std::unique_ptr<Task> make_microphone_task();
std::unique_ptr<Task> make_generate_particle_task();
std::unique_ptr<Task> make_breeze_task();
std::unique_ptr<Task> make_change_dandelion_task();
std::unique_ptr<Task> make_batch_particle_execution_task();
std::unique_ptr<Task> make_single_particle_task(int particle_slot,
                                                int generation_index,
                                                int ownership_token = 0);

void set_thread_mode(int mode);
int current_thread_mode();

const std::vector<RuntimeThread>& runtime_threads();
const std::vector<TaskRecord>& queued_p1_tasks();
const std::vector<TaskRecord>& queued_p2_tasks();
const std::vector<TaskRecord>& queued_p3_tasks();
SchedulerSnapshot scheduler_snapshot();

RenderData& render_data();
const RenderData& current_render_data();
RuntimeState& runtime_state();
const RuntimeState& current_runtime_state();
LockDebugState lock_debug_state();
void set_runtime_phase(RuntimePhase phase);
const char* runtime_phase_label(RuntimePhase phase);
void request_shutdown();
bool shutdown_requested();
void clear_task_queue(PriorityLevel priority);
bool has_queued_task(TaskType type, PriorityLevel priority);
void with_shared_state_write(bool lock_particle,
                             bool lock_power,
                             bool lock_render,
                             const std::function<void()>& fn);

void push_runtime_note(const std::string& note);
const std::vector<std::string>& current_runtime_notes();
std::vector<std::string> drain_runtime_notes();
void refresh_bridge_inputs();

bool init_visualization();
void shutdown_visualization();
void process_visual_input();
void render_visual_frame();
bool visualization_running();
void set_visualization_running(bool running);

#endif
