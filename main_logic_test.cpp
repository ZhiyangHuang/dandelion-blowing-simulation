#include "thread.h"

#include <cassert>
#include <iostream>

int main() {
    bootstrap_runtime();

    assert(current_thread_mode() == 1);
    assert(runtime_threads().size() == 3);
    assert(current_render_data().particles.size() == 100);
    assert(current_render_data().ui.remaining_particles == 100);

    seed_startup_flow();
    SchedulerSnapshot before_tick = scheduler_snapshot();
    assert(before_tick.p1_queue.size() == 2);
    assert(before_tick.p2_queue.size() == 2);
    assert(before_tick.p3_queue.empty());

    scheduler_tick();
    SchedulerSnapshot after_tick = scheduler_snapshot();
    assert(after_tick.frame_index == 1);
    assert(after_tick.p1_queue.size() == 1);

    set_thread_mode(3);
    SchedulerSnapshot threaded = scheduler_snapshot();
    assert(threaded.thread_mode == 3);
    assert(threaded.threads[0].state == ThreadState::IDLE);
    assert(threaded.threads[2].state == ThreadState::IDLE);

    reset_runtime();
    SchedulerSnapshot reset = scheduler_snapshot();
    assert(reset.frame_index == 0);
    assert(reset.p1_queue.empty());
    assert(reset.remaining_particles == 100);
    assert(current_render_data().ui.banner == "DandelionOS Scaffold");

    std::cout << "logic test passed\n";
    return 0;
}
