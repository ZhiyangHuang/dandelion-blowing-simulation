#include "thread.h"
#include <cassert>
#include <iostream>

int main() {
    std::cout.setf(std::ios::unitbuf);
    set_thread_mode(THREAD_MODE_1);
    runtime_config().initial_seed_count = 100;

    if (!init_visualization()) {
        std::cerr << "runtime smoke test failed: visualization init failed\n";
        return 1;
    }

    rebuild_runtime_threads(THREAD_MODE_1);
    process_visual_input();
    render_visual_frame();

    assert(current_thread_mode() == THREAD_MODE_1);
    assert(created_thread_count() == 1);
    assert(latest_buffer().seeds_total == 100);
    assert(latest_buffer().max_spawn_budget == 100);

    shutdown_visualization();
    std::cout << "runtime smoke test passed\n";
    return 0;
}
