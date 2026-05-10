#include "thread.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

const char* kCameraBridgePath = "camera_bridge_latest.json";
const char* kCameraControlPath = "camera_bridge_control.json";
const char* kMicrophoneBridgePath = "microphone_bridge_latest.json";

void load_camera_bridge_from_json(const std::string& payload);

CameraBridgeState camera_bridge_disabled_state(const std::string& status_text) {
    CameraBridgeState state;
    state.bridge_connected = false;
    state.sample_ready = false;
    state.device_unavailable = false;
    state.face_detected = false;
    state.mouth_open_state = false;
    state.looking_forward = false;
    state.timestamp_ms = 0;
    state.backend = "camera-bridge-disabled";
    state.status_text = status_text;
    return state;
}

MicrophoneBridgeState microphone_bridge_disabled_state(const std::string& backend) {
    MicrophoneBridgeState state;
    state.bridge_connected = false;
    state.sample_ready = false;
    state.device_unavailable = false;
    state.voice_detected = false;
    state.fallback_requested = false;
    state.suggested_power = 0.1f;
    state.direction_x = 0.0f;
    state.direction_y = -1.0f;
    state.confidence = 0.0f;
    state.timestamp_ms = 0;
    state.backend = backend;
    state.status_text = backend;
    return state;
}

void remove_file_if_exists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

std::string read_text_file_if_exists(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return {};
    }

    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::size_t skip_json_ws(const std::string& text, std::size_t index) {
    while (index < text.size() &&
           std::isspace(static_cast<unsigned char>(text[index])) != 0) {
        ++index;
    }
    return index;
}

#if defined(_WIN32)

template <typename Fn>
Fn load_winmm_symbol(HMODULE library, const char* name) {
    FARPROC raw = GetProcAddress(library, name);
    Fn fn = nullptr;
    static_assert(sizeof(fn) == sizeof(raw), "unexpected function pointer size");
    std::memcpy(&fn, &raw, sizeof(fn));
    return fn;
}

struct ChildProcess {
    PROCESS_INFORMATION process_info{};
    bool running = false;
    std::string command;
    std::string error_text;
};

std::string narrow_ascii(const std::wstring& text) {
    std::string output;
    output.reserve(text.size());
    for (wchar_t ch : text) {
        output.push_back(ch <= 127 ? static_cast<char>(ch) : '?');
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

bool launch_camera_bridge_process(ChildProcess& child) {
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

bool child_process_alive(ChildProcess& child) {
    if (!child.running) {
        return false;
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(child.process_info.hProcess, &exit_code)) {
        stop_child_process(child);
        return false;
    }
    if (exit_code == STILL_ACTIVE) {
        return true;
    }

    CloseHandle(child.process_info.hThread);
    CloseHandle(child.process_info.hProcess);
    child.process_info = PROCESS_INFORMATION{};
    child.running = false;
    child.error_text =
        "python bridge exited with code " +
        std::to_string(static_cast<unsigned long>(exit_code));
    return false;
}

class CameraBridgeController {
public:
    void start() {
        enabled_ = true;
        startup_started_at_ms_ = now_ms();
        remove_file_if_exists(kCameraBridgePath);
        write_command("run");
        ensure_process();
    }

    void stop() {
        enabled_ = false;
        startup_started_at_ms_ = 0;
        write_command("stop");
        stop_child_process(process_);
        remove_file_if_exists(kCameraBridgePath);
    }

    bool enabled() const {
        return enabled_;
    }

    bool refresh(CameraBridgeState& out_state) {
        if (!enabled_) {
            out_state = camera_bridge_disabled_state("camera bridge disabled");
            return true;
        }

        if (!child_process_alive(process_)) {
            ensure_process();
        }

        const std::string payload = read_text_file_if_exists(kCameraBridgePath);

        if (!payload.empty()) {
            load_camera_bridge_from_json(payload);
            out_state = runtime_state().camera_bridge;
            return true;
        }

        out_state = camera_bridge_disabled_state(starting_status_text());
        out_state.device_unavailable = startup_timed_out() && !process_.running;
        if (out_state.device_unavailable && process_.error_text.empty()) {
            out_state.status_text = "camera device unavailable";
        }
        return true;
    }

private:
    static long long now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    void ensure_process() {
        if (process_.running) {
            return;
        }

        if (launch_camera_bridge_process(process_)) {
            startup_started_at_ms_ = now_ms();
            push_runtime_note("BridgeIO: camera bridge process started with " + process_.command);
        } else if (!process_.error_text.empty()) {
            push_runtime_note("BridgeIO: " + process_.error_text);
        }
    }

    bool startup_timed_out() const {
        return startup_started_at_ms_ > 0 &&
            (now_ms() - startup_started_at_ms_) >= 3000;
    }

    std::string starting_status_text() const {
        if (process_.running) {
            return startup_timed_out()
                ? "camera bridge waiting for device"
                : "camera bridge starting";
        }
        if (!process_.error_text.empty()) {
            return process_.error_text;
        }
        return startup_timed_out()
            ? "camera device unavailable"
            : "camera bridge starting";
    }

    void write_command(const std::string& cmd) {
        std::ofstream out(kCameraControlPath, std::ios::binary);
        out << "{ \"command\": \"" << cmd << "\" }";
    }

    bool enabled_ = false;
    long long startup_started_at_ms_ = 0;
    ChildProcess process_{};
};

class NativeMicrophoneBridge {
public:
    NativeMicrophoneBridge() = default;

    void start() {
        ensure_started();
    }

    void stop() {
        shutdown();
    }

    bool refresh(MicrophoneBridgeState& out_state) {
        if (!ensure_started()) {
            out_state = unavailable_state("native-cpp-wavein-unavailable");
            return false;
        }

        bool consumed_any_buffer = false;
        for (BufferSlot& slot : buffers_) {
            if ((slot.header.dwFlags & WHDR_DONE) == 0) {
                continue;
            }

            consumed_any_buffer = true;
            analyze_samples(slot.samples, out_state);
            slot.header.dwBytesRecorded = 0;
            slot.header.dwFlags &= ~WHDR_DONE;
            add_buffer(slot);
        }

        if (!consumed_any_buffer) {
            out_state = last_state_;
            out_state.bridge_connected = true;
            out_state.backend = "native-cpp-wavein";
            out_state.status_text = "microphone ready";
        }

        return true;
    }

    ~NativeMicrophoneBridge() {
        shutdown();
    }

private:
    struct BufferSlot {
        WAVEHDR header{};
        std::vector<short> samples;
    };

    bool ensure_started() {
        if (started_) {
            return true;
        }

        device_ = waveInOpen_dynamic();
        if (device_ == nullptr) {
            return false;
        }

        for (BufferSlot& slot : buffers_) {
            slot.samples.assign(kSamplesPerBuffer, 0);
            slot.header = {};
            slot.header.lpData = reinterpret_cast<LPSTR>(slot.samples.data());
            slot.header.dwBufferLength =
                static_cast<DWORD>(slot.samples.size() * sizeof(short));
            if (!prepare_buffer(slot) || !add_buffer(slot)) {
                shutdown();
                return false;
            }
        }

        if (!start_capture()) {
            shutdown();
            return false;
        }

        started_ = true;
        last_state_ = unavailable_state("native-cpp-wavein");
        last_state_.bridge_connected = true;
        last_state_.sample_ready = false;
        last_state_.device_unavailable = false;
        last_state_.status_text = "microphone ready, waiting for sample";
        return true;
    }

    MicrophoneBridgeState unavailable_state(const std::string& backend) const {
        MicrophoneBridgeState state;
        state.backend = backend;
        state.bridge_connected = false;
        state.sample_ready = false;
        state.device_unavailable = true;
        state.voice_detected = false;
        state.fallback_requested = false;
        state.suggested_power = 0.1f;
        state.status_text = "microphone device unavailable";
        return state;
    }

    void analyze_samples(const std::vector<short>& samples, MicrophoneBridgeState& out_state) {
        if (samples.empty()) {
            out_state = last_state_;
            return;
        }

        double energy = 0.0;
        int zero_crossings = 0;
        int prev_sign = 0;
        for (short sample : samples) {
            const double normalized = static_cast<double>(sample) / 32768.0;
            energy += normalized * normalized;

            const int sign = sample > 0 ? 1 : (sample < 0 ? -1 : 0);
            if (sign != 0 && prev_sign != 0 && sign != prev_sign) {
                zero_crossings++;
            }
            if (sign != 0) {
                prev_sign = sign;
            }
        }

        const double rms = std::sqrt(energy / static_cast<double>(samples.size()));
        const double zcr =
            static_cast<double>(zero_crossings) / static_cast<double>(samples.size());

        out_state = MicrophoneBridgeState{};
        out_state.bridge_connected = true;
        out_state.sample_ready = true;
        out_state.device_unavailable = false;
        out_state.backend = "native-cpp-wavein";
        out_state.confidence = static_cast<float>(std::clamp(rms * 6.0, 0.0, 1.0));
        out_state.direction_x = 0.0f;
        out_state.direction_y = -1.0f;
        out_state.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();

        const bool silence = rms < 0.01;
        const bool breath_like = !silence && zcr > 0.18;
        const bool voiced = !silence && !breath_like;

        out_state.voice_detected = voiced;
        out_state.fallback_requested = breath_like;
        out_state.suggested_power =
            std::clamp(static_cast<float>(0.1 + rms * 14.0), 0.1f, 10.0f);
        out_state.status_text = voiced
            ? "microphone sample ready, voice detected"
            : (breath_like ? "microphone sample ready, fallback requested"
                           : "microphone sample ready, no voice");

        last_state_ = out_state;
    }

    bool prepare_buffer(BufferSlot& slot) {
        return waveInPrepareHeader_ptr_(
                   device_,
                   &slot.header,
                   static_cast<UINT>(sizeof(WAVEHDR))) == MMSYSERR_NOERROR;
    }

    bool add_buffer(BufferSlot& slot) {
        return waveInAddBuffer_ptr_(
                   device_,
                   &slot.header,
                   static_cast<UINT>(sizeof(WAVEHDR))) == MMSYSERR_NOERROR;
    }

    bool start_capture() {
        return waveInStart_ptr_(device_) == MMSYSERR_NOERROR;
    }

    HWAVEIN waveInOpen_dynamic() {
        if (!load_winmm()) {
            return nullptr;
        }

        HWAVEIN device = nullptr;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign =
            static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        const MMRESULT result = waveInOpen_ptr_(
            &device,
            WAVE_MAPPER,
            &format,
            0,
            0,
            CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            return nullptr;
        }
        return device;
    }

    bool load_winmm() {
        if (winmm_loaded_) {
            return true;
        }

        library_ = LoadLibraryA("winmm.dll");
        if (library_ == nullptr) {
            return false;
        }

        waveInOpen_ptr_ = load_winmm_symbol<WaveInOpenFn>(library_, "waveInOpen");
        waveInPrepareHeader_ptr_ =
            load_winmm_symbol<WaveInPrepareHeaderFn>(library_, "waveInPrepareHeader");
        waveInUnprepareHeader_ptr_ =
            load_winmm_symbol<WaveInUnprepareHeaderFn>(library_, "waveInUnprepareHeader");
        waveInAddBuffer_ptr_ =
            load_winmm_symbol<WaveInAddBufferFn>(library_, "waveInAddBuffer");
        waveInStart_ptr_ = load_winmm_symbol<WaveInStartFn>(library_, "waveInStart");
        waveInStop_ptr_ = load_winmm_symbol<WaveInStopFn>(library_, "waveInStop");
        waveInReset_ptr_ = load_winmm_symbol<WaveInResetFn>(library_, "waveInReset");
        waveInClose_ptr_ = load_winmm_symbol<WaveInCloseFn>(library_, "waveInClose");

        winmm_loaded_ =
            waveInOpen_ptr_ != nullptr &&
            waveInPrepareHeader_ptr_ != nullptr &&
            waveInUnprepareHeader_ptr_ != nullptr &&
            waveInAddBuffer_ptr_ != nullptr &&
            waveInStart_ptr_ != nullptr &&
            waveInStop_ptr_ != nullptr &&
            waveInReset_ptr_ != nullptr &&
            waveInClose_ptr_ != nullptr;
        return winmm_loaded_;
    }

    void shutdown() {
        if (device_ != nullptr) {
            waveInStop_ptr_(device_);
            waveInReset_ptr_(device_);
            for (BufferSlot& slot : buffers_) {
                if (slot.header.lpData != nullptr) {
                    waveInUnprepareHeader_ptr_(
                        device_,
                        &slot.header,
                        static_cast<UINT>(sizeof(WAVEHDR)));
                }
            }
            waveInClose_ptr_(device_);
            device_ = nullptr;
        }

        if (library_ != nullptr) {
            FreeLibrary(library_);
            library_ = nullptr;
        }
        started_ = false;
        winmm_loaded_ = false;
    }

    using WaveInOpenFn =
        MMRESULT (WINAPI*)(LPHWAVEIN, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
    using WaveInPrepareHeaderFn = MMRESULT (WINAPI*)(HWAVEIN, LPWAVEHDR, UINT);
    using WaveInUnprepareHeaderFn = MMRESULT (WINAPI*)(HWAVEIN, LPWAVEHDR, UINT);
    using WaveInAddBufferFn = MMRESULT (WINAPI*)(HWAVEIN, LPWAVEHDR, UINT);
    using WaveInStartFn = MMRESULT (WINAPI*)(HWAVEIN);
    using WaveInStopFn = MMRESULT (WINAPI*)(HWAVEIN);
    using WaveInResetFn = MMRESULT (WINAPI*)(HWAVEIN);
    using WaveInCloseFn = MMRESULT (WINAPI*)(HWAVEIN);

    static constexpr int kSampleRate = 16000;
    static constexpr int kSamplesPerBuffer = 2048;

    HMODULE library_ = nullptr;
    HWAVEIN device_ = nullptr;
    bool started_ = false;
    bool winmm_loaded_ = false;
    WaveInOpenFn waveInOpen_ptr_ = nullptr;
    WaveInPrepareHeaderFn waveInPrepareHeader_ptr_ = nullptr;
    WaveInUnprepareHeaderFn waveInUnprepareHeader_ptr_ = nullptr;
    WaveInAddBufferFn waveInAddBuffer_ptr_ = nullptr;
    WaveInStartFn waveInStart_ptr_ = nullptr;
    WaveInStopFn waveInStop_ptr_ = nullptr;
    WaveInResetFn waveInReset_ptr_ = nullptr;
    WaveInCloseFn waveInClose_ptr_ = nullptr;
    BufferSlot buffers_[2];
    MicrophoneBridgeState last_state_{};
};

NativeMicrophoneBridge& native_microphone_bridge() {
    static NativeMicrophoneBridge bridge;
    return bridge;
}

#endif

bool find_json_value_range(const std::string& text,
                           const std::string& key,
                           std::size_t& value_begin,
                           std::size_t& value_end) {
    const std::string quoted_key = "\"" + key + "\"";
    const std::size_t key_pos = text.find(quoted_key);
    if (key_pos == std::string::npos) {
        return false;
    }

    std::size_t colon_pos = text.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return false;
    }

    value_begin = skip_json_ws(text, colon_pos + 1);
    if (value_begin >= text.size()) {
        return false;
    }

    if (text[value_begin] == '"') {
        value_end = value_begin + 1;
        while (value_end < text.size()) {
            if (text[value_end] == '"' && text[value_end - 1] != '\\') {
                ++value_end;
                return true;
            }
            ++value_end;
        }
        return false;
    }

    value_end = value_begin;
    while (value_end < text.size() &&
           text[value_end] != ',' &&
           text[value_end] != '}' &&
           text[value_end] != '\n' &&
           text[value_end] != '\r') {
        ++value_end;
    }
    return value_end > value_begin;
}

bool parse_json_bool(const std::string& text,
                     const std::string& key,
                     bool default_value) {
    std::size_t begin = 0;
    std::size_t end = 0;
    if (!find_json_value_range(text, key, begin, end)) {
        return default_value;
    }

    const std::string token = text.substr(begin, end - begin);
    if (token.find("true") != std::string::npos) {
        return true;
    }
    if (token.find("false") != std::string::npos) {
        return false;
    }
    return default_value;
}

float parse_json_float(const std::string& text,
                       const std::string& key,
                       float default_value) {
    std::size_t begin = 0;
    std::size_t end = 0;
    if (!find_json_value_range(text, key, begin, end)) {
        return default_value;
    }

    try {
        return std::stof(text.substr(begin, end - begin));
    } catch (...) {
        return default_value;
    }
}

long long parse_json_int64(const std::string& text,
                           const std::string& key,
                           long long default_value) {
    std::size_t begin = 0;
    std::size_t end = 0;
    if (!find_json_value_range(text, key, begin, end)) {
        return default_value;
    }

    try {
        return std::stoll(text.substr(begin, end - begin));
    } catch (...) {
        return default_value;
    }
}

std::string parse_json_string(const std::string& text,
                              const std::string& key,
                              const std::string& default_value) {
    std::size_t begin = 0;
    std::size_t end = 0;
    if (!find_json_value_range(text, key, begin, end)) {
        return default_value;
    }

    if (end <= begin + 1 || text[begin] != '"') {
        return default_value;
    }

    return text.substr(begin + 1, end - begin - 2);
}

void load_camera_bridge_from_json(const std::string& payload) {
    CameraBridgeState parsed{};
    parsed.bridge_connected = parse_json_bool(payload, "bridge_connected", true);
    parsed.sample_ready = parse_json_bool(payload, "sample_ready", true);
    parsed.device_unavailable = parse_json_bool(payload, "device_unavailable", false);
    parsed.face_detected = parse_json_bool(payload, "face_detected", false);
    parsed.mouth_open_state = parse_json_bool(payload, "mouth_open_state", false);
    parsed.looking_forward = parse_json_bool(payload, "looking_forward", false);
    parsed.mouth_center_x = std::clamp(parse_json_float(payload, "mouth_center_x", 0.5f), 0.0f, 1.0f);
    parsed.mouth_center_y = std::clamp(parse_json_float(payload, "mouth_center_y", 0.5f), 0.0f, 1.0f);
    parsed.confidence = std::clamp(parse_json_float(payload, "confidence", 0.0f), 0.0f, 1.0f);
    parsed.jaw_open_score = std::clamp(parse_json_float(payload, "jaw_open_score", 0.0f), 0.0f, 1.0f);
    parsed.mouth_open_ratio = std::max(0.0f, parse_json_float(payload, "mouth_open_ratio", 0.0f));
    parsed.timestamp_ms = parse_json_int64(payload, "timestamp_ms", 0);
    parsed.backend = parse_json_string(payload, "backend", "camera-json-bridge");
    parsed.status_text = parse_json_string(payload, "status_text", "camera bridge packet received");

    runtime_state().camera_bridge = parsed;
    runtime_state().camera_device_available = parsed.bridge_connected && !parsed.device_unavailable;
}

void load_microphone_bridge_from_json(const std::string& payload) {
    MicrophoneBridgeState parsed{};
    parsed.bridge_connected = parse_json_bool(payload, "bridge_connected", true);
    parsed.sample_ready = parse_json_bool(payload, "sample_ready", true);
    parsed.device_unavailable = parse_json_bool(payload, "device_unavailable", false);
    parsed.voice_detected = parse_json_bool(payload, "voice_detected", false);
    parsed.fallback_requested = parse_json_bool(payload, "fallback_requested", false);
    parsed.suggested_power = std::clamp(parse_json_float(payload, "suggested_power", 0.1f), 0.1f, 10.0f);
    parsed.direction_x = std::clamp(parse_json_float(payload, "direction_x", 0.0f), -1.0f, 1.0f);
    parsed.direction_y = std::clamp(parse_json_float(payload, "direction_y", -1.0f), -1.0f, 1.0f);
    parsed.confidence = std::clamp(parse_json_float(payload, "confidence", 0.0f), 0.0f, 1.0f);
    parsed.timestamp_ms = parse_json_int64(payload, "timestamp_ms", 0);
    parsed.backend = parse_json_string(payload, "backend", "microphone-json-bridge");
    parsed.status_text = parse_json_string(payload, "status_text", "microphone bridge packet received");

    runtime_state().microphone_bridge = parsed;
    runtime_state().microphone_device_available = parsed.bridge_connected && !parsed.device_unavailable;
}

}  // namespace

#if defined(_WIN32)
static CameraBridgeController g_camera;
#endif
static bool g_microphone_enabled = false;

void start_camera_bridge() {
    set_camera_bridge_enabled(true);
    refresh_bridge_inputs();
}

void stop_camera_bridge() {
    set_camera_bridge_enabled(false);
    refresh_bridge_inputs();
}

void start_microphone_bridge() {
    remove_file_if_exists(kMicrophoneBridgePath);
    set_microphone_bridge_enabled(true);
    refresh_bridge_inputs();
}

void stop_microphone_bridge() {
    set_microphone_bridge_enabled(false);
    refresh_bridge_inputs();
}

void set_camera_bridge_enabled(bool enabled) {
#if defined(_WIN32)
    if (enabled) {
        remove_file_if_exists(kCameraBridgePath);
        g_camera.start();
    } else {
        g_camera.stop();
    }
#else
    (void)enabled;
#endif

    RuntimeState& state_ref = runtime_state();
    state_ref.camera_bridge = enabled
        ? camera_bridge_disabled_state("camera bridge starting")
        : camera_bridge_disabled_state("camera bridge disabled");
    state_ref.camera_device_available = false;
    state_ref.camera_available = false;
}

void set_microphone_bridge_enabled(bool enabled) {
    g_microphone_enabled = enabled;

#if defined(_WIN32)
    if (enabled) {
        remove_file_if_exists(kMicrophoneBridgePath);
        native_microphone_bridge().start();
    } else {
        native_microphone_bridge().stop();
    }
#endif

    if (!enabled) {
        RuntimeState& state_ref = runtime_state();
        state_ref.microphone_bridge = microphone_bridge_disabled_state("microphone-bridge-disabled");
        state_ref.microphone_device_available = false;
        state_ref.microphone_available = false;
    }
}

bool camera_bridge_enabled() {
#if defined(_WIN32)
    return g_camera.enabled();
#else
    return false;
#endif
}

bool microphone_bridge_enabled() {
    return g_microphone_enabled;
}

void refresh_bridge_inputs() {
    CameraBridgeState cam_state;
#if defined(_WIN32)
    if (g_camera.refresh(cam_state)) {
        runtime_state().camera_bridge = cam_state;
        runtime_state().camera_device_available =
            cam_state.bridge_connected && !cam_state.device_unavailable;
    }
#else
    runtime_state().camera_bridge = camera_bridge_disabled_state("camera bridge unsupported");
    runtime_state().camera_device_available = false;
#endif

    if (!g_microphone_enabled) {
        runtime_state().microphone_bridge =
            microphone_bridge_disabled_state("microphone-bridge-disabled");
        runtime_state().microphone_device_available = false;
        return;
    }

    const std::string microphone_payload = read_text_file_if_exists(kMicrophoneBridgePath);
    if (!microphone_payload.empty()) {
        load_microphone_bridge_from_json(microphone_payload);
        return;
    }

#if defined(_WIN32)
    MicrophoneBridgeState native_state;
    if (native_microphone_bridge().refresh(native_state)) {
        runtime_state().microphone_bridge = native_state;
        runtime_state().microphone_device_available = native_state.bridge_connected;
        return;
    }
#endif

    runtime_state().microphone_bridge =
        microphone_bridge_disabled_state("microphone-bridge-unavailable");
    runtime_state().microphone_device_available = false;
}
