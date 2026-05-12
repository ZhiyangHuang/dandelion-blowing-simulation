#include "thread.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr auto kLoopDelay = std::chrono::milliseconds(33);

struct StartupOptions {
    int thread_mode = 3;
    bool demo_camera = false;
    bool demo_io_loop = false;
    long long auto_exit_ms = 0;
    std::string log_file_path;
};

void print_usage(const char* exe_name) {
    std::cout
        << "Usage: " << exe_name << " [--single|--dual|--triple] [--demo-camera] [--demo-io-loop]\n"
        << "  --single  deterministic sequential runtime\n"
        << "  --dual    camera-prioritized constrained parallelism\n"
        << "  --triple  fully partitioned listener + throughput runtime\n"
        << "  --demo-camera  use stable manual camera demo source instead of live camera bridge\n"
        << "  --demo-io-loop  use auto-looping simulated camera+microphone I/O sources\n"
        << "  --auto-exit-ms N  exit automatically after N milliseconds\n"
        << "  --log-file PATH  write runtime log lines to PATH\n";
}

bool parse_positive_long_long(const std::string& text, long long& value) {
    std::istringstream in(text);
    long long parsed = 0;
    in >> parsed;
    if (!in || !in.eof() || parsed < 0) {
        return false;
    }
    value = parsed;
    return true;
}

int parse_startup_options(int argc, char** argv, StartupOptions& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--single") {
            options.thread_mode = 1;
        } else if (arg == "--dual") {
            options.thread_mode = 2;
        } else if (arg == "--triple") {
            options.thread_mode = 3;
        } else if (arg == "--demo-camera") {
            options.demo_camera = true;
        } else if (arg == "--demo-io-loop") {
            options.demo_io_loop = true;
            options.demo_camera = true;
        } else if (arg == "--auto-exit-ms") {
            if (index + 1 >= argc) {
                std::cerr << "Missing value after --auto-exit-ms\n";
                print_usage(argv[0]);
                return -1;
            }
            long long parsed = 0;
            if (!parse_positive_long_long(argv[index + 1], parsed)) {
                std::cerr << "Invalid --auto-exit-ms value: " << argv[index + 1] << "\n";
                print_usage(argv[0]);
                return -1;
            }
            options.auto_exit_ms = parsed;
            ++index;
        } else if (arg == "--log-file") {
            if (index + 1 >= argc) {
                std::cerr << "Missing value after --log-file\n";
                print_usage(argv[0]);
                return -1;
            }
            options.log_file_path = argv[index + 1];
            ++index;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return -1;
        }
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    StartupOptions options;
    const int parse_status = parse_startup_options(argc, argv, options);
    if (parse_status <= 0) {
        return parse_status < 0 ? 1 : 0;
    }
    if (!options.log_file_path.empty() &&
        !set_runtime_log_file(options.log_file_path)) {
        std::cerr << "Failed to open log file: " << options.log_file_path << "\n";
        return 1;
    }

    bootstrap_runtime();
    set_camera_demo_fallback_enabled(options.demo_camera);
    set_demo_io_loop_enabled(options.demo_io_loop);
    set_thread_mode(options.thread_mode);

    if (!init_visualization()) {
        std::cerr << "Failed to initialize runtime visualization state\n";
        return 1;
    }

    seed_startup_flow();
    scheduler_tick();
    scheduler_tick();

    std::cout << "DandelionOS live runtime starting\n";
    std::cout << "Startup mode locked at "
              << (options.thread_mode == 1 ? "single" : (options.thread_mode == 2 ? "dual" : "triple"))
              << ".\n";
    if (options.demo_camera) {
        std::cout << "Camera source locked at demo fallback. Press O in SDL to inject mouth-open.\n";
    }
    if (options.demo_io_loop) {
        std::cout << "Demo I/O loop enabled. Simulated camera and microphone samples will cycle automatically.\n";
        push_runtime_note("Startup: demo I/O loop enabled.");
    }
    if (options.auto_exit_ms > 0) {
        std::cout << "Auto exit set to " << options.auto_exit_ms << " ms.\n";
        push_runtime_note(
            "Startup: auto exit after " + std::to_string(options.auto_exit_ms) + " ms.");
    }
    if (!options.log_file_path.empty()) {
        std::cout << "Runtime log file: " << options.log_file_path << "\n";
        push_runtime_note("Startup: runtime log file -> " + options.log_file_path);
    }
    std::cout << "Controls in SDL window: q/esc quit | r reset | c change dandelion | x force breeze | o demo mouth-open\n";
    std::cout << "Live mode uses python MediaPipe camera bridge, native C++ microphone capture, and SDL feedback.\n";

    auto last_render = std::chrono::steady_clock::now();
    const auto started_at = std::chrono::steady_clock::now();

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

        if (options.auto_exit_ms > 0) {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - started_at);
            if (elapsed_ms.count() >= options.auto_exit_ms) {
                push_runtime_note(
                    "Main: auto-exit timer reached " +
                    std::to_string(options.auto_exit_ms) + " ms.");
                request_shutdown();
                set_visualization_running(false);
            }
        }
    }

    shutdown_visualization();

    std::cout << "\nRuntime shut down.\n";
    drain_runtime_notes();
    close_runtime_log_file();
    return 0;
}
