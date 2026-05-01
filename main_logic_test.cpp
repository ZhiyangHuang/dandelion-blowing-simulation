#include "thread.h"
#include <cassert>
#include <iostream>

int main() {
    std::cout.setf(std::ios::unitbuf);

    runtime_config().initial_seed_count = 100;
    rebuild_runtime_threads(THREAD_MODE_1);

    assert(current_thread_mode() == THREAD_MODE_1);
    assert(created_thread_count() == 1);
    assert(latest_buffer().seeds_total == 100);
    assert(latest_buffer().seeds_left == 100);

    handle_event({CAMERA_OPEN, 1, "test_camera"});
    assert(current_render_state() == UI_INPUT_ACTIVE);

    latest_buffer().wind_strength = 0.55f;
    int seeds_before = latest_buffer().seeds_left;
    handle_event({MIC_START, 1, "test_mic"});
    assert(latest_buffer().spawn_budget > 0);
    assert(latest_buffer().seeds_left < seeds_before);
    assert(wind_on);

    handle_event({MIC_END, 0, "test_mic_end"});
    assert(!wind_on);
    assert(current_render_state() == UI_WAITING || current_render_state() == UI_RUNNING);

    handle_event({BUFFER_EMPTY, 0, "test_particles_done"});
    assert(current_render_state() == UI_INPUT_ACTIVE || current_render_state() == UI_WAITING);

    rebuild_runtime_threads(THREAD_MODE_2);
    assert(current_thread_mode() == THREAD_MODE_2);
    assert(created_thread_count() == 2);
    handle_event({PARTICLE_HIT_BORDER, 1, "test_border"});
    assert(current_render_state() == UI_INPUT_ACTIVE || current_render_state() == UI_PAUSED);

    rebuild_runtime_threads(THREAD_MODE_3);
    assert(current_thread_mode() == THREAD_MODE_3);
    assert(created_thread_count() == 3);

    handle_event({UI_RESET, 0, "test_reset"});
    assert(latest_buffer().spawn_budget == 0);
    assert(latest_buffer().seeds_left == 100);

    std::cout << "logic test passed\n";
    return 0;
}
