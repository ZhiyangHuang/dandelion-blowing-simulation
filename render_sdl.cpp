#include "thread.h"
#include "scheduler_task_support.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>

namespace {

constexpr int kWindowWidth = 1360;
constexpr int kWindowHeight = 820;
constexpr int kWorldWidthPercent = 75;

constexpr SDL_Color kSkyTop{121, 196, 255, 255};
constexpr SDL_Color kSkyBottom{209, 236, 255, 255};
constexpr SDL_Color kGrassTop{103, 173, 98, 255};
constexpr SDL_Color kGrassBottom{62, 118, 67, 255};
constexpr SDL_Color kPanelColor{18, 24, 31, 240};
constexpr SDL_Color kPanelAlt{28, 37, 47, 255};
constexpr SDL_Color kPanelBorder{79, 98, 118, 255};
constexpr SDL_Color kTextColor{232, 238, 245, 255};
constexpr SDL_Color kMutedTextColor{142, 158, 176, 255};
constexpr SDL_Color kThreadRun{91, 232, 145, 255};
constexpr SDL_Color kThreadWait{255, 195, 88, 255};
constexpr SDL_Color kThreadSleep{118, 124, 136, 255};
constexpr SDL_Color kThreadIdle{96, 176, 255, 255};
constexpr SDL_Color kThreadClosed{72, 72, 72, 255};
constexpr SDL_Color kParticleAttached{245, 244, 233, 255};
constexpr SDL_Color kParticleFlying{255, 216, 109, 255};
constexpr SDL_Color kParticleCleaned{77, 87, 101, 255};
constexpr SDL_Color kParticleBoundary{255, 121, 121, 255};
constexpr SDL_Color kDandelionCore{237, 216, 109, 255};
constexpr SDL_Color kStemColor{57, 110, 63, 255};
constexpr SDL_Color kMouthClosed{255, 116, 125, 255};
constexpr SDL_Color kMouthOpen{101, 236, 154, 255};
constexpr SDL_Color kWindIdle{84, 113, 161, 255};
constexpr SDL_Color kWindActive{99, 224, 255, 255};
constexpr SDL_Color kButtonFill{46, 58, 73, 255};
constexpr SDL_Color kButtonFillActive{70, 108, 166, 255};
constexpr SDL_Color kButtonFillWarn{141, 97, 58, 255};

constexpr int kFontScale = 2;
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphAdvance = 6;

struct ButtonSpec {
    SDL_Rect rect{};
    std::string label;
    SDL_Keycode hotkey = SDLK_UNKNOWN;
    SDL_Color fill = kButtonFill;
};

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
bool g_running = false;
int g_window_width = kWindowWidth;
int g_window_height = kWindowHeight;
std::string g_last_window_title;
std::vector<ButtonSpec> g_buttons;

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

int world_width() {
    return g_window_width * kWorldWidthPercent / 100;
}

int scale_world_x(float normalized_x) {
    return static_cast<int>(clamp01(normalized_x) * static_cast<float>(world_width()));
}

int scale_world_y(float normalized_y) {
    return static_cast<int>(clamp01(normalized_y) * static_cast<float>(g_window_height));
}

SDL_Rect make_rect(int x, int y, int w, int h) {
    SDL_Rect rect{};
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    return rect;
}

void set_color(SDL_Color color) {
    SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
}

void fill_rect(const SDL_Rect& rect, SDL_Color color) {
    set_color(color);
    SDL_RenderFillRect(g_renderer, &rect);
}

void stroke_rect(const SDL_Rect& rect, SDL_Color color) {
    set_color(color);
    SDL_RenderDrawRect(g_renderer, &rect);
}

void draw_line(int x1, int y1, int x2, int y2, SDL_Color color) {
    set_color(color);
    SDL_RenderDrawLine(g_renderer, x1, y1, x2, y2);
}

void fill_circle(int center_x, int center_y, int radius, SDL_Color color) {
    set_color(color);
    for (int dy = -radius; dy <= radius; ++dy) {
        const int dx = static_cast<int>(std::sqrt(
            std::max(0.0, static_cast<double>(radius * radius - dy * dy))));
        SDL_RenderDrawLine(g_renderer, center_x - dx, center_y + dy, center_x + dx, center_y + dy);
    }
}

std::array<const char*, 7> glyph_rows(char c) {
    switch (std::toupper(static_cast<unsigned char>(c))) {
    case 'A': return {"01110","10001","10001","11111","10001","10001","10001"};
    case 'B': return {"11110","10001","10001","11110","10001","10001","11110"};
    case 'C': return {"01111","10000","10000","10000","10000","10000","01111"};
    case 'D': return {"11110","10001","10001","10001","10001","10001","11110"};
    case 'E': return {"11111","10000","10000","11110","10000","10000","11111"};
    case 'F': return {"11111","10000","10000","11110","10000","10000","10000"};
    case 'G': return {"01111","10000","10000","10111","10001","10001","01111"};
    case 'H': return {"10001","10001","10001","11111","10001","10001","10001"};
    case 'I': return {"11111","00100","00100","00100","00100","00100","11111"};
    case 'J': return {"00111","00010","00010","00010","10010","10010","01100"};
    case 'K': return {"10001","10010","10100","11000","10100","10010","10001"};
    case 'L': return {"10000","10000","10000","10000","10000","10000","11111"};
    case 'M': return {"10001","11011","10101","10101","10001","10001","10001"};
    case 'N': return {"10001","10001","11001","10101","10011","10001","10001"};
    case 'O': return {"01110","10001","10001","10001","10001","10001","01110"};
    case 'P': return {"11110","10001","10001","11110","10000","10000","10000"};
    case 'Q': return {"01110","10001","10001","10001","10101","10010","01101"};
    case 'R': return {"11110","10001","10001","11110","10100","10010","10001"};
    case 'S': return {"01111","10000","10000","01110","00001","00001","11110"};
    case 'T': return {"11111","00100","00100","00100","00100","00100","00100"};
    case 'U': return {"10001","10001","10001","10001","10001","10001","01110"};
    case 'V': return {"10001","10001","10001","10001","10001","01010","00100"};
    case 'W': return {"10001","10001","10001","10101","10101","10101","01010"};
    case 'X': return {"10001","10001","01010","00100","01010","10001","10001"};
    case 'Y': return {"10001","10001","01010","00100","00100","00100","00100"};
    case 'Z': return {"11111","00001","00010","00100","01000","10000","11111"};
    case '0': return {"01110","10001","10011","10101","11001","10001","01110"};
    case '1': return {"00100","01100","00100","00100","00100","00100","01110"};
    case '2': return {"01110","10001","00001","00010","00100","01000","11111"};
    case '3': return {"11110","00001","00001","01110","00001","00001","11110"};
    case '4': return {"00010","00110","01010","10010","11111","00010","00010"};
    case '5': return {"11111","10000","10000","11110","00001","00001","11110"};
    case '6': return {"01110","10000","10000","11110","10001","10001","01110"};
    case '7': return {"11111","00001","00010","00100","01000","01000","01000"};
    case '8': return {"01110","10001","10001","01110","10001","10001","01110"};
    case '9': return {"01110","10001","10001","01111","00001","00001","01110"};
    case ':': return {"00000","00100","00100","00000","00100","00100","00000"};
    case '.': return {"00000","00000","00000","00000","00000","00110","00110"};
    case '-': return {"00000","00000","00000","11111","00000","00000","00000"};
    case '+': return {"00000","00100","00100","11111","00100","00100","00000"};
    case '/': return {"00001","00010","00100","00100","01000","10000","00000"};
    case '>': return {"10000","01000","00100","00010","00100","01000","10000"};
    case ' ': return {"00000","00000","00000","00000","00000","00000","00000"};
    default: return {"00000","00000","01110","00100","00100","00000","00100"};
    }
}

void draw_text(int x, int y, const std::string& text, SDL_Color color, int scale = kFontScale) {
    set_color(color);
    int cursor_x = x;
    for (char c : text) {
        const auto rows = glyph_rows(c);
        for (int row = 0; row < kGlyphHeight; ++row) {
            for (int col = 0; col < kGlyphWidth; ++col) {
                if (rows[static_cast<std::size_t>(row)][col] != '1') {
                    continue;
                }
                SDL_Rect pixel = make_rect(
                    cursor_x + col * scale,
                    y + row * scale,
                    scale,
                    scale);
                SDL_RenderFillRect(g_renderer, &pixel);
            }
        }
        cursor_x += kGlyphAdvance * scale;
    }
}

int text_width(const std::string& text, int scale = kFontScale) {
    return static_cast<int>(text.size()) * kGlyphAdvance * scale;
}

SDL_Color thread_state_color(ThreadState state) {
    switch (state) {
    case ThreadState::RUNNING:
        return kThreadRun;
    case ThreadState::WAITING:
        return kThreadWait;
    case ThreadState::SLEEPING:
        return kThreadSleep;
    case ThreadState::CLOSED:
        return kThreadClosed;
    case ThreadState::IDLE:
    default:
        return kThreadIdle;
    }
}

SDL_Color particle_color(const ParticleRenderData& particle) {
    const Uint8 alpha = static_cast<Uint8>(
        std::clamp(particle.visual_alpha, 0.0f, 1.0f) * 255.0f);
    if (particle.status == "particle-cleaned") {
        return SDL_Color{kParticleCleaned.r, kParticleCleaned.g, kParticleCleaned.b, alpha};
    }
    if (particle.status == "particle-boundary-stop" ||
        particle.status == "particle-fade-pending" ||
        particle.status == "particle-fading" ||
        particle.status == "particle-fade-queued" ||
        particle.status == "particle-fade-execute" ||
        particle.status == "particle-fade-resume") {
        return SDL_Color{kParticleBoundary.r, kParticleBoundary.g, kParticleBoundary.b, alpha};
    }
    if (!particle.attached) {
        return SDL_Color{kParticleFlying.r, kParticleFlying.g, kParticleFlying.b, alpha};
    }
    return SDL_Color{kParticleAttached.r, kParticleAttached.g, kParticleAttached.b, alpha};
}

bool is_resident_p2_type(TaskType type) {
    switch (type) {
    case TaskType::CAMERA:
    case TaskType::MICROPHONE:
    case TaskType::BATCH_PARTICLE_EXECUTION:
        return true;
    default:
        return false;
    }
}

std::string crop_text(const std::string& text, int max_chars) {
    if (max_chars <= 0 || static_cast<int>(text.size()) <= max_chars) {
        return text;
    }
    if (max_chars <= 3) {
        return text.substr(0, static_cast<std::size_t>(max_chars));
    }
    return text.substr(0, static_cast<std::size_t>(max_chars - 3)) + "...";
}

const TaskRecord* next_queue_record(const std::vector<TaskRecord>& queue,
                                    PriorityLevel priority) {
    if (queue.empty()) {
        return nullptr;
    }
    if (priority == PriorityLevel::P1_SYSTEM) {
        return &queue.back();
    }
    return &queue.front();
}

const TaskRecord* next_queue_record_for_type(const std::vector<TaskRecord>& queue,
                                             TaskType type) {
    for (const TaskRecord& record : queue) {
        if (record.type == type) {
            return &record;
        }
    }
    return nullptr;
}

std::string task_semantic_summary(TaskType type, PriorityLevel priority) {
    return std::string(scheduler_queue_label(priority)) +
        " / " + latency_class_label_for_task(type, priority) +
        " / " + task_tree_node_label_for_task(type);
}

std::string queue_record_summary(const TaskRecord& record) {
    return task_semantic_summary(record.type, record.priority) +
        " #" + std::to_string(record.id);
}

std::string orchestration_headline(const OrchestrationRecord& record) {
    return record.name + std::string(record.active ? " ACTIVE" : " IDLE") +
        " / " + orchestration_node_label(record.current_node);
}

std::string orchestration_detail(const OrchestrationRecord& record) {
    std::string leaf = record.current_leaf_id >= 0
        ? (std::string(task_tree_node_label_for_task(record.current_leaf_type)) +
           " #" + std::to_string(record.current_leaf_id))
        : "idle";
    return std::string("ST ") + orchestration_status_label(record.current_status) +
        " / EV " + orchestration_event_label(record.last_event) +
        " / LEAF " + leaf;
}

std::string thread_role_label(const RuntimeThread& thread) {
    if (!thread.is_pinned) {
        return "ROLE shared-lane";
    }
    if (thread.pinned_task_type == TaskType::CAMERA) {
        return "ROLE camera-lane";
    }
    if (thread.pinned_task_type == TaskType::MICROPHONE) {
        return "ROLE microphone-lane";
    }
    return "ROLE general-lane";
}

std::string thread_active_semantic_line(const RuntimeThread& thread) {
    if (thread.bound_task_id < 0 || thread.bound_task_type == TaskType::NONE) {
        return "NODE idle";
    }
    return std::string(latency_class_label_for_task(
        thread.bound_task_type,
        thread.bound_task_priority)) +
        " / " + task_tree_node_label_for_task(thread.bound_task_type);
}

std::string thread_last_semantic_line(const RuntimeThread& thread) {
    if (thread.last_completed_task_id < 0 ||
        thread.last_completed_task_type == TaskType::NONE) {
        return "LAST idle";
    }
    return std::string("LAST ") +
        latency_class_label_for_task(
            thread.last_completed_task_type,
            thread.last_completed_task_priority) +
        " / " + task_tree_node_label_for_task(thread.last_completed_task_type);
}

std::string thread_state_label(ThreadState state) {
    switch (state) {
    case ThreadState::RUNNING:
        return "RUNNING";
    case ThreadState::WAITING:
        return "WAITING";
    case ThreadState::SLEEPING:
        return "SLEEPING";
    case ThreadState::CLOSED:
        return "CLOSED";
    case ThreadState::IDLE:
    default:
        return "IDLE";
    }
}

std::string microphone_mode_label(const RuntimeState& state) {
    if (state.microphone_bridge.voice_detected) {
        return "VOICE";
    }
    if (state.microphone_bridge.fallback_requested) {
        return "BREATH";
    }
    return "SILENT";
}

std::string mouth_label(const RuntimeState& state) {
    if (!state.camera_bridge.face_detected) {
        return "NO FACE";
    }
    return state.camera_bridge.mouth_open_state ? "OPEN" : "CLOSED";
}

SDL_Color focus_color(const RenderData& data) {
    if (data.ui.input_focus_status == "CAMERA FOCUS" ||
        data.ui.input_focus_status == "CAMERA WINDOW") {
        return kThreadIdle;
    }
    if (data.ui.input_focus_status == "MIC FOCUS" ||
        data.ui.input_focus_status == "MIC WINDOW") {
        return kWindActive;
    }
    if (data.ui.input_focus_status == "GATE OPEN") {
        return kMouthOpen;
    }
    return kMutedTextColor;
}

SDL_Color listener_lane_color(bool enabled,
                              bool unavailable,
                              bool sample_ready,
                              bool stale,
                              bool active_signal) {
    if (!enabled) {
        return kMutedTextColor;
    }
    if (unavailable) {
        return kParticleBoundary;
    }
    if (active_signal) {
        return kMouthOpen;
    }
    if (sample_ready && !stale) {
        return kWindActive;
    }
    if (sample_ready && stale) {
        return kThreadWait;
    }
    return kThreadIdle;
}

std::string camera_listener_lane_detail(const RuntimeState& state) {
    const CameraListenerState& listener = state.camera_listener;
    std::string device = listener.unavailable
        ? "device-down"
        : (listener.device_available ? "device-up" : "device-wait");
    std::string sample = listener.sample_ready
        ? (listener.stale ? "sample-stale" : "sample-hot")
        : "sample-empty";
    std::string stage = state.camera_available
        ? "mouth-ready"
        : (state.camera_device_available ? "scan-mouth" : "boot");
    return std::string(listener.bridge_running ? "bridge-on" : "bridge-off") +
        " / " + device + " / " + sample + " / " + stage;
}

std::string microphone_listener_lane_detail(const RuntimeState& state) {
    const MicrophoneListenerState& listener = state.microphone_listener;
    std::string device = listener.unavailable
        ? "device-down"
        : (listener.device_available ? "device-up" : "device-wait");
    std::string sample = listener.sample_ready
        ? (listener.stale ? "sample-stale" : "sample-hot")
        : "sample-empty";
    std::string mode = state.microphone_bridge.voice_detected
        ? "voice-live"
        : (state.microphone_bridge.fallback_requested ? "breath-live" : "listen");
    return std::string(listener.bridge_running ? "bridge-on" : "bridge-off") +
        " / " + device + " / " + sample + " / " + mode;
}

std::string listener_service_status_line(const ListenerServiceDiagnostics& diagnostics) {
    std::string line = std::string("SRV ") +
        (diagnostics.service_task_alive ? "alive" : "absent") +
        " #" + std::to_string(diagnostics.service_task_id) +
        " / L2 " + (diagnostics.consumer_task_queued ? "queued" : "idle") +
        " #" + std::to_string(diagnostics.consumer_task_id);
    if (diagnostics.short_lease_active) {
        const long long now_ms = scheduler_task_support::current_time_ms();
        const long long remaining_ms =
            diagnostics.short_lease_until_ms > now_ms
                ? diagnostics.short_lease_until_ms - now_ms
                : 0;
        const long long detect_ready_ms =
            diagnostics.short_detect_ready_at_ms > now_ms
                ? diagnostics.short_detect_ready_at_ms - now_ms
                : 0;
        line += " / SHORT " + std::to_string(remaining_ms) + "ms";
        line += " / READY " + std::to_string(detect_ready_ms) + "ms";
    }
    return line;
}

std::string listener_sample_line(const ListenerServiceDiagnostics& diagnostics) {
    return "HB " + std::to_string(diagnostics.last_heartbeat_ms) +
        " / SEEN " + std::to_string(diagnostics.last_seen_sample_ms) +
        " / SEED " + std::to_string(diagnostics.last_seeded_sample_ms) +
        " / USED " + std::to_string(diagnostics.last_consumed_sample_ms);
}

void submit_visual_hotkey_task(SDL_Keycode key) {
    switch (key) {
    case SDLK_q:
    case SDLK_ESCAPE:
        if (!has_queued_task(TaskType::EXIT_APP, PriorityLevel::P1_SYSTEM) &&
            !shutdown_requested()) {
            submit_task(make_exit_task());
        }
        break;
    case SDLK_r:
        if (!has_queued_task(TaskType::RESET, PriorityLevel::P1_SYSTEM)) {
            submit_task(make_reset_task());
        }
        break;
    case SDLK_c:
        if (!has_queued_task(TaskType::CHANGE_DANDELION, PriorityLevel::P2_FUNCTIONAL)) {
            submit_task(make_change_dandelion_task());
        }
        break;
    case SDLK_x:
        if (!has_queued_task(TaskType::BREEZE, PriorityLevel::P2_FUNCTIONAL)) {
            submit_task(make_breeze_task());
        }
        break;
    case SDLK_o:
        if (camera_demo_fallback_enabled()) {
            trigger_camera_demo_pulse();
            push_runtime_note("Demo camera fallback: injected mouth-open pulse.");
        }
        break;
    default:
        break;
    }
}

void rebuild_buttons() {
    g_buttons.clear();
    const int panel_x = world_width();
    const int panel_width = g_window_width - panel_x;
    const int padding = 16;
    const int button_gap = 10;
    const int button_width = (panel_width - padding * 2 - button_gap) / 2;
    const int button_height = 42;
    int row_y = g_window_height - 4 * (button_height + 8);

    auto push_button = [&](int col,
                           const std::string& label,
                           SDL_Keycode key,
                           SDL_Color fill) {
        const int x = panel_x + padding + col * (button_width + button_gap);
        g_buttons.push_back({make_rect(x, row_y, button_width, button_height), label, key, fill});
    };

    push_button(0, "RESET", SDLK_r, kButtonFillWarn);
    push_button(1, "CHANGE", SDLK_c, kButtonFillWarn);
    row_y += button_height + 8;
    push_button(0, "BREEZE", SDLK_x, kButtonFillActive);
    push_button(1, "QUIT", SDLK_q, kButtonFillWarn);
    row_y += button_height + 8;
    push_button(0, "MOUTH", SDLK_o, kButtonFillActive);
}

void handle_sdl_events() {
    SDL_Event event{};
    while (SDL_PollEvent(&event) == 1) {
        if (event.type == SDL_QUIT) {
            submit_visual_hotkey_task(SDLK_q);
            continue;
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            g_window_width = std::max(event.window.data1, 760);
            g_window_height = std::max(event.window.data2, 560);
            rebuild_buttons();
            continue;
        }
        if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
            submit_visual_hotkey_task(event.key.keysym.sym);
            continue;
        }
        if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
            const int mouse_x = event.button.x;
            const int mouse_y = event.button.y;
            for (const ButtonSpec& button : g_buttons) {
                if (mouse_x >= button.rect.x &&
                    mouse_x < button.rect.x + button.rect.w &&
                    mouse_y >= button.rect.y &&
                    mouse_y < button.rect.y + button.rect.h) {
                    submit_visual_hotkey_task(button.hotkey);
                    break;
                }
            }
        }
    }
}

void render_background() {
    for (int y = 0; y < g_window_height; ++y) {
        const float t = static_cast<float>(y) / std::max(1, g_window_height - 1);
        SDL_Color sky{
            static_cast<Uint8>(kSkyTop.r + (kSkyBottom.r - kSkyTop.r) * t),
            static_cast<Uint8>(kSkyTop.g + (kSkyBottom.g - kSkyTop.g) * t),
            static_cast<Uint8>(kSkyTop.b + (kSkyBottom.b - kSkyTop.b) * t),
            255};
        draw_line(0, y, world_width(), y, sky);
    }

    const int grass_top = static_cast<int>(g_window_height * 0.74f);
    for (int y = grass_top; y < g_window_height; ++y) {
        const float t = static_cast<float>(y - grass_top) / std::max(1, g_window_height - grass_top - 1);
        SDL_Color grass{
            static_cast<Uint8>(kGrassTop.r + (kGrassBottom.r - kGrassTop.r) * t),
            static_cast<Uint8>(kGrassTop.g + (kGrassBottom.g - kGrassTop.g) * t),
            static_cast<Uint8>(kGrassTop.b + (kGrassBottom.b - kGrassTop.b) * t),
            255};
        draw_line(0, y, g_window_width, y, grass);
    }
}

void render_camera_layer_card(const RuntimeState& state) {
    const SDL_Rect card = make_rect(22, 20, world_width() / 3, g_window_height / 3);
    fill_rect(card, SDL_Color{21, 29, 37, 214});
    stroke_rect(card, SDL_Color{88, 113, 138, 255});
    draw_text(card.x + 14, card.y + 14, "CAMERA LAYER", kTextColor);
    draw_text(card.x + 14, card.y + 40, state.camera_bridge.backend.empty()
        ? "NO BACKEND"
        : state.camera_bridge.backend.substr(0, std::min<std::size_t>(state.camera_bridge.backend.size(), 18)), kMutedTextColor);
    draw_text(card.x + 14, card.y + 56,
        state.camera_bridge.status_text.substr(0, std::min<std::size_t>(state.camera_bridge.status_text.size(), 24)),
        kMutedTextColor);

    const SDL_Rect viewport = make_rect(card.x + 14, card.y + 86, card.w - 28, card.h - 106);
    fill_rect(viewport, SDL_Color{34, 46, 58, 255});
    stroke_rect(viewport, SDL_Color{78, 103, 124, 255});

    if (!state.camera_bridge.face_detected) {
        draw_text(viewport.x + 18, viewport.y + viewport.h / 2 - 10, "NO FACE", kMutedTextColor);
        return;
    }

    const int mouth_x = viewport.x + static_cast<int>(clamp01(state.camera_bridge.mouth_center_x) * viewport.w);
    const int mouth_y = viewport.y + static_cast<int>(clamp01(state.camera_bridge.mouth_center_y) * viewport.h);
    const int box_half = 26;
    stroke_rect(make_rect(mouth_x - box_half, mouth_y - box_half, box_half * 2, box_half * 2), SDL_Color{180, 214, 236, 255});
    const SDL_Color mouth_color = state.camera_bridge.mouth_open_state ? kMouthOpen : kMouthClosed;
    draw_line(mouth_x - 18, mouth_y, mouth_x + 18, mouth_y, mouth_color);
    draw_line(mouth_x, mouth_y - 18, mouth_x, mouth_y + 18, mouth_color);
    fill_circle(mouth_x, mouth_y, state.camera_bridge.mouth_open_state ? 10 : 6, mouth_color);

    draw_text(viewport.x + 12, viewport.y + viewport.h - 30, mouth_label(state), mouth_color);
    if (state.camera_bridge.looking_forward) {
        draw_text(viewport.x + viewport.w - 94, viewport.y + viewport.h - 30, "FORWARD", kTextColor);
    }
}

void render_dandelion_world(const RuntimeState& state, const RenderData& data) {
    const int center_x = scale_world_x(state.world.dandelion_x);
    const int center_y = scale_world_y(state.world.dandelion_y);
    const int ground_y = static_cast<int>(g_window_height * 0.88f);

    draw_line(center_x, center_y + 14, center_x, ground_y, kStemColor);
    draw_line(center_x - 14, ground_y - 76, center_x, ground_y - 36, kStemColor);
    draw_line(center_x + 14, ground_y - 92, center_x, ground_y - 48, kStemColor);

    for (const ParticleRenderData& particle : data.particles) {
        const int particle_x = scale_world_x(particle.x);
        const int particle_y = scale_world_y(particle.y);
        const int radius = particle.attached ? 4 : 5;
        fill_circle(particle_x, particle_y, radius, particle_color(particle));
        if (particle.attached) {
            draw_line(particle_x, particle_y, center_x, center_y, SDL_Color{255, 255, 255, 96});
        }
        if (particle.active) {
            for (int i = -3; i <= 3; ++i) {
                draw_line(particle_x, particle_y, particle_x + 15, particle_y - 12 + i * 4, particle_color(particle));
            }
        }
    }

    fill_circle(center_x, center_y, 12, kDandelionCore);
    stroke_rect(make_rect(center_x - 18, center_y - 18, 36, 36), SDL_Color{255, 243, 170, 110});

    if (data.camera_layer.enabled || state.camera_bridge.sample_ready) {
        const int mouth_x = scale_world_x(data.camera_layer.mouth_x);
        const int mouth_y = scale_world_y(data.camera_layer.mouth_y);
        const SDL_Color mouth_color = state.camera_bridge.mouth_open_state ? kMouthOpen : kMouthClosed;
        fill_circle(mouth_x, mouth_y, state.camera_bridge.mouth_open_state ? 12 : 8, mouth_color);
    }

    if (data.wind_layer.active || state.microphone_bridge.fallback_requested) {
        const float wind_strength = std::clamp(data.wind_layer.power / 5.0f, 0.0f, 1.0f);
        const int start_x = world_width() - 180;
        const int start_y = static_cast<int>(g_window_height * 0.28f);
        const int arrow_len = 50 + static_cast<int>(wind_strength * 130.0f);
        const SDL_Color wind_color = data.wind_layer.active ? kWindActive : kWindIdle;
        for (int lane = 0; lane < 4; ++lane) {
            const int lane_y = start_y + lane * 24;
            draw_line(start_x, lane_y, start_x + arrow_len, lane_y, wind_color);
            draw_line(start_x + arrow_len - 12, lane_y - 6, start_x + arrow_len, lane_y, wind_color);
            draw_line(start_x + arrow_len - 12, lane_y + 6, start_x + arrow_len, lane_y, wind_color);
        }
    }
}

void render_status_panel(const RuntimeState& state,
                         const RenderData& data,
                         const SchedulerSnapshot& snapshot) {
    const int panel_x = world_width();
    const int panel_w = g_window_width - panel_x;
    const SDL_Rect panel = make_rect(panel_x, 0, panel_w, g_window_height);
    fill_rect(panel, kPanelColor);
    draw_line(panel_x, 0, panel_x, g_window_height, kPanelBorder);

    const int padding = 18;
    int y = 18;
    draw_text(panel_x + padding, y, "DANDELION OS", kTextColor, 3);
    y += 34;
    draw_text(panel_x + padding, y, "PHASE " + std::string(runtime_phase_label(state.phase)), kMutedTextColor);
    y += 28;

    draw_text(panel_x + padding, y, "QUEUES", kTextColor);
    y += 18;
    const SDL_Rect queue_bar = make_rect(panel_x + padding, y, panel_w - padding * 2, 18);
    fill_rect(queue_bar, SDL_Color{45, 55, 67, 255});
    const int total = static_cast<int>(
        snapshot.p1_queue.size() +
        snapshot.p2_realtime_queue.size() +
        snapshot.p2_queue.size() +
        snapshot.p3_queue.size());
    const int p1w = total == 0 ? 0 : queue_bar.w * static_cast<int>(snapshot.p1_queue.size()) / total;
    const int p2rtw = total == 0 ? 0 : queue_bar.w * static_cast<int>(snapshot.p2_realtime_queue.size()) / total;
    const int p2w = total == 0 ? 0 : queue_bar.w * static_cast<int>(snapshot.p2_queue.size()) / total;
    const int p3w = total == 0 ? 0 : queue_bar.w - p1w - p2rtw - p2w;
    fill_rect(make_rect(queue_bar.x, queue_bar.y, p1w, queue_bar.h), SDL_Color{249, 114, 114, 255});
    fill_rect(make_rect(queue_bar.x + p1w, queue_bar.y, p2rtw, queue_bar.h), SDL_Color{104, 221, 184, 255});
    fill_rect(make_rect(queue_bar.x + p1w + p2rtw, queue_bar.y, p2w, queue_bar.h), SDL_Color{109, 172, 255, 255});
    fill_rect(make_rect(queue_bar.x + p1w + p2rtw + p2w, queue_bar.y, p3w, queue_bar.h), SDL_Color{255, 207, 92, 255});
    y += 26;
    int resident_p2 = 0;
    int event_p2 = 0;
    for (const TaskRecord& record : snapshot.p2_queue) {
        if (is_resident_p2_type(record.type)) {
            resident_p2++;
        } else {
            event_p2++;
        }
    }
    draw_text(panel_x + padding, y, "P1 " + std::to_string(snapshot.p1_queue.size()), kMutedTextColor);
    draw_text(panel_x + padding + 90, y,
        "P2 " + std::to_string(snapshot.p2_realtime_queue.size()),
        kMutedTextColor);
    draw_text(panel_x + padding + 170, y,
        "P3 " + std::to_string(snapshot.p2_queue.size()) +
        " R" + std::to_string(resident_p2) +
        " E" + std::to_string(event_p2),
        kMutedTextColor);
    draw_text(
        panel_x + padding + 300,
        y,
        "P4 " + std::to_string(snapshot.p3_queue.size()) +
            " M" + std::to_string(snapshot.p3_move_queued + snapshot.p3_move_running) +
            " F" + std::to_string(snapshot.p3_fade_queued + snapshot.p3_fade_running),
        kMutedTextColor);
    y += 22;
    const int semantic_chars = std::max(12, (panel_w - padding * 2) / (kGlyphAdvance * kFontScale));
    draw_text(panel_x + padding, y, "MAP P1/L0  P2/L1  P3/L2  P4/L3", kMutedTextColor);
    y += 18;
    if (const TaskRecord* record = next_queue_record(snapshot.p1_queue, PriorityLevel::P1_SYSTEM)) {
        draw_text(panel_x + padding, y, crop_text("NEXT " + queue_record_summary(*record), semantic_chars), kMutedTextColor);
        y += 18;
    }
    if (const TaskRecord* record = next_queue_record(snapshot.p2_realtime_queue, PriorityLevel::P2_REALTIME)) {
        draw_text(panel_x + padding, y, crop_text("NEXT " + queue_record_summary(*record), semantic_chars), kMutedTextColor);
        y += 18;
    }
    if (const TaskRecord* record = next_queue_record(snapshot.p2_queue, PriorityLevel::P2_FUNCTIONAL)) {
        draw_text(panel_x + padding, y, crop_text("NEXT " + queue_record_summary(*record), semantic_chars), kMutedTextColor);
        y += 18;
    }
    if (const TaskRecord* record = next_queue_record(snapshot.p3_queue, PriorityLevel::P3_PARTICLE)) {
        draw_text(panel_x + padding, y, crop_text("NEXT " + queue_record_summary(*record), semantic_chars), kMutedTextColor);
        y += 18;
    }
    draw_text(
        panel_x + padding,
        y,
        crop_text(
            "P3 MOVE q" + std::to_string(snapshot.p3_move_queued) +
                " r" + std::to_string(snapshot.p3_move_running) +
                " / FADE q" + std::to_string(snapshot.p3_fade_queued) +
                " r" + std::to_string(snapshot.p3_fade_running),
            semantic_chars),
        kMutedTextColor);
    y += 18;
    if (const TaskRecord* record = next_queue_record_for_type(snapshot.p3_queue, TaskType::SINGLE_PARTICLE)) {
        draw_text(panel_x + padding, y, crop_text("MOVE " + queue_record_summary(*record), semantic_chars), kMutedTextColor);
        y += 18;
    }
    if (const TaskRecord* record = next_queue_record_for_type(snapshot.p3_queue, TaskType::FADE_PARTICLE)) {
        draw_text(panel_x + padding, y, crop_text("FADE " + queue_record_summary(*record), semantic_chars), kMutedTextColor);
        y += 18;
    }
    y += 16;

    draw_text(panel_x + padding, y, "L1 REALTIME LANES", kTextColor);
    y += 18;
    draw_text(panel_x + padding, y, crop_text("CAM listener-lane", semantic_chars), kTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(camera_listener_lane_detail(state), semantic_chars),
        listener_lane_color(
            state.camera_listener.enabled,
            state.camera_listener.unavailable,
            state.camera_listener.sample_ready,
            state.camera_listener.stale,
            state.camera_available));
    y += 20;
    draw_text(panel_x + padding, y, crop_text("MIC listener-lane", semantic_chars), kTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(microphone_listener_lane_detail(state), semantic_chars),
        listener_lane_color(
            state.microphone_listener.enabled,
            state.microphone_listener.unavailable,
            state.microphone_listener.sample_ready,
            state.microphone_listener.stale,
            state.microphone_available || state.microphone_bridge.voice_detected ||
                state.microphone_bridge.fallback_requested));
    y += 26;

    draw_text(panel_x + padding, y, "L1 REALTIME DIAGNOSTICS", kTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(snapshot.l1_camera_lane, semantic_chars),
        snapshot.l1_realtime_active ? kMutedTextColor : kThreadIdle);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(listener_service_status_line(snapshot.camera_listener_runtime), semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(listener_sample_line(snapshot.camera_listener_runtime), semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(snapshot.l1_microphone_lane, semantic_chars),
        snapshot.l1_realtime_active ? kMutedTextColor : kThreadIdle);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(listener_service_status_line(snapshot.microphone_listener_runtime), semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(listener_sample_line(snapshot.microphone_listener_runtime), semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(snapshot.l1_gate_lane, semantic_chars),
        snapshot.l1_realtime_active ? kMutedTextColor : kThreadIdle);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text("BLOCKED " + snapshot.l1_blocked_reason, semantic_chars),
        snapshot.l1_realtime_active ? kMutedTextColor : kThreadIdle);
    y += 26;

    draw_text(panel_x + padding, y, "INPUT FLOW", kTextColor);
    y += 18;
    draw_text(panel_x + padding, y, crop_text("FOCUS " + data.ui.input_focus_status, semantic_chars), focus_color(data));
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(
            "MOUTH " + mouth_label(state) + " / MIC " + microphone_mode_label(state),
            semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(
            "GATE " + std::string(state.camera_gate_open ? "OPEN" : "CLOSED") +
                " / PWR " +
                std::to_string(static_cast<int>(data.ui.power * 100.0f) / 100.0f),
            semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text("SCHED " + data.ui.scheduler_state, semantic_chars),
        kMutedTextColor);
    y += 26;

    draw_text(panel_x + padding, y, "RUNTIME COUNTERS", kTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(
            "RESEED CAM " + std::to_string(snapshot.counters.camera_reseed_count) +
                " / MIC " + std::to_string(snapshot.counters.microphone_reseed_count),
            semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(
            "WATCHDOG " + std::to_string(snapshot.counters.watchdog_recovery_count) +
                " / RT SLICE " +
                std::to_string(snapshot.counters.realtime_slices_last_frame),
            semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(
            "RT MAX " + std::to_string(snapshot.counters.max_realtime_slices_per_frame) +
                " / DRAIN " +
                std::to_string(snapshot.counters.particle_drain_cycles_completed),
            semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(
            "AVG DRAIN " +
                std::to_string(snapshot.counters.average_particle_drain_ticks),
            semantic_chars),
        kMutedTextColor);
    y += 26;

    draw_text(panel_x + padding, y, "SINGLE-THREAD CHAIN", kTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(snapshot.single_thread_chain_path, semantic_chars),
        snapshot.single_thread_chain_active ? kTextColor : kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text("CUR " + snapshot.single_thread_chain_current, semantic_chars),
        snapshot.single_thread_chain_active ? kTextColor : kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text("NXT " + snapshot.single_thread_chain_next, semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text("ST  " + snapshot.single_thread_chain_status, semantic_chars),
        kMutedTextColor);
    y += 26;

    draw_text(panel_x + padding, y, "PARENT FLOW RECORDS", kTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(orchestration_headline(snapshot.human_behavior_flow), semantic_chars),
        snapshot.human_behavior_flow.active ? kTextColor : kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(orchestration_detail(snapshot.human_behavior_flow), semantic_chars),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(orchestration_headline(snapshot.particle_root_flow), semantic_chars),
        snapshot.particle_root_flow.active ? kTextColor : kMutedTextColor);
    y += 18;
    draw_text(
        panel_x + padding,
        y,
        crop_text(orchestration_detail(snapshot.particle_root_flow), semantic_chars),
        kMutedTextColor);
    y += 24;

    draw_text(panel_x + padding, y, "THREADS", kTextColor);
    y += 22;
    for (std::size_t index = 0; index < snapshot.threads.size(); ++index) {
        const RuntimeThread& thread = snapshot.threads[index];
        const SDL_Rect row = make_rect(panel_x + padding, y, panel_w - padding * 2, 64);
        const int thread_line_width =
            std::max(8, (row.w - 48) / (kGlyphAdvance * kFontScale));
        fill_rect(row, kPanelAlt);
        stroke_rect(row, thread_state_color(thread.state));
        fill_rect(make_rect(row.x + 6, row.y + 6, 24, row.h - 12), thread_state_color(thread.state));
        draw_text(
            row.x + 40,
            row.y + 8,
            crop_text(
                "T" + std::to_string(thread.id) + " " +
                    thread_state_label(thread.state) + " " +
                    thread_role_label(thread),
                thread_line_width),
            kTextColor);
        draw_text(row.x + 40, row.y + 24,
            crop_text(thread_active_semantic_line(thread), thread_line_width),
            kMutedTextColor);
        draw_text(row.x + 40, row.y + 40,
            crop_text(
                "EV " + thread.last_task_event + " / " +
                    thread_last_semantic_line(thread),
                thread_line_width),
            kMutedTextColor);
        y += 74;
    }
    y -=280;
    const int panel_left = panel_x + padding;
    draw_text(panel_left, y, "PARTICLES", kTextColor);
    y += 20;
    draw_text(panel_left, y, "REMAIN " + std::to_string(data.ui.remaining_particles), kMutedTextColor);
    y += 18;
    draw_text(panel_left, y, "QUEUED " + std::to_string(data.ui.queued_particle_tasks), kMutedTextColor);
    y += 18;
    draw_text(
        panel_left,
        y,
        "MOVE q" + std::to_string(snapshot.p3_move_queued) +
            " r" + std::to_string(snapshot.p3_move_running),
        kMutedTextColor);
    y += 18;
    draw_text(
        panel_left,
        y,
        "FADE q" + std::to_string(snapshot.p3_fade_queued) +
            " r" + std::to_string(snapshot.p3_fade_running),
        kMutedTextColor);
    y += 18;
    const SDL_Rect remain_bar = make_rect(panel_left, y, panel_w - padding * 2, 16);
    fill_rect(remain_bar, SDL_Color{45, 55, 67, 255});
    const int total_particles = std::max(1, static_cast<int>(data.particles.size()));
    fill_rect(
        make_rect(remain_bar.x, remain_bar.y, remain_bar.w * data.ui.remaining_particles / total_particles, remain_bar.h),
        kParticleAttached);
    y += 24;
    const SDL_Rect queued_bar = make_rect(panel_left, y, panel_w - padding * 2, 16);
    fill_rect(queued_bar, SDL_Color{45, 55, 67, 255});
    fill_rect(
        make_rect(queued_bar.x, queued_bar.y, std::min(queued_bar.w, data.ui.queued_particle_tasks * 18), queued_bar.h),
        kParticleFlying);
    y += 32;

    draw_text(panel_left, y, "LEAF TASK STATUS", kTextColor);
    y += 20;
    const int task_line_width = std::max(0, (panel_w - padding * 2) / (kGlyphAdvance * kFontScale));
    draw_text(panel_left, y, crop_text("FOC " + data.ui.input_focus_status, task_line_width), focus_color(data));
    y += 18;
    draw_text(panel_left, y, crop_text("CAM " + data.ui.camera_task_status, task_line_width), kMutedTextColor);
    y += 18;
    draw_text(panel_left, y, crop_text("MIC " + data.ui.microphone_task_status, task_line_width), kMutedTextColor);
    y += 18;
    draw_text(panel_left, y, crop_text("GEN " + data.ui.generate_task_status, task_line_width), kMutedTextColor);
    y += 18;
    draw_text(panel_left, y, crop_text("BAT " + data.ui.batch_task_status, task_line_width), kMutedTextColor);
    y += 18;
    draw_text(panel_left, y, crop_text("P3  " + data.ui.particle_task_status, task_line_width), kMutedTextColor);
    y += 28;

    draw_text(panel_left, y, "EVENT LOG", kTextColor);
    y += 20;
    const std::vector<std::string>& notes = current_runtime_notes();
    const std::size_t visible_notes = std::min<std::size_t>(notes.size(), 10);
    for (std::size_t index = 0; index < visible_notes; ++index) {
        const std::size_t note_index = notes.size() - visible_notes + index;
        draw_text(
            panel_left,
            y,
            crop_text("> " + notes[note_index], task_line_width),
            index + 1 == visible_notes ? kTextColor : kMutedTextColor);
        y += 18;
    }
}

void render_buttons() {
    for (const ButtonSpec& button : g_buttons) {
        fill_rect(button.rect, button.fill);
        stroke_rect(button.rect, kPanelBorder);
        const int label_x = button.rect.x + (button.rect.w - text_width(button.label)) / 2;
        const int label_y = button.rect.y + (button.rect.h - kGlyphHeight * kFontScale) / 2;
        draw_text(label_x, label_y, button.label, kTextColor);
    }

    if (!g_buttons.empty()) {
        draw_text(g_buttons.front().rect.x, g_buttons.front().rect.y - 24, "CONTROLS", kTextColor);
    }
}

void update_window_title(const RuntimeState& state,
                         const RenderData& data,
                         const SchedulerSnapshot& snapshot) {
    const RuntimeThread primary_thread = snapshot.threads.empty()
        ? RuntimeThread{}
        : snapshot.threads.front();
    std::string title =
        "DandelionOS | " +
        std::string(runtime_phase_label(state.phase)) +
        " | FACE " + (state.camera_bridge.face_detected ? std::string("YES") : std::string("NO")) +
        " | MOUTH " + mouth_label(state) +
        " | MIC " + microphone_mode_label(state) +
        " | PWR " + std::to_string(static_cast<int>(data.ui.power * 100.0f) / 100.0f) +
        " | P1 " + std::to_string(snapshot.p1_queue.size()) +
        " P2 " + std::to_string(snapshot.p2_realtime_queue.size()) +
        " P3 " + std::to_string(snapshot.p2_queue.size()) +
        " P4 " + std::to_string(snapshot.p3_queue.size()) +
        " M" + std::to_string(snapshot.p3_move_queued + snapshot.p3_move_running) +
        " F" + std::to_string(snapshot.p3_fade_queued + snapshot.p3_fade_running) +
        " | T1 " + primary_thread.last_task_event +
        " " + latency_class_label_for_task(
            primary_thread.last_completed_task_type,
            primary_thread.last_completed_task_priority);
    if (title != g_last_window_title) {
        SDL_SetWindowTitle(g_window, title.c_str());
        g_last_window_title = title;
    }
}

}  // namespace

bool init_visualization() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        push_runtime_note(std::string("SDL init failed: ") + SDL_GetError());
        return false;
    }

    g_window = SDL_CreateWindow(
        "DandelionOS",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        kWindowWidth,
        kWindowHeight,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (g_window == nullptr) {
        push_runtime_note(std::string("SDL window creation failed: ") + SDL_GetError());
        SDL_Quit();
        return false;
    }

    g_renderer = SDL_CreateRenderer(
        g_window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (g_renderer == nullptr) {
        push_runtime_note(std::string("SDL renderer creation failed: ") + SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        SDL_Quit();
        return false;
    }

    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    g_running = true;
    g_window_width = kWindowWidth;
    g_window_height = kWindowHeight;
    g_last_window_title.clear();
    rebuild_buttons();
    set_visualization_running(true);
    push_runtime_note("SDL visualization initialized.");
    return true;
}

void shutdown_visualization() {
    g_running = false;
    set_visualization_running(false);

    if (g_renderer != nullptr) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = nullptr;
    }
    if (g_window != nullptr) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    SDL_Quit();
    push_runtime_note("SDL visualization shut down.");
}

void process_visual_input() {
    if (!g_running) {
        return;
    }
    handle_sdl_events();
    refresh_bridge_inputs();
}

void render_visual_frame() {
    if (!g_running || g_renderer == nullptr) {
        return;
    }

    const RuntimeState& state = current_runtime_state();
    const RenderData& data = current_render_data();
    const SchedulerSnapshot snapshot = scheduler_snapshot();

    render_background();
    render_camera_layer_card(state);
    render_status_panel(state, data, snapshot);
    render_dandelion_world(state, data);
    render_buttons();
    update_window_title(state, data, snapshot);
    SDL_RenderPresent(g_renderer);
}

bool visualization_running() {
    return g_running;
}
