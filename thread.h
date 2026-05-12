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
    P2_REALTIME,
    P2_FUNCTIONAL,
    P3_PARTICLE
};

enum class TaskType {
    START,
    RESET,
    CAMERA_LISTENER_SERVICE,
    MICROPHONE_LISTENER_SERVICE,
    CAMERA_LISTENER_BURST,
    MICROPHONE_LISTENER_BURST,
    CAMERA,
    MICROPHONE,
    GENERATE_PARTICLE,
    BREEZE,
    CHANGE_DANDELION,
    BATCH_PARTICLE_EXECUTION,
    SINGLE_PARTICLE,
    FADE_PARTICLE,
    EXIT_APP,
    PLACEHOLDER,
    NONE
};

enum class RuntimePhase {
    BOOTSTRAP,
    STARTING,
    RESETTING,
    READY,
    SHUTTING_DOWN
};

enum class OrchestrationNode {
    IDLE,
    CAMERA_DETECT,
    BLOW_DETECT,
    PARTICLE_GENERATE,
    PARTICLE_BATCH,
    PARTICLE_MOVE,
    PARTICLE_FADE,
    CHANGE_DANDELION
};

enum class OrchestrationStatus {
    IDLE,
    WAITING,
    ACTIVE,
    HANDOFF,
    FALLBACK,
    DRAINED,
    COMPLETED,
    BLOCKED,
    ERROR_STATE
};

enum class OrchestrationEvent {
    NONE,
    CAMERA_DEVICE_WAIT,
    CAMERA_MOUTH_WAIT,
    CAMERA_GATE_HOLD,
    CAMERA_DEVICE_UNAVAILABLE,
    MOUTH_OPEN,
    MICROPHONE_DEVICE_UNAVAILABLE,
    MICROPHONE_DEVICE_WAIT,
    MICROPHONE_BRIDGE_DISCONNECTED,
    MICROPHONE_SAMPLE_WAIT,
    MICROPHONE_GATE_WAIT,
    MICROPHONE_SAMPLE_STALE,
    VOICE_WAIT,
    BLOW_FALLBACK,
    BLOW_DETECTED,
    PARTICLE_GENERATE,
    PARTICLE_GENERATE_EMPTY,
    PARTICLE_BATCH_TICK,
    PARTICLE_BATCH_HANDOFF,
    PARTICLE_STAGE_DRAINED,
    PARTICLE_MOVE_EXECUTE,
    PARTICLE_MOVE_BOUNDARY_STOP,
    PARTICLE_MOVE_LOST_SLOT,
    PARTICLE_MOVE_LOST_OWNERSHIP,
    PARTICLE_FADE_EXECUTE,
    PARTICLE_FADE_FINISHED,
    PARTICLE_FADE_EMPTY,
    PARTICLE_FADE_LOST_SLOT,
    PARTICLE_FADE_LOST_OWNERSHIP,
    CHANGE_DANDELION
};

struct ParticleRenderData {
    int id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float visual_alpha = 1.0f;
    bool attached = true;
    bool active = false;
    int ownership_token = 0;
    int source_task_id = -1;
    int fade_steps_remaining = 0;
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
    std::string input_focus_status = "FREE FOCUS";
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
    std::string bound_task_name = "none";
    TaskType bound_task_type = TaskType::NONE;
    PriorityLevel bound_task_priority = PriorityLevel::P2_FUNCTIONAL;
    int dispatch_count = 0;
    int last_completed_task_id = -1;
    std::string last_completed_task_name = "none";
    TaskType last_completed_task_type = TaskType::NONE;
    PriorityLevel last_completed_task_priority = PriorityLevel::P2_FUNCTIONAL;
    std::string last_task_event = "idle";
    std::string last_task_transition = "created";
    TaskType pinned_task_type = TaskType::NONE;
    bool is_pinned = false;
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

struct OrchestrationRecord {
    std::string name = "Flow";
    bool active = false;
    OrchestrationNode current_node = OrchestrationNode::IDLE;
    OrchestrationStatus current_status = OrchestrationStatus::IDLE;
    OrchestrationEvent last_event = OrchestrationEvent::NONE;
    TaskType current_leaf_type = TaskType::NONE;
    PriorityLevel current_leaf_priority = PriorityLevel::P2_FUNCTIONAL;
    int current_leaf_id = -1;
    int last_updated_frame = 0;
};

struct ListenerServiceDiagnostics {
    bool service_task_alive = false;
    int service_task_id = -1;
    bool consumer_task_queued = false;
    int consumer_task_id = -1;
    bool short_lease_active = false;
    long long short_lease_started_at_ms = 0;
    long long short_lease_until_ms = 0;
    long long short_detect_ready_at_ms = 0;
    long long last_heartbeat_ms = 0;
    long long last_seen_sample_ms = 0;
    long long last_seeded_sample_ms = 0;
    long long last_consumed_sample_ms = 0;
};

struct RuntimeCounters {
    int camera_reseed_count = 0;
    int microphone_reseed_count = 0;
    int watchdog_recovery_count = 0;
    int realtime_slices_this_frame = 0;
    int realtime_slices_last_frame = 0;
    int max_realtime_slices_per_frame = 0;
    int particle_drain_cycles_completed = 0;
    long long total_particle_drain_ticks = 0;
    double average_particle_drain_ticks = 0.0;
};

struct SchedulerSnapshot {
    int frame_index = 0;
    int thread_mode = 1;
    bool visualization_enabled = false;
    bool shutdown_requested = false;
    bool single_thread_chain_active = false;
    bool l1_realtime_active = false;
    int remaining_particles = 100;
    float power = 0.1f;
    int p3_move_queued = 0;
    int p3_fade_queued = 0;
    int p3_move_running = 0;
    int p3_fade_running = 0;
    std::string single_thread_chain_path =
        "CameraBurst -> Camera -> MicrophoneBurst -> Microphone -> Generate/Breeze -> Batch -> P3 RR -> drain -> CameraBurst";
    std::string single_thread_chain_current = "n/a";
    std::string single_thread_chain_next = "n/a";
    std::string single_thread_chain_status = "n/a";
    std::string l1_camera_lane = "n/a";
    std::string l1_microphone_lane = "n/a";
    std::string l1_gate_lane = "n/a";
    std::string l1_blocked_reason = "n/a";
    RuntimeCounters counters;
    ListenerServiceDiagnostics camera_listener_runtime;
    ListenerServiceDiagnostics microphone_listener_runtime;
    OrchestrationRecord human_behavior_flow;
    OrchestrationRecord particle_root_flow;
    std::vector<RuntimeThread> threads;
    std::vector<TaskRecord> p1_queue;
    std::vector<TaskRecord> p2_realtime_queue;
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
    bool device_unavailable = false;
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
    std::string status_text = "camera bridge idle";
};

struct MicrophoneBridgeState {
    bool bridge_connected = false;
    bool sample_ready = false;
    bool device_unavailable = false;
    bool voice_detected = false;
    bool fallback_requested = false;
    float suggested_power = 0.1f;
    float direction_x = 0.0f;
    float direction_y = -1.0f;
    float confidence = 0.0f;
    long long timestamp_ms = 0;
    std::string backend = "microphone-bridge-unset";
    std::string status_text = "microphone bridge idle";
};

struct CameraListenerState {
    bool enabled = false;
    bool bridge_running = false;
    bool device_available = false;
    bool sample_ready = false;
    bool stale = true;
    bool unavailable = false;
    long long started_at_ms = 0;
    long long first_mouth_seen_at_ms = 0;
    bool service_task_alive = false;
    int service_task_id = -1;
    bool consumer_task_queued = false;
    int consumer_task_id = -1;
    bool short_lease_active = false;
    long long short_lease_started_at_ms = 0;
    long long short_lease_until_ms = 0;
    long long short_warmup_until_ms = 0;
    long long short_detect_ready_at_ms = 0;
    long long last_heartbeat_ms = 0;
    long long last_seen_sample_ms = 0;
    long long last_seeded_sample_ms = 0;
    long long last_consumed_sample_ms = 0;
};

struct MicrophoneListenerState {
    bool enabled = false;
    bool bridge_running = false;
    bool device_available = false;
    bool sample_ready = false;
    bool stale = true;
    bool unavailable = false;
    bool service_task_alive = false;
    int service_task_id = -1;
    bool consumer_task_queued = false;
    int consumer_task_id = -1;
    bool short_lease_active = false;
    long long short_lease_started_at_ms = 0;
    long long short_lease_until_ms = 0;
    long long short_warmup_until_ms = 0;
    long long short_detect_ready_at_ms = 0;
    long long last_heartbeat_ms = 0;
    long long last_seen_sample_ms = 0;
    long long last_seeded_sample_ms = 0;
    long long last_consumed_sample_ms = 0;
};

struct RuntimeState {
    RuntimePhase phase = RuntimePhase::BOOTSTRAP;
    bool shutdown_requested = false;
    bool camera_device_available = false;
    bool microphone_device_available = false;
    bool camera_available = false;
    bool microphone_available = false;
    bool camera_focus_locked = true;
    bool microphone_focus_locked = false;
    bool microphone_focus_consumed_for_gate = false;
    bool camera_gate_open = false;
    int camera_gate_frame = -1;
    long long camera_gate_until_ms = 0;
    long long microphone_focus_until_ms = 0;
    long long last_consumed_microphone_sample_ms = 0;
    SharedWorldState world;
    OrchestrationRecord human_behavior_flow{"HumanBehaviorTask"};
    OrchestrationRecord particle_root_flow{"ParticleRootTask"};
    CameraListenerState camera_listener;
    MicrophoneListenerState microphone_listener;
    CameraBridgeState camera_bridge;
    MicrophoneBridgeState microphone_bridge;
    RuntimeCounters counters;
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
    int realtime_order = 0;
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
    virtual std::unique_ptr<Task> clone_for_requeue() const = 0;
};

class PlaceholderTask final : public Task {
public:
    PlaceholderTask(TaskType task_type,
                    PriorityLevel task_priority,
                    std::string task_name,
                    bool resumable = false);

    void execute() override;
    std::unique_ptr<Task> clone_for_requeue() const override;
};

class StartTask final : public Task {
public:
    StartTask();
    void execute() override;
    std::unique_ptr<Task> clone_for_requeue() const override;
};

class ResetTask final : public Task {
public:
    ResetTask();
    void execute() override;
    std::unique_ptr<Task> clone_for_requeue() const override;
};

class ExitTask final : public Task {
public:
    ExitTask();
    void execute() override;
    std::unique_ptr<Task> clone_for_requeue() const override;
};

class CameraListenerServiceTask final : public Task {
public:
    CameraListenerServiceTask();
    void execute() override;
    void resume() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    void run_cycle(bool resumed);
};

class MicrophoneListenerServiceTask final : public Task {
public:
    MicrophoneListenerServiceTask();
    void execute() override;
    void resume() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    void run_cycle(bool resumed);
};

class CameraListenerBurstTask final : public Task {
public:
    CameraListenerBurstTask();
    void execute() override;
    void resume() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    void run_cycle(bool resumed);
};

class MicrophoneListenerBurstTask final : public Task {
public:
    MicrophoneListenerBurstTask();
    void execute() override;
    void resume() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    void run_cycle(bool resumed);
};

class CameraTask final : public Task {
public:
    CameraTask();
    void execute() override;
    void resume() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    void run_cycle(bool resumed);

    int cycle_count_ = 0;
};

class MicrophoneTask final : public Task {
public:
    MicrophoneTask();
    void execute() override;
    void resume() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    void run_cycle(bool resumed);

    int sample_count_ = 0;
};

class GenerateParticleTask final : public Task {
public:
    GenerateParticleTask();
    void execute() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    int compute_spawn_count(float power) const;
};

class BreezeTask final : public Task {
public:
    BreezeTask();
    void execute() override;
    std::unique_ptr<Task> clone_for_requeue() const override;
};

class ChangeDandelionTask final : public Task {
public:
    ChangeDandelionTask();
    void execute() override;
    std::unique_ptr<Task> clone_for_requeue() const override;
};

class BatchParticleExecutionTask final : public Task {
public:
    BatchParticleExecutionTask();
    void execute() override;
    void resume() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    void run_cycle(bool resumed);

    int tick_count_ = 0;
    long long decay_accumulator_ms_ = 0;
    long long last_decay_timestamp_ms_ = 0;
    bool started_ = false;
};

class SingleParticleTask final : public Task {
public:
    SingleParticleTask(int particle_slot,
                       int generation_index,
                       int ownership_token,
                       float mouth_x,
                       float mouth_y);
    void execute() override;
    void resume() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    void run_cycle(bool resumed);

    int particle_slot_ = 0;
    int generation_index_ = 0;
    int ownership_token_ = 0;
    int cycle_count_ = 0;
    bool initialized_ = false;
    bool completed_ = false;
    float mouth_x_ = 0.5f;
    float mouth_y_ = 0.5f;
    float x_ = 0.0f;
    float y_ = 0.0f;
    float dir_x_ = 0.0f;
    float dir_y_ = 0.0f;
    float distance_per_tick_ = 0.0f;
};

class ParticleFadeTask final : public Task {
public:
    ParticleFadeTask(int particle_slot, int ownership_token);
    void execute() override;
    void resume() override;
    std::unique_ptr<Task> clone_for_requeue() const override;

private:
    void run_cycle(bool resumed);

    int particle_slot_ = 0;
    int ownership_token_ = 0;
    int cycle_count_ = 0;
    bool completed_ = false;
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
std::unique_ptr<Task> make_camera_listener_service_task();
std::unique_ptr<Task> make_microphone_listener_service_task();
std::unique_ptr<Task> make_camera_listener_burst_task();
std::unique_ptr<Task> make_microphone_listener_burst_task();
std::unique_ptr<Task> make_camera_task();
std::unique_ptr<Task> make_microphone_task();
std::unique_ptr<Task> make_generate_particle_task();
std::unique_ptr<Task> make_breeze_task();
std::unique_ptr<Task> make_change_dandelion_task();
std::unique_ptr<Task> make_batch_particle_execution_task();
std::unique_ptr<Task> make_single_particle_task(int particle_slot,
                                                int generation_index,
                                                int ownership_token = 0,
                                                float mouth_x = 0.5f,
                                                float mouth_y = 0.5f);
std::unique_ptr<Task> make_particle_fade_task(int particle_slot,
                                              int ownership_token = 0);

void set_thread_mode(int mode);
int current_thread_mode();

const std::vector<RuntimeThread>& runtime_threads();
const std::vector<TaskRecord>& queued_p1_tasks();
const std::vector<TaskRecord>& queued_p2_realtime_tasks();
const std::vector<TaskRecord>& queued_p2_tasks();
const std::vector<TaskRecord>& queued_p3_tasks();
SchedulerSnapshot scheduler_snapshot();
const char* scheduler_queue_label(PriorityLevel priority);
const char* latency_class_label_for_task(TaskType type, PriorityLevel priority);
const char* task_tree_node_label_for_task(TaskType type);
const char* orchestration_node_label(OrchestrationNode node);
const char* orchestration_status_label(OrchestrationStatus status);
const char* orchestration_event_label(OrchestrationEvent event);
void set_orchestration_record(OrchestrationRecord& record,
                              bool active,
                              OrchestrationNode node,
                              OrchestrationStatus status,
                              OrchestrationEvent event,
                              TaskType leaf_type,
                              PriorityLevel leaf_priority,
                              int leaf_id,
                              int frame_index);

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
bool set_runtime_log_file(const std::string& path);
void close_runtime_log_file();
void start_camera_bridge();
void stop_camera_bridge();
void start_microphone_bridge();
void stop_microphone_bridge();
void set_camera_bridge_enabled(bool enabled);
void set_microphone_bridge_enabled(bool enabled);
bool camera_bridge_enabled();
bool microphone_bridge_enabled();
void set_camera_demo_fallback_enabled(bool enabled);
bool camera_demo_fallback_enabled();
void trigger_camera_demo_pulse();
void set_demo_io_loop_enabled(bool enabled);
bool demo_io_loop_enabled();
void refresh_bridge_inputs();

bool init_visualization();
void shutdown_visualization();
void process_visual_input();
void render_visual_frame();
bool visualization_running();
void set_visualization_running(bool running);

#endif
