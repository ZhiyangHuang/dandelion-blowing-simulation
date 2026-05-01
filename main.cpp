#include "thread.h"
#include <iostream>

int main() {
    std::cout.setf(std::ios::unitbuf);
    set_thread_mode(THREAD_MODE_1);

    if (!init_visualization()) {
        std::cerr << "Failed to initialize SDL visualization\n";
        return 1;
    }

    rebuild_runtime_threads(THREAD_MODE_1);

    event_loop();
    shutdown_visualization();

    return 0;
}
