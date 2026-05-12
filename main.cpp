#include "thread.h"

#include <chrono>
#include <iostream>
#include <thread>

namespace {

constexpr auto kLoopDelay = std::chrono::milliseconds(33);

}  // namespace

int main() {
    std::cout.setf(std::ios::unitbuf);
    bootstrap_runtime();

    if (!init_visualization()) {
        std::cerr << "Failed to initialize runtime visualization state\n";
        return 1;
    }

    seed_startup_flow();
    set_thread_mode(1);
    scheduler_tick();
    scheduler_tick();

    std::cout << "DandelionOS live runtime starting\n";
    std::cout << "Controls in SDL window: q/esc quit | r reset | c change dandelion | 1/2/3 thread mode | x force breeze\n";
    std::cout << "Live mode uses python MediaPipe camera bridge, native C++ microphone capture, and SDL feedback.\n";

    auto last_render = std::chrono::steady_clock::now();

    while (!shutdown_requested() && visualization_running()) {
        // Keep SDL input and bridge refresh hot so reset/quit controls stay responsive.
        process_visual_input();

        // Advance scheduler state continuously while the runtime is alive.
        scheduler_tick();

        // Render at a stable cadence while control and scheduling continue every loop.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_render >= kLoopDelay) {
            render_visual_frame();
            last_render = now;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    shutdown_visualization();

    std::cout << "\nRuntime shut down.\n";
    drain_runtime_notes();
    return 0;
}
