#include "thread.h"

#include <algorithm>
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
const char* kMicrophoneBridgePath = "microphone_bridge_latest.json";

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

class NativeMicrophoneBridge {
public:
    NativeMicrophoneBridge() = default;

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
        return true;
    }

    MicrophoneBridgeState unavailable_state(const std::string& backend) const {
        MicrophoneBridgeState state;
        state.backend = backend;
        state.bridge_connected = false;
        state.sample_ready = false;
        state.voice_detected = false;
        state.fallback_requested = false;
        state.suggested_power = 0.1f;
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
    runtime_state().camera_device_available = parsed.bridge_connected;
}

void load_microphone_bridge_from_json(const std::string& payload) {
    MicrophoneBridgeState parsed{};
    parsed.bridge_connected = parse_json_bool(payload, "bridge_connected", true);
    parsed.sample_ready = parse_json_bool(payload, "sample_ready", true);
    parsed.voice_detected = parse_json_bool(payload, "voice_detected", false);
    parsed.fallback_requested = parse_json_bool(payload, "fallback_requested", false);
    parsed.suggested_power = std::clamp(parse_json_float(payload, "suggested_power", 0.1f), 0.1f, 10.0f);
    parsed.direction_x = std::clamp(parse_json_float(payload, "direction_x", 0.0f), -1.0f, 1.0f);
    parsed.direction_y = std::clamp(parse_json_float(payload, "direction_y", -1.0f), -1.0f, 1.0f);
    parsed.confidence = std::clamp(parse_json_float(payload, "confidence", 0.0f), 0.0f, 1.0f);
    parsed.timestamp_ms = parse_json_int64(payload, "timestamp_ms", 0);
    parsed.backend = parse_json_string(payload, "backend", "microphone-json-bridge");

    runtime_state().microphone_bridge = parsed;
    runtime_state().microphone_device_available = parsed.bridge_connected;
}

}  // namespace

void refresh_bridge_inputs() {
    const std::string camera_payload = read_text_file_if_exists(kCameraBridgePath);
    if (!camera_payload.empty()) {
        load_camera_bridge_from_json(camera_payload);
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
    }
#endif
}
