#ifndef THREAD_H
#define THREAD_H

#include <functional>
#include <queue>
#include <string>
#include <vector>

using namespace std;

enum State {
    READY,
    RUNNING,
    BLOCKED,
    FINISHED
};

enum Priority {
    HIGH,
    MEDIUM,
    LOW
};

enum EventType {
    CAMERA_OPEN,
    CAMERA_CLOSE,
    CAMERA_DETECTED,
    MOUTH_OPEN,
    MIC_START,
    MIC_END,
    PARTICLE_SPAWN,
    PARTICLE_HIT_BORDER,
    BUFFER_OVERFLOW,
    BUFFER_EMPTY,
    RENDER_UPDATE,
    UI_RESET,
    UI_STOP,
    UI_CONTINUE
};

enum RenderState {
    UI_START,
    UI_CAMERA_LOADING,
    UI_INPUT_ACTIVE,
    UI_RUNNING,
    UI_WAITING,
    UI_PAUSED
};

enum ThreadMode {
    THREAD_MODE_1 = 1,
    THREAD_MODE_2 = 2,
    THREAD_MODE_3 = 3
};

struct Event {
    EventType type;
    int value = 0;
    string label;
};

struct ParticleState {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float dir_x = 1.0f;
    float dir_y = 0.0f;
    float force = 0.0f;
    float size = 4.0f;
    int life = 0;
    bool active = false;
    bool stopped_by_border = false;
};

struct Thread {
    int id;
    State state;
    Priority priority;

    bool started = false;
    int time_slice = 0;
    int pc = 0;

    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    int life = 4;
    float dir_x = 1.0f;
    float dir_y = 0.0f;
    float force = 0.0f;
    float size = 4.0f;
    bool active = false;
    bool stopped_by_border = false;

    string name;
    function<void(Thread*)> func;
};

struct Mutex {
    bool locked = false;
    queue<Thread*> waiters;
};

struct LatestValueBuffer {
    int spawn_budget = 0;
    int max_spawn_budget = 100;
    int overflow_count = 0;
    bool mouth_open = false;
    bool wind_active = false;
    bool buffer_paused = false;
    float mouth_x = 0.5f;
    float mouth_y = 0.5f;
    float wind_strength = 0.0f;
    float wind_dir_x = 1.0f;
    float wind_dir_y = 0.0f;
    int seeds_total = 100;
    int seeds_left = 100;
};

struct RuntimeConfig {
    ThreadMode thread_mode = THREAD_MODE_1;
    bool camera_background_enabled = true;
    bool mouth_control_enabled = true;
    bool mic_input_enabled = true;
    int initial_seed_count = 100;
    float mic_upper_threshold = 0.040f;
    float mic_lower_threshold = 0.018f;
};

Thread* create_thread(const string& name, function<void(Thread*)> func, Priority p);
void rebuild_runtime_threads(ThreadMode mode);
int created_thread_count();

void yield();
void block();
void wakeup(Thread* t);

void lock(Mutex* m);
void unlock(Mutex* m);

void event_loop();
void timer_event();

void emit_event(EventType type, int value = 0, const string& label = "");
bool poll_event(Event& event);

LatestValueBuffer& latest_buffer();
RuntimeConfig& runtime_config();
RenderState current_render_state();
void set_render_state(RenderState state);
void set_thread_mode(ThreadMode mode);
ThreadMode current_thread_mode();
void set_motion_halted(bool halted);
bool motion_halted();

bool simulation_done();
void simulation_tick();
void handle_event(const Event& event);
void update_particle(Thread* t);
void reset_simulation();

const vector<Thread*>& all_threads();
void reset_scheduler_state();

bool init_visualization();
void shutdown_visualization();
void process_visual_input();
void render_visual_frame();
bool visualization_running();

void camera(Thread* t);
void mic_thread(Thread* t);
void render_thread(Thread* t);
void particle(Thread* t);

extern Thread* cam;
extern Thread* mic;
extern Thread* render_ui;

extern bool wind_on;

#endif
