#include "thread.h"

#include <cassert>
#include <iostream>

int main() {
    bootstrap_runtime();
    seed_startup_flow();

    if (!init_visualization()) {
        std::cerr << "runtime smoke test failed: visualization scaffold init failed\n";
        return 1;
    }

    process_visual_input();
    scheduler_tick();
    render_visual_frame();

    SchedulerSnapshot snapshot = scheduler_snapshot();
    assert(snapshot.visualization_enabled);
    assert(snapshot.thread_mode == 1);
    assert(snapshot.remaining_particles == 100);

    shutdown_visualization();
    assert(!visualization_running());

    std::cout << "runtime smoke test passed\n";
    return 0;
}
