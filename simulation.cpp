#include "thread.h"
#include <algorithm>
#include <iostream>

using namespace std;

Thread* cam = nullptr;
Thread* mic = nullptr;
Thread* render_ui = nullptr;
bool wind_on = false;

static Mutex buffer_lock;

static int camera_bursts_done = 0;
static const int max_camera_bursts = 6;
static int particle_snapshots = 0;
static int scheduler_ticks = 0;
static int camera_signals_sent = 0;
static int mic_signals_sent = 0;
static int active_particles = 3;
static bool particle_motion_halted = false;
static ThreadMode active_thread_mode = THREAD_MODE_1;
static RuntimeConfig config = {};

static bool production_complete() {
    LatestValueBuffer& latest = latest_buffer();
    return camera_bursts_done >= max_camera_bursts &&
        latest.spawn_budget == 0 &&
        latest.seeds_left <= 0;
}

static const char* render_state_name(RenderState state) {
    switch (state) {
    case UI_START:
        return "START";
    case UI_CAMERA_LOADING:
        return "CAMERA_LOADING";
    case UI_INPUT_ACTIVE:
        return "INPUT_ACTIVE";
    case UI_RUNNING:
        return "RUNNING";
    case UI_WAITING:
        return "WAITING";
    case UI_PAUSED:
        return "PAUSED";
    default:
        return "UNKNOWN";
    }
}

RuntimeConfig& runtime_config() {
    return config;
}

void set_thread_mode(ThreadMode mode) {
    active_thread_mode = mode;
    config.thread_mode = mode;
}

ThreadMode current_thread_mode() {
    return active_thread_mode;
}

void set_motion_halted(bool halted) {
    particle_motion_halted = halted;
}

bool motion_halted() {
    return particle_motion_halted;
}

static int compute_spawn_count_from_wind(float wind_strength) {
    if (wind_strength < 0.10f) return 1;
    if (wind_strength < 0.25f) return 3;
    if (wind_strength < 0.45f) return 6;
    if (wind_strength < 0.70f) return 10;
    return 14;
}

static void consume_center_seeds(int count) {
    LatestValueBuffer& latest = latest_buffer();
    count = max(0, count);
    count = min(count, latest.seeds_left);
    latest.seeds_left -= count;
}

static bool buffer_can_accept_spawn(int count) {
    LatestValueBuffer& latest = latest_buffer();
    return latest.spawn_budget + count <= latest.max_spawn_budget;
}

static void handle_buffer_overflow_protection() {
    LatestValueBuffer& latest = latest_buffer();
    latest.overflow_count++;
    latest.buffer_paused = true;
    particle_motion_halted = true;
    wind_on = false;
    latest.wind_active = false;
    set_render_state(UI_WAITING);
    emit_event(BUFFER_OVERFLOW, latest.overflow_count, "overflow_protection");
}

bool simulation_done() {
    LatestValueBuffer& latest = latest_buffer();
    return production_complete() &&
        !latest.wind_active &&
        active_particles == 0;
}

void simulation_tick() {
    LatestValueBuffer& latest = latest_buffer();
    scheduler_ticks++;

    if (particle_motion_halted && current_render_state() == UI_RUNNING) {
        set_render_state(UI_PAUSED);
    }

    if (latest.spawn_budget == 0 && !latest.wind_active && active_particles == 0) {
        latest.buffer_paused = true;
        if (current_render_state() != UI_START) {
            set_render_state(UI_WAITING);
        }
    }
}

void handle_event(const Event& event) {
    LatestValueBuffer& latest = latest_buffer();

    switch (event.type) {
    case CAMERA_OPEN:
        cout << "[EVENT] camera open\n";
        particle_motion_halted = false;
        latest.buffer_paused = false;
        set_render_state(UI_INPUT_ACTIVE);
        camera_signals_sent++;
        wakeup(cam);
        wakeup(render_ui);
        break;

    case CAMERA_DETECTED:
        cout << "[EVENT] camera detected\n";
        set_render_state(UI_INPUT_ACTIVE);
        wakeup(render_ui);
        break;

    case MOUTH_OPEN:
        cout << "[EVENT] mouth open\n";
        set_render_state(UI_INPUT_ACTIVE);
        wakeup(render_ui);
        wakeup(mic);
        break;

    case MIC_START: {
        cout << "[EVENT] mic start wind\n";
        latest.wind_active = true;
        latest.buffer_paused = false;
        wind_on = true;
        particle_motion_halted = false;
        mic_signals_sent++;
        set_render_state(UI_RUNNING);
        wakeup(mic);
        wakeup(render_ui);

        int spawn_count = compute_spawn_count_from_wind(latest.wind_strength);
        spawn_count = min(spawn_count, latest.seeds_left);

        if (spawn_count > 0) {
            if (!buffer_can_accept_spawn(spawn_count)) {
                handle_buffer_overflow_protection();
                break;
            }

            latest.spawn_budget += spawn_count;
            consume_center_seeds(spawn_count);
            emit_event(PARTICLE_SPAWN, spawn_count, "wind_spawn");
        }
        break;
    }

    case MIC_END:
        cout << "[EVENT] mic end wind\n";
        wind_on = false;
        latest.wind_active = false;
        if (latest.spawn_budget == 0) {
            set_render_state(UI_WAITING);
        }
        wakeup(render_ui);
        break;

    case PARTICLE_SPAWN:
        cout << "[EVENT] particle spawn +" << event.value << "\n";
        wakeup(render_ui);
        break;

    case PARTICLE_HIT_BORDER:
        cout << "[EVENT] particle hit border\n";
        if (active_thread_mode != THREAD_MODE_3) {
            set_render_state(UI_INPUT_ACTIVE);
            wakeup(cam);
        }
        wakeup(render_ui);
        break;

    case BUFFER_OVERFLOW:
        cout << "[EVENT] latest buffer overwrite\n";
        latest.buffer_paused = true;
        set_render_state(UI_WAITING);
        wakeup(render_ui);
        break;

    case BUFFER_EMPTY:
        cout << "[EVENT] buffer empty\n";
        latest.buffer_paused = true;
        set_render_state(UI_WAITING);
        if (active_thread_mode != THREAD_MODE_3) {
            wakeup(cam);
        }
        wakeup(render_ui);
        break;

    case RENDER_UPDATE:
        wakeup(render_ui);
        break;

    case UI_STOP:
        cout << "[EVENT] ui stop\n";
        particle_motion_halted = true;
        set_render_state(UI_PAUSED);
        wakeup(render_ui);
        break;

    case UI_CONTINUE:
        cout << "[EVENT] ui continue\n";
        particle_motion_halted = false;
        latest.buffer_paused = false;
        if (latest.wind_active || latest.spawn_budget > 0) {
            set_render_state(UI_RUNNING);
        } else {
            set_render_state(UI_INPUT_ACTIVE);
        }
        wakeup(render_ui);
        break;

    case UI_RESET:
        reset_simulation();
        break;

    case CAMERA_CLOSE:
        break;
    }
}

void update_particle(Thread* t) {
    LatestValueBuffer& latest = latest_buffer();

    if (wind_on) {
        t->vx += latest.wind_dir_x * (0.015f + latest.wind_strength * 0.08f);
        t->vy += latest.wind_dir_y * (0.010f + latest.wind_strength * 0.05f);
    } else {
        t->vx *= 0.985f;
        t->vy *= 0.985f;
        t->vy += 0.0015f;
    }

    t->x += t->vx;
    t->y += t->vy;

    cout << "Particle " << t->id << " moved to (" << t->x << ", " << t->y << ")\n";
}

void camera(Thread* t) {
    if (camera_bursts_done >= max_camera_bursts) {
        t->state = FINISHED;
        cout << "Camera finished all detections\n";
        return;
    }

    if (t->pc == 0) {
        cout << "Camera waiting...\n";
        if (current_render_state() != UI_RUNNING) {
            set_render_state(UI_CAMERA_LOADING);
        }
        t->pc = 1;
        block();
        return;
    }

    LatestValueBuffer& latest = latest_buffer();
    camera_bursts_done++;

    emit_event(CAMERA_DETECTED, 1, "camera_position");
    emit_event(RENDER_UPDATE, 0, "camera_update");

    if (latest.mouth_open) {
        emit_event(MOUTH_OPEN, 1, "mouth_open");
    }

    if (active_thread_mode != THREAD_MODE_3) {
        t->pc = 0;
        block();
        return;
    }

    if (camera_bursts_done >= max_camera_bursts) {
        t->state = FINISHED;
        cout << "Camera finished all detections\n";
        return;
    }

    t->pc = 0;
    yield();
}

void mic_thread(Thread* t) {
    LatestValueBuffer& latest = latest_buffer();

    if (t->pc == 0) {
        cout << "Mic waiting...\n";
        t->pc = 1;
        block();
        return;
    }

    if (latest.wind_active) {
        wind_on = true;
        set_render_state(UI_RUNNING);
        cout << "Mic ON -> wind strength " << latest.wind_strength << "\n";
        emit_event(RENDER_UPDATE, 1, "wind_on");
    } else {
        wind_on = false;
        cout << "Mic OFF -> wind stopped\n";
        if (active_thread_mode == THREAD_MODE_1) {
            latest.buffer_paused = false;
            set_render_state(UI_RUNNING);
        }
        emit_event(RENDER_UPDATE, 0, "wind_off");
    }

    t->pc = 0;
    yield();
}

void render_thread(Thread* t) {
    if (t->pc == 0) {
        cout << "[UI] " << render_state_name(current_render_state()) << "\n";
        t->pc = 1;
        block();
        return;
    }

    LatestValueBuffer& latest = latest_buffer();
    particle_snapshots++;
    cout << "[UI] state=" << render_state_name(current_render_state())
         << " wind=" << (latest.wind_active ? "ON" : "OFF")
         << " wind_strength=" << latest.wind_strength
         << " spawn_budget=" << latest.spawn_budget
         << " seeds_left=" << latest.seeds_left
         << " overflow=" << latest.overflow_count
         << " frame=" << particle_snapshots << "\n";

    t->pc = 0;
    block();
}

void particle(Thread* t) {
    LatestValueBuffer& latest = latest_buffer();

    if (t->life <= 0) {
        t->state = FINISHED;
        active_particles--;
        cout << "Particle " << t->id << " finished\n";
        return;
    }

    if (particle_motion_halted || latest.buffer_paused) {
        if (current_render_state() == UI_RUNNING) {
            set_render_state(UI_WAITING);
        }
        yield();
        return;
    }

    lock(&buffer_lock);
    if (t->state == BLOCKED) {
        return;
    }

    if (!t->active && latest.spawn_budget > 0) {
        latest.spawn_budget--;
        t->active = true;
        t->force = latest.wind_strength;
        t->dir_x = latest.wind_dir_x;
        t->dir_y = latest.wind_dir_y;
        unlock(&buffer_lock);
    } else {
        unlock(&buffer_lock);
    }

    if (!t->active) {
        if (camera_bursts_done >= max_camera_bursts && latest.spawn_budget == 0) {
            t->state = FINISHED;
            active_particles--;
            emit_event(BUFFER_EMPTY, 0, "particles_idle");
            cout << "Particle " << t->id << " finished\n";
            return;
        }

        yield();
        return;
    }

    update_particle(t);
    t->pc++;
    t->life--;
    emit_event(RENDER_UPDATE, t->id, "particle_step");
    cout << "Particle " << t->id << " RR step " << t->pc << "\n";

    if (t->x < -1.0f || t->x > 1.0f || t->y < -1.0f || t->y > 1.0f) {
        t->stopped_by_border = true;
        t->active = false;
        t->state = FINISHED;
        active_particles--;
        emit_event(PARTICLE_HIT_BORDER, t->id, "particle_border");
        cout << "Particle " << t->id << " hit border and stopped\n";
        return;
    }

    if (t->life <= 0) {
        t->state = FINISHED;
        active_particles--;
        cout << "Particle " << t->id << " finished\n";
        return;
    }

    yield();
}

void reset_simulation() {
    LatestValueBuffer& latest = latest_buffer();
    latest.spawn_budget = 0;
    latest.max_spawn_budget = 100;
    latest.overflow_count = 0;
    latest.mouth_open = false;
    latest.wind_active = false;
    latest.buffer_paused = false;
    latest.mouth_x = 0.5f;
    latest.mouth_y = 0.5f;
    latest.wind_strength = 0.0f;
    latest.wind_dir_x = 1.0f;
    latest.wind_dir_y = 0.0f;
    latest.seeds_total = config.initial_seed_count;
    latest.seeds_left = config.initial_seed_count;

    wind_on = false;
    camera_bursts_done = 0;
    particle_snapshots = 0;
    scheduler_ticks = 0;
    camera_signals_sent = 0;
    mic_signals_sent = 0;
    active_particles = 0;
    particle_motion_halted = false;
    set_render_state(UI_START);

    if (cam) {
        cam->state = READY;
        cam->pc = 0;
        cam->time_slice = 0;
        cam->started = false;
    }

    if (mic) {
        mic->state = READY;
        mic->pc = 0;
        mic->time_slice = 0;
        mic->started = false;
    }

    if (render_ui) {
        render_ui->state = READY;
        render_ui->pc = 0;
        render_ui->time_slice = 0;
        render_ui->started = false;
    }

    for (Thread* t : all_threads()) {
        if (t->name != "particle") {
            continue;
        }
        t->state = READY;
        t->pc = 0;
        t->started = false;
        t->time_slice = 0;
        t->x = 0.0f;
        t->y = 0.0f;
        t->vx = 0.0f;
        t->vy = 0.0f;
        t->dir_x = 1.0f;
        t->dir_y = 0.0f;
        t->force = 0.0f;
        t->size = 4.0f;
        t->life = 4;
        t->active = false;
        t->stopped_by_border = false;
        active_particles++;
    }

    cout << "[EVENT] ui reset\n";
    reset_scheduler_state();
}
