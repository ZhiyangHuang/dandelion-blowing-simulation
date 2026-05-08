#include "thread.h"

#include <string>

namespace {

bool g_running = false;

}  // namespace

bool init_visualization() {
    g_running = true;
    set_visualization_running(true);
    push_runtime_note("Visualization scaffold initialized.");
    return true;
}

void shutdown_visualization() {
    g_running = false;
    set_visualization_running(false);
    push_runtime_note("Visualization scaffold shut down.");
}

void process_visual_input() {
    if (!g_running) {
        return;
    }
    push_runtime_note("Input layer placeholder: no SDL loop attached yet.");
}

void render_visual_frame() {
    if (!g_running) {
        return;
    }
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    push_runtime_note(
        "Render scaffold frame " + std::to_string(snapshot.frame_index) +
        " | thread mode " + std::to_string(snapshot.thread_mode));
}

bool visualization_running() {
    return g_running;
}
