#include "thread.h"

#include <cmath>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace {

RenderData g_render_data;
RuntimeState g_runtime_state;
std::mutex g_particle_mutex;
std::mutex g_power_mutex;
std::mutex g_render_mutex;
LockDebugState g_lock_debug_state;

void rebuild_particle_ring(int count) {
    g_render_data.particles.clear();
    g_render_data.particles.reserve(count);

    const float center_x = 0.5f;
    const float center_y = 0.5f;
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
        g_render_data.particles.push_back(particle);
    }
}

std::string build_lock_sequence(bool lock_particle, bool lock_power, bool lock_render) {
    std::string sequence;
    if (lock_particle) {
        sequence = "particle";
    }
    if (lock_power) {
        if (!sequence.empty()) {
            sequence += "->";
        }
        sequence += "power";
    }
    if (lock_render) {
        if (!sequence.empty()) {
            sequence += "->";
        }
        sequence += "render";
    }
    if (sequence.empty()) {
        sequence = "none";
    }
    return sequence;
}

}  // namespace

RenderData& render_data() {
    return g_render_data;
}

const RenderData& current_render_data() {
    return g_render_data;
}

RuntimeState& runtime_state() {
    return g_runtime_state;
}

const RuntimeState& current_runtime_state() {
    return g_runtime_state;
}

LockDebugState lock_debug_state() {
    return g_lock_debug_state;
}

void with_shared_state_write(bool lock_particle,
                             bool lock_power,
                             bool lock_render,
                             const std::function<void()>& fn) {
    std::unique_lock<std::mutex> particle_lock;
    std::unique_lock<std::mutex> power_lock;
    std::unique_lock<std::mutex> render_lock;

    if (lock_particle) {
        particle_lock = std::unique_lock<std::mutex>(g_particle_mutex);
        ++g_lock_debug_state.particle_lock_count;
    }
    if (lock_power) {
        power_lock = std::unique_lock<std::mutex>(g_power_mutex);
        ++g_lock_debug_state.power_lock_count;
    }
    if (lock_render) {
        render_lock = std::unique_lock<std::mutex>(g_render_mutex);
        ++g_lock_debug_state.render_lock_count;
    }

    g_lock_debug_state.last_lock_sequence =
        build_lock_sequence(lock_particle, lock_power, lock_render);
    if (lock_particle || lock_power) {
        g_lock_debug_state.last_ordered_write_sequence =
            g_lock_debug_state.last_lock_sequence;
    }
    fn();
}

void set_runtime_phase(RuntimePhase phase) {
    g_runtime_state.phase = phase;
    with_shared_state_write(false, false, true, [phase]() {
        g_render_data.ui.phase_label = runtime_phase_label(phase);
    });
}

const char* runtime_phase_label(RuntimePhase phase) {
    switch (phase) {
    case RuntimePhase::BOOTSTRAP:
        return "BOOTSTRAP";
    case RuntimePhase::STARTING:
        return "STARTING";
    case RuntimePhase::RESETTING:
        return "RESETTING";
    case RuntimePhase::READY:
        return "READY";
    case RuntimePhase::SHUTTING_DOWN:
        return "SHUTTING_DOWN";
    default:
        return "UNKNOWN";
    }
}

void request_shutdown() {
    g_runtime_state.shutdown_requested = true;
    set_runtime_phase(RuntimePhase::SHUTTING_DOWN);
}

bool shutdown_requested() {
    return g_runtime_state.shutdown_requested;
}

void reset_simulation_world() {
    with_shared_state_write(true, true, true, []() {
        g_runtime_state.world.dandelion_x = 0.5f;
        g_runtime_state.world.dandelion_y = 0.5f;
        g_runtime_state.world.mouth_x = g_runtime_state.world.dandelion_x;
        g_runtime_state.world.mouth_y = g_runtime_state.world.dandelion_y;
        g_runtime_state.world.remaining_particles = 100;
        g_runtime_state.world.queued_particle_tasks = 0;
        g_runtime_state.world.power = 0.1f;
        g_runtime_state.camera_device_available = false;
        g_runtime_state.microphone_device_available = false;
        g_runtime_state.camera_available = false;
        g_runtime_state.microphone_available = false;
        g_runtime_state.camera_focus_locked = true;
        g_runtime_state.microphone_focus_locked = false;
        g_runtime_state.microphone_focus_consumed_for_gate = false;
        g_runtime_state.camera_gate_open = false;
        g_runtime_state.camera_gate_frame = -1;
        g_runtime_state.camera_gate_until_ms = 0;
        g_runtime_state.microphone_focus_until_ms = 0;
        g_runtime_state.last_consumed_microphone_sample_ms = 0;
        g_runtime_state.human_behavior_flow = OrchestrationRecord{"HumanBehaviorTask"};
        g_runtime_state.particle_root_flow = OrchestrationRecord{"ParticleRootTask"};
        g_runtime_state.camera_listener = CameraListenerState{};
        g_runtime_state.microphone_listener = MicrophoneListenerState{};
        g_runtime_state.camera_bridge = CameraBridgeState{};
        g_runtime_state.microphone_bridge = MicrophoneBridgeState{};

        g_render_data.sky_grass_layer.theme = "meadow";
        g_render_data.camera_layer.enabled = false;
        g_render_data.camera_layer.mouth_detected = false;
        g_render_data.camera_layer.mouth_x = g_runtime_state.world.mouth_x;
        g_render_data.camera_layer.mouth_y = g_runtime_state.world.mouth_y;
        g_render_data.camera_layer.update_tick = 0;
        g_render_data.camera_layer.status = "offline";
        g_render_data.wind_layer.active = false;
        g_render_data.wind_layer.power = g_runtime_state.world.power;
        g_render_data.wind_layer.sample_tick = 0;
        g_render_data.wind_layer.tick_count = 0;
        g_render_data.wind_layer.status = "idle";
        g_render_data.ui.banner = "DandelionOS Runtime Scaffold";
        g_render_data.ui.scheduler_state = "Idle";
        g_render_data.ui.phase_label = runtime_phase_label(g_runtime_state.phase);
        g_render_data.ui.input_focus_status = "FREE FOCUS";
        g_render_data.ui.camera_task_status = "offline";
        g_render_data.ui.microphone_task_status = "offline";
        g_render_data.ui.generate_task_status = "idle";
        g_render_data.ui.batch_task_status = "idle";
        g_render_data.ui.particle_task_status = "idle";
        g_render_data.ui.thread_mode = current_thread_mode();
        g_render_data.ui.remaining_particles = g_runtime_state.world.remaining_particles;
        g_render_data.ui.queued_particle_tasks = 0;
        g_render_data.ui.power = g_runtime_state.world.power;
        rebuild_particle_ring(g_render_data.ui.remaining_particles);
    });
}
