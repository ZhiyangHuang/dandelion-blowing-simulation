#include "thread.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

constexpr auto kLoopDelay = std::chrono::milliseconds(33);
const char* kCameraBridgePath = "camera_bridge_latest.json";
const char* kMicrophoneBridgePath = "microphone_bridge_latest.json";

#if defined(_WIN32)
struct ChildProcess {
    PROCESS_INFORMATION process_info{};
    bool running = false;
    std::string command;
};
#else
struct ChildProcess {
    bool running = false;
    std::string command;
};
#endif

void remove_bridge_override_file(const std::filesystem::path& path,
                                 const std::string& label) {
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (removed) {
        std::cout << "Removed stale " << label << ": " << path.filename().string() << "\n";
    } else if (ec) {
        std::cout << "Warning: failed to remove " << label << " "
                  << path.filename().string() << ": " << ec.message() << "\n";
    }
}

#if defined(_WIN32)
bool launch_camera_bridge(ChildProcess& child) {
    const std::vector<std::string> commands = {
        "python python_mediapipe_bridge.py",
        "py -3 python_mediapipe_bridge.py",
    };
    const std::string working_directory = std::filesystem::current_path().string();

    for (const std::string& command : commands) {
        STARTUPINFOA startup_info{};
        startup_info.cb = sizeof(startup_info);
        PROCESS_INFORMATION process_info{};
        std::vector<char> command_line(command.begin(), command.end());
        command_line.push_back('\0');

        const BOOL started = CreateProcessA(
            nullptr,
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            nullptr,
            working_directory.c_str(),
            &startup_info,
            &process_info);
        if (!started) {
            continue;
        }

        child.process_info = process_info;
        child.running = true;
        child.command = command;
        return true;
    }

    return false;
}

void stop_child_process(ChildProcess& child) {
    if (!child.running) {
        return;
    }

    TerminateProcess(child.process_info.hProcess, 0);
    WaitForSingleObject(child.process_info.hProcess, 1000);
    CloseHandle(child.process_info.hThread);
    CloseHandle(child.process_info.hProcess);
    child.process_info = PROCESS_INFORMATION{};
    child.running = false;
}

#else
bool launch_camera_bridge(ChildProcess&) {
    return false;
}

void stop_child_process(ChildProcess&) {}
#endif

}  // namespace

int main() {
    bootstrap_runtime();
    seed_startup_flow();
    set_thread_mode(3);
    scheduler_tick();
    scheduler_tick();

    if (!init_visualization()) {
        std::cerr << "Failed to initialize runtime visualization state\n";
        return 1;
    }

    std::cout << "DandelionOS live runtime starting\n";
    std::cout << "Controls in SDL window: q/esc quit | r reset | c change dandelion | 1/2/3 thread mode | x force breeze\n";
    std::cout << "Live mode uses python MediaPipe camera bridge, native C++ microphone capture, and SDL feedback.\n";

    remove_bridge_override_file(kMicrophoneBridgePath, "offline microphone override");
    remove_bridge_override_file(kCameraBridgePath, "stale camera bridge packet");

    ChildProcess camera_bridge_process;
    if (launch_camera_bridge(camera_bridge_process)) {
        std::cout << "Spawned camera bridge with: " << camera_bridge_process.command << "\n";
        push_runtime_note("Live launcher: camera bridge process started.");
    } else {
        std::cout << "Warning: failed to launch python camera bridge automatically.\n";
        std::cout << "You can still start it manually with: python python_mediapipe_bridge.py\n";
        push_runtime_note("Live launcher: camera bridge auto-start failed.");
    }

    while (!shutdown_requested() && visualization_running()) {
        process_visual_input();
        scheduler_tick();
        render_visual_frame();
        std::this_thread::sleep_for(kLoopDelay);
    }

    stop_child_process(camera_bridge_process);
    shutdown_visualization();

    std::cout << "\nRuntime shut down.\n";
    const std::vector<std::string> final_notes = drain_runtime_notes();
    for (const std::string& note : final_notes) {
        std::cout << "  - " << note << "\n";
    }
    return 0;
}
