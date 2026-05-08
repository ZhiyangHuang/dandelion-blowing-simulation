#include "thread.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

RenderData g_render_data;

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
        particle.attached = true;
        particle.active = false;
        g_render_data.particles.push_back(particle);
    }
}

}  // namespace

RenderData& render_data() {
    return g_render_data;
}

const RenderData& current_render_data() {
    return g_render_data;
}

void reset_simulation_world() {
    g_render_data.sky_grass_layer.theme = "meadow";
    g_render_data.camera_layer.enabled = false;
    g_render_data.camera_layer.mouth_detected = false;
    g_render_data.camera_layer.mouth_x = 0.5f;
    g_render_data.camera_layer.mouth_y = 0.5f;
    g_render_data.wind_layer.active = false;
    g_render_data.wind_layer.power = 0.1f;
    g_render_data.ui.banner = "DandelionOS Scaffold";
    g_render_data.ui.scheduler_state = "Idle";
    g_render_data.ui.thread_mode = current_thread_mode();
    g_render_data.ui.remaining_particles = 100;
    g_render_data.ui.power = 0.1f;
    rebuild_particle_ring(g_render_data.ui.remaining_particles);
}
