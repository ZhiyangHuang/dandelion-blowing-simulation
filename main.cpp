#include "thread.h"

#include <iostream>
#include <vector>

int main() {
    bootstrap_runtime();
    seed_startup_flow();
    scheduler_tick();

    if (!init_visualization()) {
        std::cerr << "Failed to initialize visualization scaffold\n";
        return 1;
    }

    process_visual_input();
    render_visual_frame();
    shutdown_visualization();

    const SchedulerSnapshot snapshot = scheduler_snapshot();
    std::cout << "DandelionOS scaffold ready\n";
    std::cout << "thread_mode=" << snapshot.thread_mode
              << " remaining_particles=" << snapshot.remaining_particles
              << " p1=" << snapshot.p1_queue.size()
              << " p2=" << snapshot.p2_queue.size()
              << " p3=" << snapshot.p3_queue.size() << "\n";

    const std::vector<std::string> notes = drain_runtime_notes();
    for (const std::string& note : notes) {
        std::cout << note << "\n";
    }

    return 0;
}
