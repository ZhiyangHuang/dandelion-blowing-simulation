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
constexpr auto kCameraBridgeStartupTimeout = std::chrono::seconds(4);
const char* kCameraBridgePath = "camera_bridge_latest.json";
const char* kMicrophoneBridgePath = "microphone_bridge_latest.json";

#if defined(_WIN32)
struct ChildProcess {
    PROCESS_INFORMATION process_info{};
    bool running = false;
    std::string command;
    std::string error_text;
};
#else
struct ChildProcess {
    bool running = false;
    std::string command;
    std::string error_text;
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

void mark_camera_bridge_starting() {
    runtime_state().camera_bridge = CameraBridgeState{};
    runtime_state().camera_bridge.backend = "camera-bridge-starting";
    runtime_state().camera_bridge.status_text = "camera bridge starting";
    runtime_state().camera_bridge.bridge_connected = false;
    runtime_state().camera_bridge.sample_ready = false;
    runtime_state().camera_device_available = false;
    runtime_state().camera_available = false;
}

bool wait_for_camera_bridge_packet() {
    const auto deadline = std::chrono::steady_clock::now() + kCameraBridgeStartupTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        refresh_bridge_inputs();
        if (runtime_state().camera_bridge.timestamp_ms > 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

#if defined(_WIN32)
std::string narrow_ascii(const std::wstring& text) {
    std::string output;
    output.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch <= 127) {
            output.push_back(static_cast<char>(ch));
        } else {
            output.push_back('?');
        }
    }
    return output;
}

std::wstring quote_argument(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::wstring search_path_executable(const wchar_t* executable_name) {
    DWORD length = SearchPathW(nullptr, executable_name, nullptr, 0, nullptr, nullptr);
    if (length == 0) {
        return L"";
    }

    std::wstring buffer;
    buffer.resize(length);
    const DWORD written = SearchPathW(
        nullptr,
        executable_name,
        nullptr,
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    if (written == 0) {
        return L"";
    }

    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

bool launch_camera_bridge(ChildProcess& child) {
    struct LaunchCandidate {
        std::wstring executable_path;
        std::wstring command_line;
        std::string label;
    };

    const std::filesystem::path working_directory = std::filesystem::current_path();
    const std::filesystem::path script_path = working_directory / "python_mediapipe_bridge.py";

    std::vector<LaunchCandidate> candidates;
    const std::wstring python_exe = search_path_executable(L"python.exe");
    if (!python_exe.empty()) {
        candidates.push_back({
            python_exe,
            quote_argument(python_exe) + L" " + quote_argument(script_path.wstring()),
            "python.exe " + narrow_ascii(python_exe),
        });
    }

    const std::wstring py_exe = search_path_executable(L"py.exe");
    if (!py_exe.empty()) {
        candidates.push_back({
            py_exe,
            quote_argument(py_exe) + L" -3 " + quote_argument(script_path.wstring()),
            "py.exe " + narrow_ascii(py_exe),
        });
    }

    if (candidates.empty()) {
        child.error_text = "No python launcher found on PATH.";
        return false;
    }

    DWORD last_error = ERROR_FILE_NOT_FOUND;
    std::string last_label = "none";
    for (const LaunchCandidate& candidate : candidates) {
        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        PROCESS_INFORMATION process_info{};
        std::vector<wchar_t> command_line(
            candidate.command_line.begin(),
            candidate.command_line.end());
        command_line.push_back(L'\0');

        const BOOL started = CreateProcessW(
            candidate.executable_path.c_str(),
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
            last_error = GetLastError();
            last_label = candidate.label;
            continue;
        }

        child.process_info = process_info;
        child.running = true;
        child.command = candidate.label;
        child.error_text.clear();
        return true;
    }

    child.error_text =
        "CreateProcessW failed for " + last_label +
        " with Win32 error " + std::to_string(static_cast<unsigned long>(last_error));
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
    std::cout.setf(std::ios::unitbuf);
    bootstrap_runtime();
    seed_startup_flow();
    set_thread_mode(1);
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
    mark_camera_bridge_starting();

    ChildProcess camera_bridge_process;
    if (launch_camera_bridge(camera_bridge_process)) {
        std::cout << "Spawned camera bridge with: " << camera_bridge_process.command << "\n";
        push_runtime_note("Live launcher: camera bridge process started.");
        if (wait_for_camera_bridge_packet()) {
            push_runtime_note("Live launcher: first camera bridge packet received.");
        } else {
            runtime_state().camera_bridge.status_text = "camera bridge started, waiting for first packet";
            push_runtime_note("Live launcher: no camera packet within startup timeout.");
        }
    } else {
        std::cout << "Warning: failed to launch python camera bridge automatically.\n";
        std::cout << "You can still start it manually with: python python_mediapipe_bridge.py\n";
        if (!camera_bridge_process.error_text.empty()) {
            std::cout << "Launch detail: " << camera_bridge_process.error_text << "\n";
            push_runtime_note("Live launcher: " + camera_bridge_process.error_text);
        }
        runtime_state().camera_bridge.status_text = "camera bridge auto-start failed";
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
    drain_runtime_notes();
    return 0;
}
