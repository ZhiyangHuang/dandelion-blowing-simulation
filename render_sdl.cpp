#include "thread.h"
#include <SDL.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <random>
#include <sstream>
#include <vector>

using namespace std;

enum ButtonAction {
    BUTTON_EVENT,
    BUTTON_THREAD_MODE,
    BUTTON_CLOSE
};

struct Button {
    SDL_Rect rect;
    ButtonAction action;
    EventType event_type;
    int value;
    const char* label;
    SDL_Color fill;
    SDL_Color accent;
};

struct VisualSeed {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float life = 0.0f;
    float sway = 0.0f;
    float size = 5.0f;
    bool active = false;
};

static SDL_Window* window_handle = nullptr;
static SDL_Renderer* renderer_handle = nullptr;
static SDL_Texture* camera_texture = nullptr;
static bool app_running = false;
static Uint32 last_frame_tick = 0;
static Uint32 last_sim_tick = 0;
static Uint32 last_camera_emit = 0;

static cv::VideoCapture camera_capture;
static cv::CascadeClassifier face_cascade;
static cv::CascadeClassifier smile_cascade;
static cv::Mat camera_frame_bgr;
static cv::Mat camera_frame_rgba;
static bool camera_ready = false;
static bool mouth_detected = false;
static float mouth_norm_x = 0.5f;
static float mouth_norm_y = 0.5f;
static float wind_dir_x = 1.0f;
static float wind_dir_y = -0.08f;

static SDL_AudioDeviceID capture_device = 0;
static bool audio_ready = false;
static float mic_level = 0.0f;
static bool mic_gate_open = false;
static int mic_above_counter = 0;
static int mic_below_counter = 0;

static vector<VisualSeed> visual_seeds(100);
static mt19937 rng(20260412);
static int last_spawn_budget_seen = 0;
static bool use_camera_background = true;
static bool use_mouth_control = true;
static int configured_seed_count = 30;
static int rendered_center_seed_count = 30;
static float smoothed_wind_strength = 0.0f;
static float last_valid_mouth_x = 0.5f;
static float last_valid_mouth_y = 0.5f;
static string capture_device_name = "SYSTEM DEFAULT INPUT";
static const int center_seed_visual_max = 27;

static const int window_width = 1360;
static const int window_height = 840;
static const SDL_Rect hud_panel = {1004, 32, 320, 240};
static const SDL_Rect controls_panel = {1004, 292, 320, 496};

static void draw_background(Uint32 now);

static const Button buttons[] = {
    {{1034, 388, 126, 52}, BUTTON_EVENT, CAMERA_OPEN, 1, "CAMERA", {244, 233, 215, 255}, {194, 129, 61, 255}},
    {{1172, 388, 126, 52}, BUTTON_EVENT, MIC_START, 1, "MIC ON", {232, 242, 230, 255}, {89, 162, 120, 255}},
    {{1034, 454, 126, 52}, BUTTON_EVENT, MIC_END, 0, "MIC OFF", {238, 232, 230, 255}, {152, 124, 116, 255}},
    {{1172, 454, 126, 52}, BUTTON_EVENT, UI_RESET, 0, "RESET", {231, 237, 245, 255}, {101, 136, 190, 255}},
    {{1034, 536, 80, 44}, BUTTON_THREAD_MODE, CAMERA_CLOSE, 1, "1T", {236, 236, 236, 255}, {124, 124, 124, 255}},
    {{1126, 536, 80, 44}, BUTTON_THREAD_MODE, CAMERA_CLOSE, 2, "2T", {236, 236, 236, 255}, {124, 124, 124, 255}},
    {{1218, 536, 80, 44}, BUTTON_THREAD_MODE, CAMERA_CLOSE, 3, "3T", {236, 236, 236, 255}, {124, 124, 124, 255}},
    {{1034, 710, 264, 52}, BUTTON_CLOSE, CAMERA_CLOSE, 0, "CLOSE", {239, 228, 228, 255}, {171, 98, 98, 255}},
};

static const char* render_state_label(RenderState state) {
    switch (state) {
    case UI_START:
        return "START";
    case UI_CAMERA_LOADING:
        return "INPUT";
    case UI_INPUT_ACTIVE:
        return "INPUT ACTIVE";
    case UI_RUNNING:
        return "RUNNING";
    case UI_WAITING:
        return "WAITING";
    case UI_PAUSED:
        return "PAUSED";
    default:
        return "UNKNOWN";
    }
}

static const char* glyph_for(char c) {
    switch (c) {
    case 'A': return "01110""10001""10001""11111""10001""10001""10001";
    case 'B': return "11110""10001""10001""11110""10001""10001""11110";
    case 'C': return "01111""10000""10000""10000""10000""10000""01111";
    case 'D': return "11110""10001""10001""10001""10001""10001""11110";
    case 'E': return "11111""10000""10000""11110""10000""10000""11111";
    case 'F': return "11111""10000""10000""11110""10000""10000""10000";
    case 'G': return "01111""10000""10000""10011""10001""10001""01110";
    case 'H': return "10001""10001""10001""11111""10001""10001""10001";
    case 'I': return "11111""00100""00100""00100""00100""00100""11111";
    case 'K': return "10001""10010""10100""11000""10100""10010""10001";
    case 'L': return "10000""10000""10000""10000""10000""10000""11111";
    case 'M': return "10001""11011""10101""10001""10001""10001""10001";
    case 'N': return "10001""11001""10101""10011""10001""10001""10001";
    case 'O': return "01110""10001""10001""10001""10001""10001""01110";
    case 'P': return "11110""10001""10001""11110""10000""10000""10000";
    case 'R': return "11110""10001""10001""11110""10100""10010""10001";
    case 'S': return "01111""10000""10000""01110""00001""00001""11110";
    case 'T': return "11111""00100""00100""00100""00100""00100""00100";
    case 'U': return "10001""10001""10001""10001""10001""10001""01110";
    case 'V': return "10001""10001""10001""10001""10001""01010""00100";
    case 'W': return "10001""10001""10001""10101""10101""10101""01010";
    case 'Y': return "10001""10001""01010""00100""00100""00100""00100";
    case '0': return "01110""10001""10011""10101""11001""10001""01110";
    case '1': return "00100""01100""00100""00100""00100""00100""01110";
    case '2': return "01110""10001""00001""00010""00100""01000""11111";
    case '3': return "11110""00001""00001""01110""00001""00001""11110";
    case '4': return "00010""00110""01010""10010""11111""00010""00010";
    case '5': return "11111""10000""10000""11110""00001""00001""11110";
    case '6': return "01110""10000""10000""11110""10001""10001""01110";
    case '7': return "11111""00001""00010""00100""01000""01000""01000";
    case '8': return "01110""10001""10001""01110""10001""10001""01110";
    case '9': return "01110""10001""10001""01111""00001""00001""01110";
    case ':': return "00000""00100""00100""00000""00100""00100""00000";
    case '/': return "00001""00010""00100""01000""10000""00000""00000";
    case '+': return "00000""00100""00100""11111""00100""00100""00000";
    case '-': return "00000""00000""00000""11111""00000""00000""00000";
    case '.': return "00000""00000""00000""00000""00000""00110""00110";
    case ' ': return "00000""00000""00000""00000""00000""00000""00000";
    default:  return "00000""00000""00000""00000""00000""00000""00000";
    }
}

static void fill_rect(const SDL_Rect& rect, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer_handle, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer_handle, &rect);
}

static void stroke_rect(const SDL_Rect& rect, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer_handle, color.r, color.g, color.b, color.a);
    SDL_RenderDrawRect(renderer_handle, &rect);
}

static void draw_gradient_rect(const SDL_Rect& rect, SDL_Color top, SDL_Color bottom) {
    for (int i = 0; i < rect.h; ++i) {
        float t = rect.h <= 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(rect.h - 1);
        Uint8 r = static_cast<Uint8>(top.r + (bottom.r - top.r) * t);
        Uint8 g = static_cast<Uint8>(top.g + (bottom.g - top.g) * t);
        Uint8 b = static_cast<Uint8>(top.b + (bottom.b - top.b) * t);
        SDL_SetRenderDrawColor(renderer_handle, r, g, b, 255);
        SDL_RenderDrawLine(renderer_handle, rect.x, rect.y + i, rect.x + rect.w, rect.y + i);
    }
}

static void draw_filled_circle(int cx, int cy, int radius, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer_handle, color.r, color.g, color.b, color.a);
    for (int dy = -radius; dy <= radius; ++dy) {
        int dx = static_cast<int>(sqrt(radius * radius - dy * dy));
        SDL_RenderDrawLine(renderer_handle, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void draw_glyph(int x, int y, char c, int scale, SDL_Color color) {
    const char* glyph = glyph_for(c);
    SDL_SetRenderDrawColor(renderer_handle, color.r, color.g, color.b, color.a);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (glyph[row * 5 + col] != '1') {
                continue;
            }
            SDL_Rect pixel = {x + col * scale, y + row * scale, scale, scale};
            SDL_RenderFillRect(renderer_handle, &pixel);
        }
    }
}

static void draw_text(int x, int y, const string& text, SDL_Color color, int scale) {
    int cursor_x = x;
    for (char c : text) {
        draw_glyph(cursor_x, y, static_cast<char>(toupper(static_cast<unsigned char>(c))), scale, color);
        cursor_x += 6 * scale;
    }
}

static void clear_visual_particles() {
    for (VisualSeed& seed : visual_seeds) {
        seed.active = false;
        seed.life = 0.0f;
        seed.vx = 0.0f;
        seed.vy = 0.0f;
    }
    last_spawn_budget_seen = 0;
    rendered_center_seed_count = configured_seed_count;
    smoothed_wind_strength = 0.0f;
}

static void spawn_visual_burst(int count) {
    uniform_real_distribution<float> angle_dist(-0.35f, 0.35f);
    uniform_real_distribution<float> speed_dist(40.0f, 95.0f);
    uniform_real_distribution<float> spread_dist(-18.0f, 18.0f);
    uniform_real_distribution<float> size_dist(4.0f, 7.0f);
    uniform_real_distribution<float> sway_dist(0.0f, 6.28318f);

    const float origin_x = window_width * 0.5f;
    const float origin_y = window_height * 0.52f;
    count = clamp(count, 0, 100);

    int activated = 0;
    for (VisualSeed& seed : visual_seeds) {
        if (activated >= count) {
            break;
        }
        if (seed.active) {
            continue;
        }

        float angle = atan2(wind_dir_y, wind_dir_x) + angle_dist(rng);
        float speed = speed_dist(rng);
        seed.x = origin_x + spread_dist(rng);
        seed.y = origin_y + spread_dist(rng) * 0.3f;
        seed.vx = cos(angle) * speed;
        seed.vy = sin(angle) * speed - 8.0f;
        seed.life = 10.0f;
        seed.sway = sway_dist(rng);
        seed.size = size_dist(rng);
        seed.active = true;
        activated++;
    }
}

static void sync_visual_spawns() {
    LatestValueBuffer& latest = latest_buffer();
    if (latest.spawn_budget > last_spawn_budget_seen) {
        int delta = latest.spawn_budget - last_spawn_budget_seen;
        spawn_visual_burst(delta);
    }
    last_spawn_budget_seen = latest.spawn_budget;
    rendered_center_seed_count = latest.seeds_left;
}

static void set_wind_direction_from_mouth() {
    float mouth_x = mouth_norm_x * window_width;
    float mouth_y = mouth_norm_y * window_height;
    float center_x = window_width * 0.5f;
    float center_y = window_height * 0.52f;
    float target_x = center_x - mouth_x;
    float target_y = center_y - mouth_y;
    float len = sqrt(target_x * target_x + target_y * target_y);
    if (len < 0.001f) {
        return;
    }
    target_x /= len;
    target_y /= len;
    wind_dir_x = wind_dir_x * 0.82f + target_x * 0.18f;
    wind_dir_y = wind_dir_y * 0.82f + target_y * 0.18f;
    float smoothed_len = sqrt(wind_dir_x * wind_dir_x + wind_dir_y * wind_dir_y);
    if (smoothed_len > 0.001f) {
        wind_dir_x /= smoothed_len;
        wind_dir_y /= smoothed_len;
    }
}

static void draw_status_chip(int x, int y, const string& label, SDL_Color fill, SDL_Color border, SDL_Color text) {
    SDL_Rect rect = {x, y, static_cast<int>(label.size()) * 14 + 28, 30};
    fill_rect(rect, fill);
    stroke_rect(rect, border);
    draw_text(x + 12, y + 8, label, text, 2);
}

static void draw_scene_background() {
    if (camera_ready && camera_texture && use_camera_background &&
        runtime_config().camera_background_enabled) {
        SDL_Rect full = {0, 0, window_width, window_height};
        SDL_RenderCopy(renderer_handle, camera_texture, nullptr, &full);
        fill_rect(full, {255, 248, 236, 70});
        return;
    }

    draw_background(SDL_GetTicks());
}

static bool init_camera_input() {
    const string face_path = "C:/msys64/mingw64/share/opencv4/haarcascades/haarcascade_frontalface_default.xml";
    const string smile_path = "C:/msys64/mingw64/share/opencv4/haarcascades/haarcascade_smile.xml";
    if (!face_cascade.load(face_path) || !smile_cascade.load(smile_path)) {
        return false;
    }
    if (!camera_capture.open(0)) {
        return false;
    }
    camera_capture.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    camera_capture.set(cv::CAP_PROP_FRAME_HEIGHT, 360);
    camera_ready = true;
    return true;
}

static bool init_audio_input() {
    SDL_AudioSpec desired = {};
    desired.freq = 48000;
    desired.format = AUDIO_F32SYS;
    desired.channels = 1;
    desired.samples = 1024;
    desired.callback = nullptr;

    int num_capture_devices = SDL_GetNumAudioDevices(SDL_TRUE);
    if (num_capture_devices > 0) {
        const char* first_name = SDL_GetAudioDeviceName(0, SDL_TRUE);
        if (first_name && first_name[0] != '\0') {
            capture_device_name = first_name;
        }
    }

    capture_device = SDL_OpenAudioDevice(nullptr, SDL_TRUE, &desired, nullptr, 0);
    if (capture_device == 0) {
        capture_device_name = "NO CAPTURE DEVICE";
        return false;
    }

    SDL_PauseAudioDevice(capture_device, 0);
    audio_ready = true;
    return true;
}

static void update_camera_texture() {
    if (!camera_ready || camera_frame_rgba.empty()) {
        return;
    }

    if (!camera_texture) {
        camera_texture = SDL_CreateTexture(renderer_handle,
                                           SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           camera_frame_rgba.cols,
                                           camera_frame_rgba.rows);
    }

    if (!camera_texture) {
        return;
    }

    SDL_UpdateTexture(camera_texture, nullptr, camera_frame_rgba.data, camera_frame_rgba.step);
}

static void process_camera_input() {
    if (!camera_ready) {
        mouth_detected = false;
        return;
    }

    camera_capture >> camera_frame_bgr;
    if (camera_frame_bgr.empty()) {
        mouth_detected = false;
        return;
    }

    cv::Mat gray;
    cv::cvtColor(camera_frame_bgr, gray, cv::COLOR_BGR2GRAY);
    cv::equalizeHist(gray, gray);

    vector<cv::Rect> faces;
    face_cascade.detectMultiScale(gray, faces, 1.1, 4, 0, cv::Size(120, 120));

    mouth_detected = false;
    if (!faces.empty()) {
        auto face = *max_element(faces.begin(), faces.end(),
                                 [](const cv::Rect& a, const cv::Rect& b) { return a.area() < b.area(); });
        cv::rectangle(camera_frame_bgr, face, cv::Scalar(80, 180, 120), 2);

        cv::Rect lower_face(face.x, face.y + face.height / 2, face.width, face.height / 2);
        lower_face &= cv::Rect(0, 0, gray.cols, gray.rows);

        vector<cv::Rect> smiles;
        smile_cascade.detectMultiScale(gray(lower_face), smiles, 1.5, 18, 0, cv::Size(32, 20));

        cv::Rect mouth_rect;
        if (!smiles.empty()) {
            auto smile = *max_element(smiles.begin(), smiles.end(),
                                      [](const cv::Rect& a, const cv::Rect& b) { return a.area() < b.area(); });
            mouth_rect = cv::Rect(lower_face.x + smile.x, lower_face.y + smile.y, smile.width, smile.height);
            mouth_detected = true;
        } else {
            mouth_rect = cv::Rect(face.x + face.width / 4,
                                  face.y + static_cast<int>(face.height * 0.68f),
                                  face.width / 2,
                                  max(18, face.height / 7));
            mouth_detected = true;
        }

        cv::rectangle(camera_frame_bgr, mouth_rect, cv::Scalar(60, 130, 255), 2);
        mouth_norm_x = (mouth_rect.x + mouth_rect.width * 0.5f) / static_cast<float>(camera_frame_bgr.cols);
        mouth_norm_y = (mouth_rect.y + mouth_rect.height * 0.5f) / static_cast<float>(camera_frame_bgr.rows);
        set_wind_direction_from_mouth();

        LatestValueBuffer& latest = latest_buffer();
        last_valid_mouth_x = mouth_norm_x;
        last_valid_mouth_y = mouth_norm_y;
        latest.mouth_x = mouth_norm_x;
        latest.mouth_y = mouth_norm_y;
        latest.mouth_open = !smiles.empty();
        latest.wind_dir_x = wind_dir_x;
        latest.wind_dir_y = wind_dir_y;

        Uint32 now = SDL_GetTicks();
        if (latest.mouth_open && now - last_camera_emit > 700) {
            emit_event(CAMERA_OPEN, 1, "mouth_open");
            last_camera_emit = now;
        }
    } else {
        latest_buffer().mouth_open = false;
    }

    cv::Mat resized;
    cv::resize(camera_frame_bgr, resized, cv::Size(window_width, window_height));
    cv::cvtColor(resized, camera_frame_rgba, cv::COLOR_BGR2RGBA);
    update_camera_texture();
}

static void process_audio_input() {
    if (!audio_ready) {
        return;
    }

    Uint32 bytes_available = SDL_GetQueuedAudioSize(capture_device);
    if (bytes_available < sizeof(float) * 512) {
        return;
    }

    vector<float> samples(bytes_available / sizeof(float));
    Uint32 bytes_read = SDL_DequeueAudio(capture_device, samples.data(), bytes_available);
    if (bytes_read == 0) {
        return;
    }

    size_t sample_count = bytes_read / sizeof(float);
    double sum_sq = 0.0;
    for (size_t i = 0; i < sample_count; ++i) {
        sum_sq += samples[i] * samples[i];
    }

    float rms = static_cast<float>(sqrt(sum_sq / max<size_t>(1, sample_count)));
    mic_level = mic_level * 0.75f + rms * 0.25f;

    LatestValueBuffer& latest = latest_buffer();
    float normalized = 0.0f;
    if (mic_level > runtime_config().mic_lower_threshold) {
        float range = runtime_config().mic_upper_threshold - runtime_config().mic_lower_threshold;
        if (range < 0.0001f) {
            range = 0.0001f;
        }
        normalized = (mic_level - runtime_config().mic_lower_threshold) / range;
        normalized = clamp(normalized, 0.0f, 1.0f);
    }
    smoothed_wind_strength = smoothed_wind_strength * 0.8f + normalized * 0.2f;
    latest.wind_strength = smoothed_wind_strength;
    latest.wind_active = mic_gate_open;
    latest.wind_dir_x = wind_dir_x;
    latest.wind_dir_y = wind_dir_y;

    if (!mic_gate_open) {
        if (mic_level > runtime_config().mic_upper_threshold) {
            mic_above_counter++;
            if (mic_above_counter >= 3) {
                mic_gate_open = true;
                mic_above_counter = 0;
                latest.wind_active = true;
                emit_event(MIC_START, 1, "mic_threshold");
            }
        } else {
            mic_above_counter = 0;
        }
    } else {
        if (mic_level < runtime_config().mic_lower_threshold) {
            mic_below_counter++;
            if (mic_below_counter >= 6) {
                mic_gate_open = false;
                mic_below_counter = 0;
                latest.wind_active = false;
                emit_event(MIC_END, 0, "mic_quiet");
            }
        } else {
            mic_below_counter = 0;
        }
    }
}

static void draw_button(const Button& button, bool active) {
    SDL_Color fill = active ? button.accent : button.fill;
    SDL_Color border = active ? SDL_Color{58, 53, 48, 255} : SDL_Color{119, 109, 98, 255};
    fill_rect(button.rect, fill);
    stroke_rect(button.rect, border);
    SDL_Rect accent_bar = {button.rect.x, button.rect.y, button.rect.w, 6};
    fill_rect(accent_bar, active ? SDL_Color{255, 255, 255, 120} : button.accent);
    draw_text(button.rect.x + 18, button.rect.y + 18, button.label,
              active ? SDL_Color{255, 255, 255, 255} : SDL_Color{42, 42, 42, 255}, 3);
}

static void update_visual_particles(float dt) {
    for (VisualSeed& seed : visual_seeds) {
        if (!seed.active) {
            continue;
        }

        seed.life -= dt;
        float wind_strength = latest_buffer().wind_strength;
        if (wind_on) {
            seed.vx += wind_dir_x * (24.0f + wind_strength * 42.0f) * dt;
            seed.vy += wind_dir_y * (16.0f + wind_strength * 28.0f) * dt;
        } else {
            seed.vx += 4.0f * dt;
            seed.vy -= 2.5f * dt;
        }

        seed.vx *= 0.995f;
        seed.vy *= 0.993f;
        seed.x += seed.vx * dt;
        seed.y += seed.vy * dt + sin(seed.sway + SDL_GetTicks() * 0.003f) * 8.0f * dt;

        if (seed.life <= 0.0f) {
            seed.active = false;
            continue;
        }

        if (seed.x < 18.0f || seed.x > window_width - 18.0f ||
            seed.y < 18.0f || seed.y > window_height - 18.0f) {
            seed.active = false;
            seed.vx = 0.0f;
            seed.vy = 0.0f;
        }
    }
}

static void trigger_reset() {
    clear_visual_particles();
    mic_gate_open = false;
    mic_above_counter = 0;
    mic_below_counter = 0;
    emit_event(UI_RESET, 0, "reset");
}

static void handle_button(const Button& button) {
    switch (button.action) {
    case BUTTON_EVENT:
        if (button.event_type == UI_RESET) {
            trigger_reset();
        } else {
            emit_event(button.event_type, button.value, button.label);
        }
        break;
    case BUTTON_THREAD_MODE:
        rebuild_runtime_threads(static_cast<ThreadMode>(button.value));
        emit_event(CAMERA_OPEN, 1, "thread_mode_restart");
        break;
    case BUTTON_CLOSE:
        app_running = false;
        break;
    }
}

static void draw_breeze_ribbons(bool wind_active, Uint32 now) {
    SDL_Color ribbon = wind_active ? SDL_Color{120, 181, 255, 150} : SDL_Color{201, 215, 232, 90};
    SDL_SetRenderDrawColor(renderer_handle, ribbon.r, ribbon.g, ribbon.b, ribbon.a);

    for (int i = 0; i < 7; ++i) {
        int base_y = 120 + i * 66;
        int phase = static_cast<int>((now / 16 + i * 20) % 120);
        for (int seg = 0; seg < 18; ++seg) {
            int x1 = 470 + seg * 42;
            int x2 = x1 + 38;
            int y1 = base_y + static_cast<int>(sin((phase + seg * 10) * 0.08) * (wind_active ? 14 : 6));
            int y2 = base_y + static_cast<int>(sin((phase + seg * 10 + 18) * 0.08) * (wind_active ? 14 : 6));
            SDL_RenderDrawLine(renderer_handle, x1, y1, x2, y2);
        }
    }
}

static bool dandelion_task_active() {
    LatestValueBuffer& latest = latest_buffer();
    return current_render_state() == UI_RUNNING || latest.spawn_budget > 0;
}

static void draw_particle_seed(const VisualSeed& seed, Uint32 now) {
    if (!seed.active) {
        return;
    }

    float flutter = sin(seed.sway + now * 0.003f) * 2.5f;
    int screen_x = static_cast<int>(seed.x + flutter);
    int screen_y = static_cast<int>(seed.y + flutter * 0.3f);

    SDL_Color head = wind_on ? SDL_Color{255, 233, 185, 240}
                             : SDL_Color{252, 248, 241, 236};

    SDL_SetRenderDrawColor(renderer_handle, 160, 144, 118, 220);
    SDL_RenderDrawLine(renderer_handle, screen_x, screen_y,
                       screen_x - static_cast<int>(seed.vx * 0.06f) - 18,
                       screen_y + static_cast<int>(seed.vy * 0.03f) + 12);

    draw_filled_circle(screen_x, screen_y, static_cast<int>(seed.size), head);

    SDL_SetRenderDrawColor(renderer_handle, 255, 252, 246, 180);
    for (int i = -3; i <= 3; ++i) {
        SDL_RenderDrawLine(renderer_handle, screen_x, screen_y, screen_x + 15, screen_y - 12 + i * 4);
    }

    SDL_SetRenderDrawColor(renderer_handle, 255, 255, 255, 80);
    SDL_RenderDrawLine(renderer_handle, screen_x - static_cast<int>(seed.vx * 0.12f),
                       screen_y - static_cast<int>(seed.vy * 0.08f),
                       screen_x + 2, screen_y);
}

static void draw_background(Uint32 now) {
    SDL_Rect sky = {0, 0, window_width, window_height};
    draw_gradient_rect(sky, {230, 239, 247, 255}, {246, 231, 206, 255});

    SDL_SetRenderDrawColor(renderer_handle, 255, 255, 255, 80);
    for (int i = 0; i < 5; ++i) {
        int cx = 190 + i * 220;
        int cy = 100 + (i % 2) * 46;
        draw_filled_circle(cx, cy, 30, {255, 255, 255, 68});
        draw_filled_circle(cx + 36, cy + 8, 24, {255, 255, 255, 68});
        draw_filled_circle(cx - 36, cy + 6, 22, {255, 255, 255, 68});
    }

    SDL_Rect meadow = {0, 620, window_width, 220};
    draw_gradient_rect(meadow, {189, 211, 166, 255}, {126, 160, 109, 255});

    bool wind_visual_active = latest_buffer().wind_active;
    SDL_Color ribbon = wind_visual_active ? SDL_Color{116, 181, 255, 120} : SDL_Color{198, 211, 227, 70};
    SDL_SetRenderDrawColor(renderer_handle, ribbon.r, ribbon.g, ribbon.b, ribbon.a);
    for (int i = 0; i < 7; ++i) {
        int base_y = 170 + i * 54;
        int phase = static_cast<int>((now / 14 + i * 19) % 120);
        for (int seg = 0; seg < 18; ++seg) {
            int x1 = 420 + seg * 42;
            int x2 = x1 + 36;
            int y1 = base_y + static_cast<int>(sin((phase + seg * 10) * 0.08) * (wind_visual_active ? 16 : 6));
            int y2 = base_y + static_cast<int>(sin((phase + seg * 10 + 18) * 0.08) * (wind_visual_active ? 16 : 6));
            SDL_RenderDrawLine(renderer_handle, x1, y1, x2, y2);
        }
    }
}

static void draw_dandelion_core(int center_x, int center_y) {
    SDL_SetRenderDrawColor(renderer_handle, 88, 129, 85, 255);
    SDL_RenderDrawLine(renderer_handle, center_x, center_y + 188, center_x, center_y + 24);
    SDL_RenderDrawLine(renderer_handle, center_x, center_y + 108, center_x - 40, center_y + 76);
    SDL_RenderDrawLine(renderer_handle, center_x, center_y + 88, center_x + 42, center_y + 58);

    draw_filled_circle(center_x, center_y, 20, {209, 198, 182, 255});
    draw_filled_circle(center_x, center_y, 13, {231, 223, 213, 255});
}

static void draw_center_seed_ring(int center_x, int center_y, int seeds_left, int seeds_total) {
    if (seeds_total <= 0 || seeds_left <= 0) {
        return;
    }

    float ratio = static_cast<float>(clamp(seeds_left, 0, seeds_total)) /
        static_cast<float>(max(1, seeds_total));
    int visible = max(1, static_cast<int>(round(ratio * center_seed_visual_max)));
    visible = clamp(visible, 0, center_seed_visual_max);
    int half_visual = center_seed_visual_max / 2;
    bool use_full_ring = visible > half_visual;

    float angle_span = use_full_ring ? 360.0f : 224.0f;
    float angle_start = use_full_ring ? -180.0f : -112.0f;
    float step = angle_span / static_cast<float>(max(1, visible));

    SDL_SetRenderDrawColor(renderer_handle, 250, 246, 239, 255);
    for (int i = 0; i < visible; ++i) {
        float angle = (angle_start + step * i) * 3.14159265f / 180.0f;
        int tip_x = center_x + static_cast<int>(cos(angle) * 82.0f);
        int tip_y = center_y + static_cast<int>(sin(angle) * 82.0f);
        SDL_RenderDrawLine(renderer_handle, center_x, center_y, tip_x, tip_y);
        draw_filled_circle(tip_x, tip_y, 3, {255, 252, 245, 255});
    }
}

static void draw_mouth_cursor_on_scene() {
    if (!use_mouth_control || !mouth_detected || !latest_buffer().mouth_open ||
        current_render_state() == UI_WAITING) {
        return;
    }

    int cx = static_cast<int>(last_valid_mouth_x * window_width);
    int cy = static_cast<int>(last_valid_mouth_y * window_height);
    int center_x = window_width / 2;
    int center_y = static_cast<int>(window_height * 0.52f);
    SDL_SetRenderDrawColor(renderer_handle, 255, 214, 140, 180);
    SDL_RenderDrawLine(renderer_handle, cx, cy, center_x, center_y);
    draw_filled_circle(cx, cy, 10, {255, 186, 84, 180});
    stroke_rect(SDL_Rect{cx - 16, cy - 16, 32, 32}, {255, 186, 84, 255});
}

static void draw_metrics_panel() {
    LatestValueBuffer& latest = latest_buffer();
    fill_rect(hud_panel, {250, 244, 236, 235});
    stroke_rect(hud_panel, {120, 109, 99, 255});

    draw_text(hud_panel.x + 18, hud_panel.y + 16, "SYSTEM HUD", {43, 39, 36, 255}, 3);
    draw_text(hud_panel.x + 18, hud_panel.y + 60, string("STATE: ") + render_state_label(current_render_state()),
              {68, 63, 58, 255}, 2);
    draw_text(hud_panel.x + 18, hud_panel.y + 88, string("WIND: ") + (latest.wind_active ? "ON" : "OFF"),
              {68, 63, 58, 255}, 2);

    ostringstream spawn;
    spawn << "SPAWN: " << latest.spawn_budget << "/" << latest.max_spawn_budget;
    draw_text(hud_panel.x + 18, hud_panel.y + 116, spawn.str(), {68, 63, 58, 255}, 2);

    ostringstream overflow;
    overflow << "OVERFLOW: " << latest.overflow_count;
    draw_text(hud_panel.x + 18, hud_panel.y + 144, overflow.str(), {68, 63, 58, 255}, 2);

    draw_text(hud_panel.x + 18, hud_panel.y + 172, "VISUAL PARTICLES: AUTO", {68, 63, 58, 255}, 2);

    ostringstream mic_text;
    mic_text << "MIC RMS: " << mic_level;
    draw_text(hud_panel.x + 18, hud_panel.y + 200, mic_text.str(), {68, 63, 58, 255}, 2);

    string mic_device_text = "MIC DEV: " + capture_device_name.substr(0, min<size_t>(19, capture_device_name.size()));
    draw_text(hud_panel.x + 18, hud_panel.y + 214, mic_device_text, {68, 63, 58, 255}, 2);

    SDL_Rect mic_bar = {hud_panel.x + 168, hud_panel.y + 217, min(120, static_cast<int>(mic_level * 1600.0f)), 10};
    fill_rect(mic_bar, mic_gate_open ? SDL_Color{82, 170, 114, 255} : SDL_Color{132, 160, 194, 255});

    ostringstream mic_threshold_text;
    mic_threshold_text << "MIC TH: " << runtime_config().mic_lower_threshold
                       << "/" << runtime_config().mic_upper_threshold;
    draw_text(hud_panel.x + 18, hud_panel.y + 242, mic_threshold_text.str(), {68, 63, 58, 255}, 2);

    ostringstream wind_strength_text;
    wind_strength_text << "WIND STRENGTH: " << static_cast<int>(latest.wind_strength * 100.0f);
    draw_text(hud_panel.x + 18, hud_panel.y + 270, wind_strength_text.str(), {68, 63, 58, 255}, 2);

    ostringstream seeds_text;
    seeds_text << "SEEDS LEFT: " << latest.seeds_left << "/" << latest.seeds_total;
    draw_text(hud_panel.x + 18, hud_panel.y + 298, seeds_text.str(), {68, 63, 58, 255}, 2);

    string thread_mode_text = "THREAD MODE: " + to_string(static_cast<int>(current_thread_mode()));
    draw_text(hud_panel.x + 18, hud_panel.y + 326, thread_mode_text, {68, 63, 58, 255}, 2);

    draw_text(hud_panel.x + 18, hud_panel.y + 354, latest.mouth_open ? "MOUTH: OPEN" : "MOUTH: CLOSED",
              {68, 63, 58, 255}, 2);

    draw_status_chip(hud_panel.x + 18, hud_panel.y + 384, latest.wind_active ? "MIC HOT" : "MIC IDLE",
                     latest.wind_active ? SDL_Color{220, 241, 223, 255} : SDL_Color{238, 236, 233, 255},
                     latest.wind_active ? SDL_Color{87, 160, 115, 255} : SDL_Color{128, 120, 114, 255},
                     latest.wind_active ? SDL_Color{52, 102, 71, 255} : SDL_Color{84, 78, 74, 255});

    draw_status_chip(hud_panel.x + 164, hud_panel.y + 384, current_render_state() == UI_RUNNING ? "LIVE" : "IDLE",
                     current_render_state() == UI_RUNNING ? SDL_Color{231, 238, 247, 255} : SDL_Color{239, 235, 230, 255},
                     current_render_state() == UI_RUNNING ? SDL_Color{94, 130, 187, 255} : SDL_Color{128, 120, 114, 255},
                     current_render_state() == UI_RUNNING ? SDL_Color{53, 84, 133, 255} : SDL_Color{84, 78, 74, 255});

    SDL_Rect spawn_track = {hud_panel.x + 18, hud_panel.y + 432, 290, 18};
    fill_rect(spawn_track, {231, 228, 222, 255});
    SDL_Rect spawn_fill = {hud_panel.x + 18, hud_panel.y + 432, min(290, latest.spawn_budget * 3), 18};
    fill_rect(spawn_fill, {80, 90, 104, 255});
    stroke_rect(spawn_track, {120, 109, 99, 255});
}

static void draw_controls_panel() {
    fill_rect(controls_panel, {248, 242, 234, 235});
    stroke_rect(controls_panel, {120, 109, 99, 255});

    draw_text(controls_panel.x + 18, controls_panel.y + 16, "LIVE CONTROLS", {43, 39, 36, 255}, 3);
    draw_text(controls_panel.x + 18, controls_panel.y + 48, "KEYS: C M N R B V ESC", {92, 87, 83, 255}, 2);
    draw_text(controls_panel.x + 18, controls_panel.y + 76, "BUTTONS REMAIN FOR TESTING", {92, 87, 83, 255}, 2);
    draw_text(controls_panel.x + 18, controls_panel.y + 104, "THREAD BUTTONS SWITCH 1T / 2T / 3T", {92, 87, 83, 255}, 2);

    LatestValueBuffer& latest = latest_buffer();
    for (const Button& button : buttons) {
        bool active = false;
        if (button.event_type == MIC_START) {
            active = latest.wind_active || mic_gate_open;
        } else if (button.event_type == CAMERA_OPEN) {
            active = current_render_state() == UI_CAMERA_LOADING ||
                (current_render_state() == UI_INPUT_ACTIVE && !latest.wind_active && latest.spawn_budget == 0);
        } else if (button.action == BUTTON_THREAD_MODE) {
            active = static_cast<int>(current_thread_mode()) == button.value;
        }
        draw_button(button, active);
    }

    draw_text(controls_panel.x + 18, controls_panel.y + 338, "1T: CAMERA THEN MIC THEN PARTICLES", {72, 70, 68, 255}, 2);
    draw_text(controls_panel.x + 18, controls_panel.y + 366, "2T: CAMERA AND MIC SHARE INPUT FLOW", {72, 70, 68, 255}, 2);
    draw_text(controls_panel.x + 18, controls_panel.y + 394, "3T: CAMERA MIC RENDER RUN SEPARATELY", {72, 70, 68, 255}, 2);
    draw_text(controls_panel.x + 18, controls_panel.y + 422, "CAMERA SHOULD BLOCK BETWEEN PHASES", {72, 70, 68, 255}, 2);
    draw_text(controls_panel.x + 18, controls_panel.y + 450, "PARTICLES STOP INDIVIDUALLY AT BORDER", {72, 70, 68, 255}, 2);
    draw_text(controls_panel.x + 18, controls_panel.y + 478, use_camera_background ? "B: CAMERA BG ON" : "B: SKY BG ON", {72, 70, 68, 255}, 2);
    draw_text(controls_panel.x + 18, controls_panel.y + 506, use_mouth_control ? "V: MOUTH CURSOR ON" : "V: MOUTH CURSOR OFF", {72, 70, 68, 255}, 2);
    draw_text(controls_panel.x + 18, controls_panel.y + 534, "LIVE INPUT ACTIVE", {72, 70, 68, 255}, 2);
}

static void draw_scene() {
    LatestValueBuffer& latest = latest_buffer();
    Uint32 now = SDL_GetTicks();

    draw_scene_background();
    draw_breeze_ribbons(latest.wind_active, now);

    SDL_Rect title_band = {414, 36, 520, 84};
    fill_rect(title_band, {252, 247, 241, 220});
    stroke_rect(title_band, {129, 118, 108, 255});
    draw_text(442, 58, "DANDELION BREEZE LIVE", {48, 43, 38, 255}, 4);
    draw_text(442, 90, "WEBCAM MOUTH TRACKING AND MIC BLOWING", {86, 80, 74, 255}, 2);

    int center_x = window_width / 2;
    int center_y = static_cast<int>(window_height * 0.52f);
    if (dandelion_task_active()) {
        center_x += static_cast<int>(sin(now * 0.0025f) * 6.0f);
        center_y += static_cast<int>(cos(now * 0.0018f) * 4.0f);
    }
    draw_dandelion_core(center_x, center_y);
    draw_center_seed_ring(center_x, center_y, rendered_center_seed_count, latest.seeds_total);
    draw_mouth_cursor_on_scene();

    for (const VisualSeed& seed : visual_seeds) {
        draw_particle_seed(seed, now);
    }

    draw_metrics_panel();
    draw_controls_panel();

    ostringstream title;
    title << "Dandelion Breeze Live | State=" << render_state_label(current_render_state())
          << " | Wind=" << (latest.wind_active ? "ON" : "OFF")
          << " | Particles=AUTO"
          << " | Mic=" << mic_level
          << " | Mode=" << static_cast<int>(current_thread_mode());
    SDL_SetWindowTitle(window_handle, title.str().c_str());

    SDL_RenderPresent(renderer_handle);
}

bool init_visualization() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        return false;
    }

    window_handle = SDL_CreateWindow(
        "Dandelion Breeze Live",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        window_width,
        window_height,
        SDL_WINDOW_SHOWN);

    if (!window_handle) {
        SDL_Quit();
        return false;
    }

    renderer_handle = SDL_CreateRenderer(
        window_handle,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!renderer_handle) {
        SDL_DestroyWindow(window_handle);
        window_handle = nullptr;
        SDL_Quit();
        return false;
    }

    SDL_SetRenderDrawBlendMode(renderer_handle, SDL_BLENDMODE_BLEND);
    init_camera_input();
    init_audio_input();
    configured_seed_count = runtime_config().initial_seed_count;
    rendered_center_seed_count = configured_seed_count;
    latest_buffer().seeds_total = configured_seed_count;
    latest_buffer().seeds_left = configured_seed_count;
    clear_visual_particles();
    app_running = true;
    last_frame_tick = SDL_GetTicks();
    last_sim_tick = last_frame_tick;
    return true;
}

void shutdown_visualization() {
    if (capture_device != 0) {
        SDL_CloseAudioDevice(capture_device);
        capture_device = 0;
    }
    if (camera_capture.isOpened()) {
        camera_capture.release();
    }
    if (camera_texture) {
        SDL_DestroyTexture(camera_texture);
        camera_texture = nullptr;
    }
    if (renderer_handle) {
        SDL_DestroyRenderer(renderer_handle);
        renderer_handle = nullptr;
    }
    if (window_handle) {
        SDL_DestroyWindow(window_handle);
        window_handle = nullptr;
    }
    SDL_Quit();
}

void process_visual_input() {
    if (!app_running) {
        return;
    }

    process_camera_input();
    process_audio_input();
    sync_visual_spawns();

    Uint32 now = SDL_GetTicks();
    float dt = min(0.05f, (now - last_sim_tick) / 1000.0f);
    last_sim_tick = now;
    update_visual_particles(dt);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            app_running = false;
            return;
        }

        if (event.type == SDL_KEYDOWN) {
            switch (event.key.keysym.sym) {
            case SDLK_1:
                rebuild_runtime_threads(THREAD_MODE_1);
                emit_event(CAMERA_OPEN, 1, "thread_mode_1");
                break;
            case SDLK_2:
                rebuild_runtime_threads(THREAD_MODE_2);
                emit_event(CAMERA_OPEN, 1, "thread_mode_2");
                break;
            case SDLK_3:
                rebuild_runtime_threads(THREAD_MODE_3);
                emit_event(CAMERA_OPEN, 1, "thread_mode_3");
                break;
            case SDLK_c:
                emit_event(CAMERA_OPEN, 1, "camera_key");
                break;
            case SDLK_m:
                emit_event(MIC_START, 1, "mic_key");
                break;
            case SDLK_n:
                emit_event(MIC_END, 0, "mic_stop_key");
                break;
            case SDLK_r:
                trigger_reset();
                break;
            case SDLK_b:
                use_camera_background = !use_camera_background;
                break;
            case SDLK_v:
                use_mouth_control = !use_mouth_control;
                break;
            case SDLK_ESCAPE:
                app_running = false;
                break;
            default:
                break;
            }
        }

        if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
            SDL_Point point = {event.button.x, event.button.y};
            for (const Button& button : buttons) {
                if (SDL_PointInRect(&point, &button.rect)) {
                    handle_button(button);
                    break;
                }
            }
        }
    }
}

void render_visual_frame() {
    if (!app_running || !renderer_handle) {
        return;
    }

    Uint32 now = SDL_GetTicks();
    if (now - last_frame_tick < 16) {
        return;
    }

    last_frame_tick = now;
    draw_scene();
}

bool visualization_running() {
    return app_running;
}
