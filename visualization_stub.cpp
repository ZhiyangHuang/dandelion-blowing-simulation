#include "thread.h"

bool init_visualization() {
    set_visualization_running(true);
    return true;
}

void shutdown_visualization() {
    set_visualization_running(false);
}

void process_visual_input() {
    refresh_bridge_inputs();
}

void render_visual_frame() {
}

bool visualization_running() {
    return true;
}
