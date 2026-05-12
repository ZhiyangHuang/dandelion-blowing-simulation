#include "thread.h"
#include "scheduler_task_support.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

struct CheckResult {
    std::string name;
    bool passed = false;
    std::vector<std::string> details;
};

std::string task_type_name(TaskType type) {
    switch (type) {
    case TaskType::CAMERA_LISTENER_SERVICE:
        return "CAMERA_LISTENER_SERVICE";
    case TaskType::MICROPHONE_LISTENER_SERVICE:
        return "MICROPHONE_LISTENER_SERVICE";
    case TaskType::CAMERA_LISTENER_BURST:
        return "CAMERA_LISTENER_BURST";
    case TaskType::MICROPHONE_LISTENER_BURST:
        return "MICROPHONE_LISTENER_BURST";
    case TaskType::CAMERA:
        return "CAMERA";
    case TaskType::MICROPHONE:
        return "MICROPHONE";
    case TaskType::GENERATE_PARTICLE:
        return "GENERATE_PARTICLE";
    case TaskType::BREEZE:
        return "BREEZE";
    case TaskType::CHANGE_DANDELION:
        return "CHANGE_DANDELION";
    case TaskType::BATCH_PARTICLE_EXECUTION:
        return "BATCH_PARTICLE_EXECUTION";
    case TaskType::SINGLE_PARTICLE:
        return "SINGLE_PARTICLE";
    case TaskType::FADE_PARTICLE:
        return "FADE_PARTICLE";
    case TaskType::NONE:
        return "NONE";
    default:
        return "OTHER";
    }
}

std::string listener_service_status_text(const ListenerServiceDiagnostics& diagnostics) {
    return std::string("SRV ") +
        (diagnostics.service_task_alive ? "alive" : "absent") +
        " #" + std::to_string(diagnostics.service_task_id) +
        " / L2 " + (diagnostics.consumer_task_queued ? "queued" : "idle") +
        " #" + std::to_string(diagnostics.consumer_task_id);
}

std::string listener_sample_text(const ListenerServiceDiagnostics& diagnostics) {
    return "SEEN " + std::to_string(diagnostics.last_seen_sample_ms) +
        " / SEED " + std::to_string(diagnostics.last_seeded_sample_ms) +
        " / USED " + std::to_string(diagnostics.last_consumed_sample_ms);
}

void prepare_ready_runtime(bool enable_camera_listener, bool enable_microphone_listener) {
    bootstrap_runtime();
    set_runtime_phase(RuntimePhase::READY);
    RuntimeState& state = runtime_state();
    state.camera_listener.enabled = enable_camera_listener;
    state.camera_listener.bridge_running = enable_camera_listener;
    state.camera_listener.device_available = enable_camera_listener;
    state.microphone_listener.enabled = enable_microphone_listener;
    state.microphone_listener.bridge_running = enable_microphone_listener;
    state.microphone_listener.device_available = enable_microphone_listener;
    render_data().ui.thread_mode = current_thread_mode();
    drain_runtime_notes();
}

bool thread_completed_type(const RuntimeThread& thread, TaskType expected) {
    return thread.last_completed_task_type == expected;
}

bool snapshot_has_task_type(const std::vector<TaskRecord>& queue, TaskType type) {
    return std::any_of(
        queue.begin(),
        queue.end(),
        [type](const TaskRecord& record) {
            return record.type == type;
        });
}

void write_microphone_bridge_fixture(long long timestamp_ms,
                                     float suggested_power,
                                     bool voice_detected,
                                     bool fallback_requested) {
    std::ofstream out("microphone_bridge_latest.json", std::ios::binary);
    out
        << "{"
        << "\"bridge_connected\":true,"
        << "\"sample_ready\":true,"
        << "\"device_unavailable\":false,"
        << "\"voice_detected\":" << (voice_detected ? "true" : "false") << ","
        << "\"fallback_requested\":" << (fallback_requested ? "true" : "false") << ","
        << "\"suggested_power\":" << suggested_power << ","
        << "\"direction_x\":0.0,"
        << "\"direction_y\":-1.0,"
        << "\"confidence\":0.8,"
        << "\"timestamp_ms\":" << timestamp_ms << ","
        << "\"backend\":\"queue-verifier\","
        << "\"status_text\":\"queue verifier microphone sample\""
        << "}";
}

void write_camera_bridge_fixture(long long timestamp_ms,
                                 bool mouth_open_state,
                                 bool looking_forward) {
    std::ofstream out("camera_bridge_latest.json", std::ios::binary);
    out
        << "{"
        << "\"bridge_connected\":true,"
        << "\"sample_ready\":true,"
        << "\"device_unavailable\":false,"
        << "\"face_detected\":true,"
        << "\"mouth_open_state\":" << (mouth_open_state ? "true" : "false") << ","
        << "\"looking_forward\":" << (looking_forward ? "true" : "false") << ","
        << "\"mouth_center_x\":0.52,"
        << "\"mouth_center_y\":0.37,"
        << "\"confidence\":0.9,"
        << "\"jaw_open_score\":0.8,"
        << "\"mouth_open_ratio\":0.7,"
        << "\"timestamp_ms\":" << timestamp_ms << ","
        << "\"backend\":\"queue-verifier\","
        << "\"status_text\":\"queue verifier camera sample\""
        << "}";
}

void remove_camera_bridge_fixture() {
    std::error_code ec;
    std::filesystem::remove("camera_bridge_latest.json", ec);
}

void remove_microphone_bridge_fixture() {
    std::error_code ec;
    std::filesystem::remove("microphone_bridge_latest.json", ec);
}

CheckResult verify_realtime_queue_precedes_interactive_queue() {
    prepare_ready_runtime(false, false);
    set_thread_mode(1);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    RuntimeState& state = runtime_state();
    state.camera_listener.enabled = true;
    state.camera_listener.bridge_running = true;
    state.camera_listener.device_available = true;
    state.camera_focus_locked = true;

    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "InteractiveProbe"));

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    const RuntimeThread& t1 = snapshot.threads.at(0);

    CheckResult result;
    result.name = "realtime queue precedes interactive queue";
    result.passed =
        t1.last_completed_task_priority == PriorityLevel::P2_FUNCTIONAL &&
        t1.last_completed_task_type == TaskType::CAMERA &&
        snapshot_has_task_type(
            snapshot.p2_realtime_queue,
            TaskType::CAMERA_LISTENER_BURST) &&
        snapshot.p2_queue.empty();
    result.details.push_back(
        "T1 priority=" + std::string(scheduler_queue_label(t1.last_completed_task_priority)));
    result.details.push_back(
        "T1 last=" + task_type_name(t1.last_completed_task_type));
    result.details.push_back(
        "P2 realtime queued=" + std::to_string(snapshot.p2_realtime_queue.size()));
    result.details.push_back(
        "P3 interactive queued=" + std::to_string(snapshot.p2_queue.size()));
    return result;
}

CheckResult verify_single_thread_idle_reseed_uses_burst_entry() {
    prepare_ready_runtime(true, true);
    set_thread_mode(1);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    RuntimeState& state = runtime_state();
    state.camera_device_available = true;
    state.camera_listener.device_available = true;
    state.camera_focus_locked = true;
    state.microphone_focus_locked = false;

    scheduler_task_support::seed_input_entry_task_if_idle();
    const SchedulerSnapshot seeded = scheduler_snapshot();

    drain_runtime_notes();
    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    drain_runtime_notes();

    CheckResult result;
    result.name = "single-thread idle reseed starts from burst listener entry";
    result.passed =
        snapshot_has_task_type(seeded.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST) &&
        !snapshot_has_task_type(seeded.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST) &&
        !snapshot_has_task_type(seeded.p2_queue, TaskType::CAMERA) &&
        snapshot.threads.at(0).last_completed_task_type == TaskType::CAMERA &&
        !snapshot_has_task_type(snapshot.p2_queue, TaskType::MICROPHONE) &&
        snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST);
    result.details.push_back(
        std::string("seeded-realtime-camera=") +
        (snapshot_has_task_type(seeded.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST)
             ? "yes"
             : "no"));
    result.details.push_back(
        std::string("seeded-realtime-mic=") +
        (snapshot_has_task_type(seeded.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST)
             ? "yes"
             : "no"));
    result.details.push_back(
        std::string("seeded-functional-camera=") +
        (snapshot_has_task_type(seeded.p2_queue, TaskType::CAMERA) ? "yes" : "no"));
    result.details.push_back(
        "T1 last=" + task_type_name(snapshot.threads.at(0).last_completed_task_type));
    result.details.push_back(
        std::string("mic-functional-queued=") +
        (snapshot_has_task_type(snapshot.p2_queue, TaskType::MICROPHONE) ? "yes" : "no"));
    result.details.push_back(
        std::string("mic-realtime-queued=") +
        (snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST)
             ? "yes"
             : "no"));
    return result;
}

CheckResult verify_single_thread_camera_handoff_swaps_to_microphone_burst() {
    prepare_ready_runtime(true, true);
    set_thread_mode(1);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    const long long sample_timestamp_ms = scheduler_task_support::current_time_ms();
    write_camera_bridge_fixture(sample_timestamp_ms, true, true);
    set_camera_bridge_enabled(false);

    RuntimeState& state = runtime_state();
    state.camera_listener.enabled = true;
    state.camera_listener.bridge_running = true;
    state.camera_listener.device_available = true;
    state.camera_listener.sample_ready = true;
    state.camera_listener.stale = false;
    state.camera_listener.last_seen_sample_ms = sample_timestamp_ms;
    state.camera_bridge.bridge_connected = true;
    state.camera_bridge.sample_ready = true;
    state.camera_bridge.device_unavailable = false;
    state.camera_bridge.face_detected = true;
    state.camera_bridge.mouth_open_state = true;
    state.camera_bridge.looking_forward = true;
    state.camera_bridge.timestamp_ms = sample_timestamp_ms;
    state.camera_bridge.mouth_center_x = 0.52f;
    state.camera_bridge.mouth_center_y = 0.37f;
    state.camera_device_available = true;
    state.camera_focus_locked = true;
    state.microphone_focus_locked = false;
    state.camera_gate_open = false;
    state.camera_gate_until_ms = 0;

    scheduler_task_support::seed_input_entry_task_if_idle();
    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();

    CheckResult result;
    result.name = "single-thread camera handoff swaps realtime entry to microphone burst";
    result.passed =
        snapshot.threads.at(0).last_completed_task_type == TaskType::CAMERA &&
        snapshot_has_task_type(snapshot.p2_queue, TaskType::MICROPHONE) &&
        snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST) &&
        snapshot.single_thread_chain_next == "Microphone";
    result.details.push_back(
        "T1 last=" + task_type_name(snapshot.threads.at(0).last_completed_task_type));
    result.details.push_back(
        std::string("mic-functional=") +
        (snapshot_has_task_type(snapshot.p2_queue, TaskType::MICROPHONE) ? "yes" : "no"));
    result.details.push_back(
        std::string("mic-realtime=") +
        (snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST)
             ? "yes"
             : "no"));
    result.details.push_back(
        std::string("cam-realtime=") +
        (snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST)
             ? "yes"
             : "no"));
    result.details.push_back(
        "chain-next=" + snapshot.single_thread_chain_next);

    remove_camera_bridge_fixture();
    return result;
}

CheckResult verify_single_thread_full_cycle_reseeds_camera_burst() {
    prepare_ready_runtime(true, true);
    set_thread_mode(1);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    set_camera_bridge_enabled(false);
    set_microphone_bridge_enabled(false);

    RuntimeState& state = runtime_state();
    state.world.dandelion_x = 0.95f;
    state.world.dandelion_y = 0.5f;
    scheduler_task_support::rebuild_particle_ring_for_world(0.95f, 0.5f, 100);

    const long long now_ms = scheduler_task_support::current_time_ms();
    state.camera_listener.enabled = true;
    state.camera_listener.bridge_running = true;
    state.camera_listener.device_available = true;
    state.camera_listener.sample_ready = true;
    state.camera_listener.stale = false;
    state.camera_listener.last_seen_sample_ms = now_ms;
    state.camera_bridge.bridge_connected = true;
    state.camera_bridge.sample_ready = true;
    state.camera_bridge.device_unavailable = false;
    state.camera_bridge.face_detected = true;
    state.camera_bridge.mouth_open_state = true;
    state.camera_bridge.looking_forward = true;
    state.camera_bridge.timestamp_ms = now_ms;
    state.camera_bridge.mouth_center_x = 0.52f;
    state.camera_bridge.mouth_center_y = 0.37f;
    state.camera_device_available = true;
    state.camera_available = true;

    state.microphone_listener.enabled = true;
    state.microphone_listener.bridge_running = true;
    state.microphone_listener.device_available = true;
    state.microphone_listener.sample_ready = true;
    state.microphone_listener.stale = false;
    state.microphone_listener.last_seen_sample_ms = now_ms + 1;
    state.microphone_bridge.bridge_connected = true;
    state.microphone_bridge.sample_ready = true;
    state.microphone_bridge.device_unavailable = false;
    state.microphone_bridge.voice_detected = true;
    state.microphone_bridge.fallback_requested = false;
    state.microphone_bridge.suggested_power = 0.1f;
    state.microphone_bridge.timestamp_ms = now_ms + 1;
    state.microphone_device_available = true;

    state.camera_focus_locked = true;
    state.microphone_focus_locked = false;
    state.microphone_focus_consumed_for_gate = false;
    state.camera_gate_open = false;
    state.camera_gate_frame = -1;
    state.camera_gate_until_ms = 0;
    state.microphone_focus_until_ms = 0;
    state.last_consumed_microphone_sample_ms = 0;

    scheduler_task_support::seed_input_entry_task_if_idle();

    bool saw_camera = false;
    bool saw_microphone = false;
    bool saw_generate_or_breeze = false;
    bool saw_batch = false;
    bool saw_drain = false;
    bool reseeded_camera = false;
    bool quieted_followup_input = false;
    int ticks_used = 0;
    SchedulerSnapshot last_snapshot;

    for (int tick = 0; tick < 80; ++tick) {
        scheduler_tick();
        ticks_used = tick + 1;
        const SchedulerSnapshot snapshot = scheduler_snapshot();
        last_snapshot = snapshot;
        const RuntimeThread& t1 = snapshot.threads.at(0);

        saw_camera = saw_camera || (t1.last_completed_task_type == TaskType::CAMERA);
        saw_microphone = saw_microphone || (t1.last_completed_task_type == TaskType::MICROPHONE);
        saw_generate_or_breeze =
            saw_generate_or_breeze ||
            t1.last_completed_task_type == TaskType::GENERATE_PARTICLE ||
            t1.last_completed_task_type == TaskType::BREEZE;
        saw_batch =
            saw_batch ||
            t1.last_completed_task_type == TaskType::BATCH_PARTICLE_EXECUTION ||
            snapshot.p3_move_queued + snapshot.p3_move_running +
                    snapshot.p3_fade_queued + snapshot.p3_fade_running >
                0;

        if (saw_generate_or_breeze && !quieted_followup_input) {
            state.camera_bridge.mouth_open_state = false;
            state.camera_bridge.looking_forward = false;
            state.microphone_bridge.voice_detected = false;
            state.microphone_bridge.sample_ready = false;
            state.microphone_listener.sample_ready = false;
            state.camera_gate_open = false;
            state.camera_gate_frame = -1;
            state.camera_gate_until_ms = 0;
            state.microphone_focus_locked = false;
            state.microphone_focus_until_ms = 0;
            state.microphone_focus_consumed_for_gate = true;
            quieted_followup_input = true;
        }

        const std::vector<std::string> notes = current_runtime_notes();
        saw_drain = saw_drain || std::any_of(
            notes.begin(),
            notes.end(),
            [](const std::string& note) {
                return note.find("feeder drained current P3 stage") != std::string::npos ||
                    note.find("single-thread cycle reseeded camera burst entry (cycle-drained)") !=
                        std::string::npos;
            });

        reseeded_camera =
            saw_drain &&
            snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST) &&
            snapshot.p2_queue.empty() &&
            snapshot.p3_queue.empty() &&
            snapshot.single_thread_chain_next == "Camera" &&
            saw_camera &&
            saw_microphone &&
            saw_generate_or_breeze &&
            saw_batch;
        if (reseeded_camera) {
            break;
        }
    }

    CheckResult result;
    result.name =
        "single-thread full cycle drains and reseeds camera burst";
    result.passed =
        saw_camera &&
        saw_microphone &&
        saw_generate_or_breeze &&
        saw_batch &&
        saw_drain &&
        reseeded_camera;
    result.details.push_back(std::string("saw-camera=") + (saw_camera ? "yes" : "no"));
    result.details.push_back(std::string("saw-microphone=") + (saw_microphone ? "yes" : "no"));
    result.details.push_back(
        std::string("saw-generate/breeze=") + (saw_generate_or_breeze ? "yes" : "no"));
    result.details.push_back(std::string("saw-batch=") + (saw_batch ? "yes" : "no"));
    result.details.push_back(std::string("saw-drain=") + (saw_drain ? "yes" : "no"));
    result.details.push_back(
        std::string("reseeded-camera-burst=") + (reseeded_camera ? "yes" : "no"));
    result.details.push_back("ticks-used=" + std::to_string(ticks_used));
    result.details.push_back(
        "final-p2-realtime=" + std::to_string(last_snapshot.p2_realtime_queue.size()));
    result.details.push_back(
        "final-p2=" + std::to_string(last_snapshot.p2_queue.size()));
    result.details.push_back(
        "final-p3=" + std::to_string(last_snapshot.p3_queue.size()));
    result.details.push_back(
        "final-chain-current=" + last_snapshot.single_thread_chain_current);
    result.details.push_back(
        "final-chain-next=" + last_snapshot.single_thread_chain_next);
    result.details.push_back(
        "final-p2-head=" +
        (last_snapshot.p2_queue.empty() ? std::string("none")
                                        : task_type_name(last_snapshot.p2_queue.front().type)));
    result.details.push_back(
        "final-p3-head=" +
        (last_snapshot.p3_queue.empty() ? std::string("none")
                                        : task_type_name(last_snapshot.p3_queue.front().type)));
    return result;
}

CheckResult verify_single_thread_fallback_cycle_reseeds_camera_burst() {
    prepare_ready_runtime(true, true);
    set_thread_mode(1);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    set_camera_bridge_enabled(false);
    set_microphone_bridge_enabled(false);

    RuntimeState& state = runtime_state();
    state.world.dandelion_x = 0.95f;
    state.world.dandelion_y = 0.5f;
    scheduler_task_support::rebuild_particle_ring_for_world(0.95f, 0.5f, 100);

    const long long now_ms = scheduler_task_support::current_time_ms();
    state.camera_listener.enabled = true;
    state.camera_listener.bridge_running = true;
    state.camera_listener.device_available = true;
    state.camera_listener.sample_ready = true;
    state.camera_listener.stale = false;
    state.camera_listener.last_seen_sample_ms = now_ms;
    state.camera_bridge.bridge_connected = true;
    state.camera_bridge.sample_ready = true;
    state.camera_bridge.device_unavailable = false;
    state.camera_bridge.face_detected = true;
    state.camera_bridge.mouth_open_state = true;
    state.camera_bridge.looking_forward = true;
    state.camera_bridge.timestamp_ms = now_ms;
    state.camera_bridge.mouth_center_x = 0.52f;
    state.camera_bridge.mouth_center_y = 0.37f;
    state.camera_device_available = true;
    state.camera_available = true;

    state.microphone_listener.enabled = true;
    state.microphone_listener.bridge_running = true;
    state.microphone_listener.device_available = true;
    state.microphone_listener.sample_ready = true;
    state.microphone_listener.stale = false;
    state.microphone_listener.last_seen_sample_ms = now_ms + 1;
    state.microphone_bridge.bridge_connected = true;
    state.microphone_bridge.sample_ready = true;
    state.microphone_bridge.device_unavailable = false;
    state.microphone_bridge.voice_detected = false;
    state.microphone_bridge.fallback_requested = true;
    state.microphone_bridge.suggested_power = 0.1f;
    state.microphone_bridge.timestamp_ms = now_ms + 1;
    state.microphone_device_available = true;

    state.camera_focus_locked = true;
    state.microphone_focus_locked = false;
    state.microphone_focus_consumed_for_gate = false;
    state.camera_gate_open = false;
    state.camera_gate_frame = -1;
    state.camera_gate_until_ms = 0;
    state.microphone_focus_until_ms = 0;
    state.last_consumed_microphone_sample_ms = 0;

    scheduler_task_support::seed_input_entry_task_if_idle();

    bool saw_camera = false;
    bool saw_microphone = false;
    bool saw_breeze = false;
    bool saw_batch = false;
    bool saw_drain = false;
    bool reseeded_camera = false;
    bool quieted_followup_input = false;
    int ticks_used = 0;
    SchedulerSnapshot last_snapshot;

    for (int tick = 0; tick < 80; ++tick) {
        scheduler_tick();
        ticks_used = tick + 1;
        const SchedulerSnapshot snapshot = scheduler_snapshot();
        last_snapshot = snapshot;
        const RuntimeThread& t1 = snapshot.threads.at(0);

        saw_camera = saw_camera || (t1.last_completed_task_type == TaskType::CAMERA);
        saw_microphone = saw_microphone || (t1.last_completed_task_type == TaskType::MICROPHONE);
        saw_breeze = saw_breeze || t1.last_completed_task_type == TaskType::BREEZE;
        saw_batch =
            saw_batch ||
            t1.last_completed_task_type == TaskType::BATCH_PARTICLE_EXECUTION ||
            snapshot.p3_move_queued + snapshot.p3_move_running +
                    snapshot.p3_fade_queued + snapshot.p3_fade_running >
                0;

        if (saw_breeze && !quieted_followup_input) {
            state.camera_bridge.mouth_open_state = false;
            state.camera_bridge.looking_forward = false;
            state.microphone_bridge.fallback_requested = false;
            state.microphone_bridge.sample_ready = false;
            state.microphone_listener.sample_ready = false;
            state.camera_gate_open = false;
            state.camera_gate_frame = -1;
            state.camera_gate_until_ms = 0;
            state.microphone_focus_locked = false;
            state.microphone_focus_until_ms = 0;
            state.microphone_focus_consumed_for_gate = true;
            quieted_followup_input = true;
        }

        const std::vector<std::string> notes = current_runtime_notes();
        saw_drain = saw_drain || std::any_of(
            notes.begin(),
            notes.end(),
            [](const std::string& note) {
                return note.find("feeder drained current P3 stage") != std::string::npos ||
                    note.find("single-thread cycle reseeded camera burst entry (cycle-drained)") !=
                        std::string::npos;
            });

        reseeded_camera =
            saw_drain &&
            snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST) &&
            snapshot.p2_queue.empty() &&
            snapshot.p3_queue.empty() &&
            snapshot.single_thread_chain_next == "Camera" &&
            saw_camera &&
            saw_microphone &&
            saw_breeze &&
            saw_batch;
        if (reseeded_camera) {
            break;
        }
    }

    CheckResult result;
    result.name =
        "single-thread fallback cycle drains and reseeds camera burst";
    result.passed =
        saw_camera &&
        saw_microphone &&
        saw_breeze &&
        saw_batch &&
        saw_drain &&
        reseeded_camera;
    result.details.push_back(std::string("saw-camera=") + (saw_camera ? "yes" : "no"));
    result.details.push_back(std::string("saw-microphone=") + (saw_microphone ? "yes" : "no"));
    result.details.push_back(std::string("saw-breeze=") + (saw_breeze ? "yes" : "no"));
    result.details.push_back(std::string("saw-batch=") + (saw_batch ? "yes" : "no"));
    result.details.push_back(std::string("saw-drain=") + (saw_drain ? "yes" : "no"));
    result.details.push_back(
        std::string("reseeded-camera-burst=") + (reseeded_camera ? "yes" : "no"));
    result.details.push_back("ticks-used=" + std::to_string(ticks_used));
    result.details.push_back(
        "final-p2-realtime=" + std::to_string(last_snapshot.p2_realtime_queue.size()));
    result.details.push_back(
        "final-p2=" + std::to_string(last_snapshot.p2_queue.size()));
    result.details.push_back(
        "final-p3=" + std::to_string(last_snapshot.p3_queue.size()));
    result.details.push_back(
        "final-chain-current=" + last_snapshot.single_thread_chain_current);
    result.details.push_back(
        "final-chain-next=" + last_snapshot.single_thread_chain_next);
    result.details.push_back(
        "final-p2-head=" +
        (last_snapshot.p2_queue.empty() ? std::string("none")
                                        : task_type_name(last_snapshot.p2_queue.front().type)));
    result.details.push_back(
        "final-p3-head=" +
        (last_snapshot.p3_queue.empty() ? std::string("none")
                                        : task_type_name(last_snapshot.p3_queue.front().type)));
    return result;
}

CheckResult verify_mode_collapse_preserves_single_thread_phase_order_for(
    TaskType downstream_entry_type,
    const char* result_name) {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    set_camera_bridge_enabled(false);
    set_microphone_bridge_enabled(false);

    RuntimeState& state = runtime_state();
    state.world.dandelion_x = 0.95f;
    state.world.dandelion_y = 0.5f;
    state.world.mouth_x = 0.52f;
    state.world.mouth_y = 0.37f;
    state.world.power = downstream_entry_type == TaskType::GENERATE_PARTICLE ? 0.6f : 5.0f;
    scheduler_task_support::rebuild_particle_ring_for_world(0.95f, 0.5f, 100);
    scheduler_task_support::reconcile_runtime_particle_bookkeeping();

    state.camera_listener.enabled = true;
    state.camera_listener.bridge_running = true;
    state.camera_listener.device_available = true;
    state.camera_device_available = true;
    state.camera_available = true;

    state.microphone_listener.enabled = true;
    state.microphone_listener.bridge_running = true;
    state.microphone_listener.device_available = true;
    state.microphone_device_available = true;

    state.camera_focus_locked = true;
    state.microphone_focus_locked = true;
    state.microphone_focus_consumed_for_gate = true;
    state.camera_gate_open = true;
    state.camera_gate_frame = scheduler_task_support::current_scheduler_frame_index();
    state.camera_gate_until_ms =
        scheduler_task_support::current_time_ms() +
        scheduler_task_support::kCameraGateHoldMs;
    state.microphone_focus_until_ms =
        scheduler_task_support::current_time_ms() +
        scheduler_task_support::kCameraGateHoldMs;

    submit_task(make_camera_listener_service_task());
    submit_task(make_microphone_listener_service_task());
    submit_task(
        downstream_entry_type == TaskType::GENERATE_PARTICLE
            ? make_generate_particle_task()
            : make_breeze_task());

    const SchedulerSnapshot before_collapse = scheduler_snapshot();
    set_thread_mode(1);
    const SchedulerSnapshot after_collapse = scheduler_snapshot();

    bool saw_downstream_entry = false;
    bool saw_batch = false;
    bool saw_drain = false;
    bool reseeded_camera = false;
    bool phase_broken = false;
    int ticks_used = 0;
    SchedulerSnapshot last_snapshot;

    for (int tick = 0; tick < 80; ++tick) {
        scheduler_tick();
        ticks_used = tick + 1;
        const SchedulerSnapshot snapshot = scheduler_snapshot();
        last_snapshot = snapshot;
        const RuntimeThread& t1 = snapshot.threads.at(0);

        const std::vector<std::string> notes = current_runtime_notes();
        const bool tick_drain_observed = std::any_of(
            notes.begin(),
            notes.end(),
            [](const std::string& note) {
                return note.find("feeder drained current P3 stage") != std::string::npos ||
                    note.find("single-thread cycle reseeded camera burst entry (cycle-drained)") !=
                        std::string::npos;
            });
        saw_drain = saw_drain || tick_drain_observed;
        const bool tick_camera_reseeded = std::any_of(
            notes.begin(),
            notes.end(),
            [](const std::string& note) {
                return note.find("single-thread cycle reseeded camera burst entry (cycle-drained)") !=
                    std::string::npos;
            });

        if (!saw_drain) {
            const bool illegal_sensor_reentry =
                t1.last_completed_task_type == TaskType::CAMERA ||
                t1.last_completed_task_type == TaskType::MICROPHONE;
            if (illegal_sensor_reentry) {
                phase_broken = true;
            }
        }

        saw_downstream_entry =
            saw_downstream_entry || t1.last_completed_task_type == downstream_entry_type;
        saw_batch =
            saw_batch ||
            t1.last_completed_task_type == TaskType::BATCH_PARTICLE_EXECUTION ||
            snapshot.p3_move_queued + snapshot.p3_move_running +
                    snapshot.p3_fade_queued + snapshot.p3_fade_running >
                0;

        reseeded_camera =
            reseeded_camera ||
            tick_camera_reseeded ||
            (saw_drain &&
             (snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST) ||
              t1.last_completed_task_type == TaskType::CAMERA)) ||
            (snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST) &&
             !snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST) &&
             snapshot.p2_queue.empty() &&
             snapshot.p3_queue.empty() &&
             saw_downstream_entry &&
             saw_batch &&
             saw_drain);
        if (reseeded_camera || phase_broken) {
            break;
        }
    }

    CheckResult result;
    result.name = result_name;
    const bool returned_to_camera_after_drain =
        reseeded_camera ||
        (saw_drain &&
         (snapshot_has_task_type(last_snapshot.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST) ||
          last_snapshot.threads.at(0).last_completed_task_type == TaskType::CAMERA));
    result.passed =
        snapshot_has_task_type(before_collapse.p2_realtime_queue, TaskType::CAMERA_LISTENER_SERVICE) &&
        snapshot_has_task_type(before_collapse.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_SERVICE) &&
        !snapshot_has_task_type(after_collapse.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST) &&
        !snapshot_has_task_type(after_collapse.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST) &&
        saw_downstream_entry &&
        saw_batch &&
        saw_drain &&
        returned_to_camera_after_drain &&
        !phase_broken;
    result.details.push_back(
        "downstream-entry=" + task_type_name(downstream_entry_type));
    result.details.push_back(
        std::string("collapse-seeded-burst-early=") +
        ((snapshot_has_task_type(after_collapse.p2_realtime_queue, TaskType::CAMERA_LISTENER_BURST) ||
          snapshot_has_task_type(after_collapse.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST))
             ? "yes"
             : "no"));
    result.details.push_back(
        std::string("saw-downstream-entry=") + (saw_downstream_entry ? "yes" : "no"));
    result.details.push_back(
        std::string("saw-batch=") + (saw_batch ? "yes" : "no"));
    result.details.push_back(
        std::string("saw-drain=") + (saw_drain ? "yes" : "no"));
    result.details.push_back(
        std::string("phase-broken=") + (phase_broken ? "yes" : "no"));
    result.details.push_back(
        std::string("reseeded-camera-burst=") + (returned_to_camera_after_drain ? "yes" : "no"));
    result.details.push_back("ticks-used=" + std::to_string(ticks_used));
    result.details.push_back(
        "final-chain-current=" + last_snapshot.single_thread_chain_current);
    result.details.push_back(
        "final-chain-next=" + last_snapshot.single_thread_chain_next);
    result.details.push_back(
        "final-last=" + task_type_name(last_snapshot.threads.at(0).last_completed_task_type));
    return result;
}

CheckResult verify_mode_collapse_preserves_generate_phase_order() {
    return verify_mode_collapse_preserves_single_thread_phase_order_for(
        TaskType::GENERATE_PARTICLE,
        "mode collapse preserves single-thread generate -> batch -> drain order");
}

CheckResult verify_mode_collapse_preserves_breeze_phase_order() {
    return verify_mode_collapse_preserves_single_thread_phase_order_for(
        TaskType::BREEZE,
        "mode collapse preserves single-thread breeze -> batch -> drain order");
}

CheckResult verify_listener_runtime_snapshot_fields() {
    prepare_ready_runtime(true, true);
    set_thread_mode(1);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    RuntimeState& state = runtime_state();
    state.camera_bridge.timestamp_ms = 111;
    state.microphone_bridge.timestamp_ms = 222;
    state.camera_listener.last_seen_sample_ms = 111;
    state.microphone_listener.last_seen_sample_ms = 222;
    state.last_consumed_microphone_sample_ms = 222;
    state.microphone_listener.last_consumed_sample_ms = 222;

    submit_task(make_camera_listener_service_task());
    submit_task(make_microphone_listener_service_task());
    scheduler_task_support::queue_camera_stage_if_needed();
    scheduler_task_support::queue_microphone_stage_if_needed();

    const SchedulerSnapshot snapshot = scheduler_snapshot();

    CheckResult result;
    result.name = "listener runtime snapshot exposes service and sample identity";
    result.passed =
        snapshot.camera_listener_runtime.service_task_alive &&
        snapshot.camera_listener_runtime.last_seen_sample_ms == 111 &&
        snapshot.camera_listener_runtime.last_seeded_sample_ms == 111 &&
        snapshot.camera_listener_runtime.consumer_task_queued &&
        snapshot.microphone_listener_runtime.last_seen_sample_ms == 222 &&
        snapshot.microphone_listener_runtime.last_seeded_sample_ms == 222 &&
        snapshot.microphone_listener_runtime.last_consumed_sample_ms == 222 &&
        snapshot.microphone_listener_runtime.consumer_task_queued;
    result.details.push_back(
        listener_service_status_text(snapshot.camera_listener_runtime));
    result.details.push_back(
        listener_sample_text(snapshot.camera_listener_runtime));
    result.details.push_back(
        listener_service_status_text(snapshot.microphone_listener_runtime));
    result.details.push_back(
        listener_sample_text(snapshot.microphone_listener_runtime));
    return result;
}

CheckResult verify_two_thread_camera_plus_general() {
    prepare_ready_runtime(true, true);
    set_thread_mode(2);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "CameraLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::CHANGE_DANDELION,
        PriorityLevel::P2_FUNCTIONAL,
        "GeneralLaneProbe"));

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();

    CheckResult result;
    result.name = "two-thread camera lane + general lane";
    const RuntimeThread& t1 = snapshot.threads.at(0);
    const RuntimeThread& t2 = snapshot.threads.at(1);
    const bool roles_ok =
        t1.is_pinned && t1.pinned_task_type == TaskType::CAMERA &&
        t2.is_pinned && t2.pinned_task_type == TaskType::NONE;
    const bool dispatch_ok =
        thread_completed_type(t1, TaskType::CAMERA) &&
        thread_completed_type(t2, TaskType::CHANGE_DANDELION);

    result.passed = roles_ok && dispatch_ok;
    result.details.push_back(
        "T1 role=" + task_type_name(t1.pinned_task_type) +
        " last=" + task_type_name(t1.last_completed_task_type));
    result.details.push_back(
        "T2 role=" + task_type_name(t2.pinned_task_type) +
        " last=" + task_type_name(t2.last_completed_task_type));
    return result;
}

CheckResult verify_three_thread_camera_microphone_general() {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "CameraLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::MICROPHONE,
        PriorityLevel::P2_FUNCTIONAL,
        "MicrophoneLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::CHANGE_DANDELION,
        PriorityLevel::P2_FUNCTIONAL,
        "GeneralLaneProbe"));

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();

    CheckResult result;
    result.name = "three-thread camera lane + microphone lane + general lane";
    const RuntimeThread& t1 = snapshot.threads.at(0);
    const RuntimeThread& t2 = snapshot.threads.at(1);
    const RuntimeThread& t3 = snapshot.threads.at(2);
    const bool roles_ok =
        t1.is_pinned && t1.pinned_task_type == TaskType::CAMERA &&
        t2.is_pinned && t2.pinned_task_type == TaskType::MICROPHONE &&
        t3.is_pinned && t3.pinned_task_type == TaskType::NONE;
    const bool dispatch_ok =
        thread_completed_type(t1, TaskType::CAMERA) &&
        thread_completed_type(t2, TaskType::MICROPHONE) &&
        thread_completed_type(t3, TaskType::CHANGE_DANDELION);

    result.passed = roles_ok && dispatch_ok;
    result.details.push_back(
        "T1 role=" + task_type_name(t1.pinned_task_type) +
        " last=" + task_type_name(t1.last_completed_task_type));
    result.details.push_back(
        "T2 role=" + task_type_name(t2.pinned_task_type) +
        " last=" + task_type_name(t2.last_completed_task_type));
    result.details.push_back(
        "T3 role=" + task_type_name(t3.pinned_task_type) +
        " last=" + task_type_name(t3.last_completed_task_type));
    return result;
}

CheckResult verify_general_lane_owns_batch_feeder() {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "CameraLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::MICROPHONE,
        PriorityLevel::P2_FUNCTIONAL,
        "MicrophoneLaneProbe"));

    with_shared_state_write(true, false, true, []() {
        ParticleRenderData& particle = render_data().particles.at(0);
        particle.ownership_token = 1;
        particle.attached = false;
        particle.active = true;
        particle.status = "queued";
        particle.fade_steps_remaining = 0;
    });

    submit_task(make_single_particle_task(0, 1, 1, 0.7f, 0.2f));
    submit_task(make_batch_particle_execution_task());

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();

    CheckResult result;
    result.name = "general lane owns batch feeder into P3";
    const RuntimeThread& t1 = snapshot.threads.at(0);
    const RuntimeThread& t2 = snapshot.threads.at(1);
    const RuntimeThread& t3 = snapshot.threads.at(2);

    const bool dedicated_filtered =
        thread_completed_type(t1, TaskType::CAMERA) &&
        thread_completed_type(t2, TaskType::MICROPHONE);
    const bool general_took_batch =
        thread_completed_type(t3, TaskType::BATCH_PARTICLE_EXECUTION);
    const bool p3_still_active =
        snapshot.p3_move_queued + snapshot.p3_move_running +
            snapshot.p3_fade_queued + snapshot.p3_fade_running > 0;

    result.passed = dedicated_filtered && general_took_batch && p3_still_active;
    result.details.push_back(
        "T1 last=" + task_type_name(t1.last_completed_task_type));
    result.details.push_back(
        "T2 last=" + task_type_name(t2.last_completed_task_type));
    result.details.push_back(
        "T3 last=" + task_type_name(t3.last_completed_task_type));
    result.details.push_back(
        "P3 move q/r=" + std::to_string(snapshot.p3_move_queued) + "/" +
        std::to_string(snapshot.p3_move_running) + " fade q/r=" +
        std::to_string(snapshot.p3_fade_queued) + "/" +
        std::to_string(snapshot.p3_fade_running));
    return result;
}

CheckResult verify_multithread_focus_is_window_only() {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    RuntimeState& state = runtime_state();
    state.camera_focus_locked = true;
    state.microphone_focus_locked = false;
    state.camera_gate_open = false;
    render_data().ui.input_focus_status =
        scheduler_task_support::build_input_focus_status(state);

    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "CameraLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::MICROPHONE,
        PriorityLevel::P2_FUNCTIONAL,
        "MicrophoneLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::CHANGE_DANDELION,
        PriorityLevel::P2_FUNCTIONAL,
        "GeneralLaneProbe"));

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();

    CheckResult result;
    result.name = "multi-thread focus acts as input window, not dispatch policy";
    const RuntimeThread& t1 = snapshot.threads.at(0);
    const RuntimeThread& t2 = snapshot.threads.at(1);
    const RuntimeThread& t3 = snapshot.threads.at(2);

    const bool dispatch_still_lane_driven =
        thread_completed_type(t1, TaskType::CAMERA) &&
        thread_completed_type(t2, TaskType::MICROPHONE) &&
        thread_completed_type(t3, TaskType::CHANGE_DANDELION);
    result.passed = dispatch_still_lane_driven;
    result.details.push_back(
        "focus-status=" + render_data().ui.input_focus_status);
    result.details.push_back(
        "T1 last=" + task_type_name(t1.last_completed_task_type));
    result.details.push_back(
        "T2 last=" + task_type_name(t2.last_completed_task_type));
    result.details.push_back(
        "T3 last=" + task_type_name(t3.last_completed_task_type));
    return result;
}

CheckResult verify_multithread_camera_reseed_while_queues_active() {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    submit_task(make_placeholder_task(
        TaskType::CHANGE_DANDELION,
        PriorityLevel::P2_FUNCTIONAL,
        "GeneralLaneProbe"));

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    const std::vector<std::string> notes = current_runtime_notes();

    CheckResult result;
    result.name = "multi-thread camera reseed while queues remain active";
    const RuntimeThread& t1 = snapshot.threads.at(0);
    const RuntimeThread& t2 = snapshot.threads.at(1);
    const RuntimeThread& t3 = snapshot.threads.at(2);

    const bool camera_reseeded =
        thread_completed_type(t1, TaskType::CAMERA);
    const bool general_work_still_progressed =
        thread_completed_type(t2, TaskType::CHANGE_DANDELION) ||
        thread_completed_type(t3, TaskType::CHANGE_DANDELION);
    const bool service_alive = snapshot.camera_listener_runtime.service_task_alive;

    result.passed = camera_reseeded && general_work_still_progressed && service_alive;
    result.details.push_back(
        "T1 last=" + task_type_name(t1.last_completed_task_type));
    result.details.push_back(
        "T2 event=" + t2.last_task_event +
        " last=" + task_type_name(t2.last_completed_task_type));
    result.details.push_back(
        "T3 event=" + t3.last_task_event +
        " last=" + task_type_name(t3.last_completed_task_type));
    result.details.push_back(
        std::string("camera-service=") + (service_alive ? "alive" : "missing"));
    result.details.push_back(
        "p2-queued=" + std::to_string(snapshot.p2_queue.size()));
    return result;
}

CheckResult verify_multithread_microphone_handoff_overlaps_p3_drain() {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    set_microphone_bridge_enabled(true);
    const long long sample_timestamp_ms = scheduler_task_support::current_time_ms();
    write_microphone_bridge_fixture(sample_timestamp_ms, 0.7f, true, false);
    refresh_bridge_inputs();

    RuntimeState& state = runtime_state();
    state.camera_device_available = true;
    state.camera_gate_open = true;
    state.camera_gate_frame = 0;
    state.microphone_focus_locked = true;
    state.microphone_focus_until_ms =
        scheduler_task_support::current_time_ms() +
        scheduler_task_support::kCameraGateHoldMs;

    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "CameraLaneProbe"));

    with_shared_state_write(true, false, true, []() {
        ParticleRenderData& particle = render_data().particles.at(0);
        particle.ownership_token = 1;
        particle.attached = false;
        particle.active = true;
        particle.status = "queued";
        particle.fade_steps_remaining = 0;
    });
    submit_task(make_single_particle_task(0, 1, 1, 0.6f, 0.3f));
    submit_task(make_batch_particle_execution_task());

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    const std::vector<std::string> notes = current_runtime_notes();

    const auto has_p2_type = [&snapshot](TaskType type) {
        return std::any_of(
            snapshot.p2_queue.begin(),
            snapshot.p2_queue.end(),
            [type](const TaskRecord& record) {
                return record.type == type;
            });
    };

    CheckResult result;
    result.name = "multi-thread microphone handoff overlaps active P3 drain";
    const RuntimeThread& t1 = snapshot.threads.at(0);
    const RuntimeThread& t2 = snapshot.threads.at(1);
    const RuntimeThread& t3 = snapshot.threads.at(2);
    const bool microphone_progressed =
        thread_completed_type(t2, TaskType::MICROPHONE);
    const bool generated_new_batch =
        thread_completed_type(t1, TaskType::GENERATE_PARTICLE) ||
        thread_completed_type(t3, TaskType::GENERATE_PARTICLE) ||
        has_p2_type(TaskType::GENERATE_PARTICLE) ||
        has_p2_type(TaskType::BREEZE);
    const bool throughput_stage_stayed_live =
        thread_completed_type(t1, TaskType::BATCH_PARTICLE_EXECUTION) ||
        thread_completed_type(t3, TaskType::BATCH_PARTICLE_EXECUTION) ||
        snapshot.p3_move_queued + snapshot.p3_move_running > 1;
    const bool service_alive = snapshot.microphone_listener_runtime.service_task_alive;

    result.passed =
        microphone_progressed &&
        generated_new_batch &&
        throughput_stage_stayed_live &&
        service_alive;
    result.details.push_back(
        "T1 last=" + task_type_name(t1.last_completed_task_type));
    result.details.push_back(
        "T2 last=" + task_type_name(t2.last_completed_task_type));
    result.details.push_back(
        "T3 last=" + task_type_name(t3.last_completed_task_type));
    result.details.push_back(
        std::string("generate-handoff=") + (generated_new_batch ? "yes" : "no"));
    result.details.push_back(
        "p3-move-q/r=" + std::to_string(snapshot.p3_move_queued) + "/" +
        std::to_string(snapshot.p3_move_running));
    result.details.push_back(
        std::string("microphone-service=") + (service_alive ? "alive" : "missing"));
    remove_microphone_bridge_fixture();
    set_microphone_bridge_enabled(false);
    return result;
}

CheckResult verify_multithread_l1_realtime_cuts_across_p2_slots() {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    submit_task(make_camera_listener_service_task());
    submit_task(make_microphone_listener_service_task());
    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "CameraLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::MICROPHONE,
        PriorityLevel::P2_FUNCTIONAL,
        "MicrophoneLaneProbe"));
    submit_task(make_batch_particle_execution_task());

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    const std::vector<std::string> notes = current_runtime_notes();

    const int realtime_slice_count = static_cast<int>(std::count_if(
        notes.begin(),
        notes.end(),
        [](const std::string& note) {
            return note.find("Realtime slice yielded") != std::string::npos;
        }));

    CheckResult result;
    result.name = "multi-thread L1 realtime slices can cut across P2 dispatch slots";
    result.passed =
        realtime_slice_count >= 2 &&
        snapshot.camera_listener_runtime.service_task_alive &&
        snapshot.microphone_listener_runtime.service_task_alive &&
        std::any_of(
            snapshot.threads.begin(),
            snapshot.threads.end(),
            [](const RuntimeThread& thread) {
                return thread.last_completed_task_type == TaskType::BATCH_PARTICLE_EXECUTION;
            });
    result.details.push_back(
        "realtime-slices=" + std::to_string(realtime_slice_count));
    result.details.push_back(
        std::string("camera-service=") +
        (snapshot.camera_listener_runtime.service_task_alive ? "alive" : "missing"));
    result.details.push_back(
        std::string("microphone-service=") +
        (snapshot.microphone_listener_runtime.service_task_alive ? "alive" : "missing"));
    result.details.push_back(
        "T1 last=" + task_type_name(snapshot.threads.at(0).last_completed_task_type));
    result.details.push_back(
        "T2 last=" + task_type_name(snapshot.threads.at(1).last_completed_task_type));
    result.details.push_back(
        "T3 last=" + task_type_name(snapshot.threads.at(2).last_completed_task_type));
    return result;
}

CheckResult verify_two_thread_prefers_camera_long_and_microphone_short_listener() {
    prepare_ready_runtime(true, true);
    set_thread_mode(2);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    submit_task(make_placeholder_task(
        TaskType::CHANGE_DANDELION,
        PriorityLevel::P2_FUNCTIONAL,
        "GeneralLaneProbe"));

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    const std::vector<std::string> notes = current_runtime_notes();

    const bool saw_camera_long_slice = std::any_of(
        notes.begin(),
        notes.end(),
        [](const std::string& note) {
            return note.find("Realtime slice yielded CameraListenerServiceTask") !=
                std::string::npos;
        });
    const bool saw_microphone_short_slice = std::any_of(
        notes.begin(),
        notes.end(),
        [](const std::string& note) {
            return note.find("Realtime slice yielded MicrophoneListenerBurstTask") !=
                std::string::npos;
        });

    CheckResult result;
    result.name = "two-thread mode prefers camera long listener and microphone short listener";
    result.passed =
        snapshot.camera_listener_runtime.service_task_alive &&
        !snapshot.microphone_listener_runtime.service_task_alive &&
        snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST) &&
        saw_camera_long_slice &&
        saw_microphone_short_slice;
    result.details.push_back(
        std::string("camera-service=") +
        (snapshot.camera_listener_runtime.service_task_alive ? "alive" : "missing"));
    result.details.push_back(
        std::string("microphone-service=") +
        (snapshot.microphone_listener_runtime.service_task_alive ? "alive" : "missing"));
    result.details.push_back(
        std::string("microphone-short=") +
        (snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST)
             ? "queued"
             : "missing"));
    result.details.push_back(
        std::string("saw-camera-long-slice=") + (saw_camera_long_slice ? "yes" : "no"));
    result.details.push_back(
        std::string("saw-microphone-short-slice=") + (saw_microphone_short_slice ? "yes" : "no"));
    return result;
}

CheckResult verify_two_thread_short_microphone_listener_does_not_auto_renew() {
    prepare_ready_runtime(true, true);
    set_thread_mode(2);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    scheduler_tick();
    drain_runtime_notes();

    with_shared_state_write(false, false, true, []() {
        RuntimeState& state_ref = runtime_state();
        state_ref.microphone_listener.short_lease_active = true;
        state_ref.microphone_listener.short_lease_started_at_ms = 1;
        state_ref.microphone_listener.short_lease_until_ms =
            scheduler_task_support::current_time_ms() - 1;
        state_ref.microphone_listener.short_warmup_until_ms = 0;
        state_ref.microphone_listener.short_detect_ready_at_ms = 0;
    });

    clear_task_queue(PriorityLevel::P2_REALTIME);
    scheduler_tick();

    const SchedulerSnapshot snapshot = scheduler_snapshot();
    const std::vector<std::string> notes = current_runtime_notes();
    const bool renewed_microphone_short =
        snapshot_has_task_type(snapshot.p2_realtime_queue, TaskType::MICROPHONE_LISTENER_BURST);
    const bool reseed_note_present = std::any_of(
        notes.begin(),
        notes.end(),
        [](const std::string& note) {
            return note.find("seeded mode-specific short listener lease") != std::string::npos;
        });

    CheckResult result;
    result.name = "two-thread short microphone listener does not auto-renew expired lease";
    result.passed =
        snapshot.camera_listener_runtime.service_task_alive &&
        !renewed_microphone_short &&
        !snapshot.microphone_listener_runtime.short_lease_active &&
        !reseed_note_present;
    result.details.push_back(
        std::string("camera-service=") +
        (snapshot.camera_listener_runtime.service_task_alive ? "alive" : "missing"));
    result.details.push_back(
        std::string("microphone-short-renewed=") + (renewed_microphone_short ? "yes" : "no"));
    result.details.push_back(
        std::string("microphone-short-lease-active=") +
        (snapshot.microphone_listener_runtime.short_lease_active ? "yes" : "no"));
    result.details.push_back(
        std::string("reseed-note=") + (reseed_note_present ? "present" : "missing"));
    return result;
}

CheckResult verify_p1_reset_preempts_multithread_lanes() {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_REALTIME,
        "RealtimeCameraProbe"));
    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "CameraLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::MICROPHONE,
        PriorityLevel::P2_FUNCTIONAL,
        "MicrophoneLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::CHANGE_DANDELION,
        PriorityLevel::P2_FUNCTIONAL,
        "GeneralLaneProbe"));
    submit_task(make_reset_task());

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    const std::vector<std::string> notes = current_runtime_notes();

    const bool reset_ran = std::any_of(
        snapshot.threads.begin(),
        snapshot.threads.end(),
        [](const RuntimeThread& thread) {
            return thread.last_completed_task_type == TaskType::RESET;
        });
    const bool peer_wait_marked = std::any_of(
        snapshot.threads.begin(),
        snapshot.threads.end(),
        [](const RuntimeThread& thread) {
            return thread.last_task_event == "reset-waiting";
        });
    const bool reseeded_entry =
        std::any_of(
            snapshot.p2_realtime_queue.begin(),
            snapshot.p2_realtime_queue.end(),
            [](const TaskRecord& record) {
                return record.type == TaskType::CAMERA_LISTENER_SERVICE ||
                    record.type == TaskType::MICROPHONE_LISTENER_SERVICE;
            });
    const bool note_present = std::any_of(
        notes.begin(),
        notes.end(),
        [](const std::string& note) {
            return note.find("world reset complete and input entry reseeded") !=
                std::string::npos;
        });

    CheckResult result;
    result.name = "P1 reset preempts multithread lanes and reseeds input";
    result.passed = reset_ran && peer_wait_marked && reseeded_entry && note_present;
    result.details.push_back(std::string("reset-ran=") + (reset_ran ? "yes" : "no"));
    result.details.push_back(std::string("peer-wait=") + (peer_wait_marked ? "yes" : "no"));
    result.details.push_back(std::string("reseeded-entry=") + (reseeded_entry ? "yes" : "no"));
    result.details.push_back(std::string("runtime-note=") + (note_present ? "present" : "missing"));

    stop_camera_bridge();
    stop_microphone_bridge();
    return result;
}

CheckResult verify_exit_requests_shutdown_and_stops_peer_dispatch() {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_REALTIME,
        "RealtimeCameraProbe"));
    submit_task(make_placeholder_task(
        TaskType::CAMERA,
        PriorityLevel::P2_FUNCTIONAL,
        "CameraLaneProbe"));
    submit_task(make_placeholder_task(
        TaskType::MICROPHONE,
        PriorityLevel::P2_FUNCTIONAL,
        "MicrophoneLaneProbe"));
    submit_task(make_exit_task());

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    const std::vector<std::string> notes = current_runtime_notes();

    const bool exit_ran = std::any_of(
        snapshot.threads.begin(),
        snapshot.threads.end(),
        [](const RuntimeThread& thread) {
            return thread.last_completed_task_type == TaskType::EXIT_APP;
        });
    const bool shutdown_set = shutdown_requested() && snapshot.shutdown_requested;
    const bool peer_wait_marked = std::any_of(
        snapshot.threads.begin(),
        snapshot.threads.end(),
        [](const RuntimeThread& thread) {
            return thread.last_task_event == "exit-waiting";
        });
    const bool note_present = std::any_of(
        notes.begin(),
        notes.end(),
        [](const std::string& note) {
            return note.find("visualization loop stop requested") != std::string::npos;
        });

    CheckResult result;
    result.name = "exit requests shutdown and stops peer dispatch";
    result.passed = exit_ran && shutdown_set && peer_wait_marked && note_present;
    result.details.push_back(std::string("exit-ran=") + (exit_ran ? "yes" : "no"));
    result.details.push_back(std::string("shutdown-set=") + (shutdown_set ? "yes" : "no"));
    result.details.push_back(std::string("peer-wait=") + (peer_wait_marked ? "yes" : "no"));
    result.details.push_back(std::string("runtime-note=") + (note_present ? "present" : "missing"));

    bootstrap_runtime();
    set_runtime_phase(RuntimePhase::READY);
    return result;
}

CheckResult verify_mode_collapse_keeps_general_work_runnable() {
    prepare_ready_runtime(false, false);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    submit_task(make_placeholder_task(
        TaskType::CHANGE_DANDELION,
        PriorityLevel::P2_FUNCTIONAL,
        "GeneralLaneProbe"));
    set_thread_mode(1);

    scheduler_tick();
    const SchedulerSnapshot snapshot = scheduler_snapshot();
    const RuntimeThread& t1 = snapshot.threads.at(0);
    const RuntimeThread& t2 = snapshot.threads.at(1);
    const RuntimeThread& t3 = snapshot.threads.at(2);

    CheckResult result;
    result.name = "mode collapse keeps runnable work on surviving single-thread path";
    result.passed =
        thread_completed_type(t1, TaskType::CHANGE_DANDELION) &&
        t2.last_task_event == "mode-retire" &&
        t3.last_task_event == "mode-retire";
    result.details.push_back("T1 last=" + task_type_name(t1.last_completed_task_type));
    result.details.push_back("T2 event=" + t2.last_task_event);
    result.details.push_back("T3 event=" + t3.last_task_event);
    return result;
}

CheckResult verify_mode_collapse_replaces_persistent_listener_with_burst() {
    prepare_ready_runtime(true, true);
    set_thread_mode(3);
    clear_task_queue(PriorityLevel::P1_SYSTEM);
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);

    RuntimeState& state = runtime_state();
    state.camera_device_available = true;
    state.camera_listener.device_available = true;

    submit_task(make_camera_listener_service_task());
    submit_task(make_microphone_listener_service_task());
    const SchedulerSnapshot before_collapse = scheduler_snapshot();

    set_thread_mode(1);
    const SchedulerSnapshot after_collapse = scheduler_snapshot();
    const std::vector<std::string> collapse_notes = current_runtime_notes();

    scheduler_tick();
    const SchedulerSnapshot after_tick = scheduler_snapshot();
    const std::vector<std::string> notes = current_runtime_notes();

    const bool had_persistent_before =
        snapshot_has_task_type(
            before_collapse.p2_realtime_queue,
            TaskType::CAMERA_LISTENER_SERVICE) &&
        snapshot_has_task_type(
            before_collapse.p2_realtime_queue,
            TaskType::MICROPHONE_LISTENER_SERVICE);
    const bool persistent_removed =
        !snapshot_has_task_type(
            after_collapse.p2_realtime_queue,
            TaskType::CAMERA_LISTENER_SERVICE) &&
        !snapshot_has_task_type(
            after_collapse.p2_realtime_queue,
            TaskType::MICROPHONE_LISTENER_SERVICE);
    const bool burst_seeded =
        snapshot_has_task_type(
            after_collapse.p2_realtime_queue,
            TaskType::CAMERA_LISTENER_BURST) &&
        !snapshot_has_task_type(
            after_collapse.p2_realtime_queue,
            TaskType::MICROPHONE_LISTENER_BURST);
    const bool hard_interrupt_noted = std::any_of(
        collapse_notes.begin(),
        collapse_notes.end(),
        [](const std::string& note) {
            return note.find("hard-interrupted") != std::string::npos &&
                note.find("multi-thread -> single-thread fallback") != std::string::npos;
        });
    const bool burst_soft_requeued =
        snapshot_has_task_type(
            after_tick.p2_realtime_queue,
            TaskType::CAMERA_LISTENER_BURST) &&
        std::any_of(
            notes.begin(),
            notes.end(),
            [](const std::string& note) {
                return note.find("Realtime slice yielded CameraListenerBurstTask") !=
                        std::string::npos ||
                    note.find("Realtime slice yielded MicrophoneListenerBurstTask") !=
                        std::string::npos;
            });

    CheckResult result;
    result.name =
        "mode collapse hard-stops persistent listener service and seeds burst listeners";
    result.passed =
        had_persistent_before &&
        persistent_removed &&
        burst_seeded &&
        hard_interrupt_noted &&
        burst_soft_requeued;
    result.details.push_back(
        std::string("persistent-before=") + (had_persistent_before ? "yes" : "no"));
    result.details.push_back(
        std::string("persistent-removed=") + (persistent_removed ? "yes" : "no"));
    result.details.push_back(
        std::string("burst-seeded=") + (burst_seeded ? "yes" : "no"));
    result.details.push_back(
        std::string("hard-note=") + (hard_interrupt_noted ? "present" : "missing"));
    result.details.push_back(
        std::string("burst-soft-requeue=") + (burst_soft_requeued ? "yes" : "no"));
    return result;
}

}  // namespace

int main() {
    const std::vector<CheckResult> results = {
        verify_realtime_queue_precedes_interactive_queue(),
        verify_single_thread_idle_reseed_uses_burst_entry(),
        verify_single_thread_camera_handoff_swaps_to_microphone_burst(),
        verify_single_thread_full_cycle_reseeds_camera_burst(),
        verify_single_thread_fallback_cycle_reseeds_camera_burst(),
        verify_mode_collapse_preserves_generate_phase_order(),
        verify_mode_collapse_preserves_breeze_phase_order(),
        verify_listener_runtime_snapshot_fields(),
        verify_two_thread_camera_plus_general(),
        verify_three_thread_camera_microphone_general(),
        verify_two_thread_prefers_camera_long_and_microphone_short_listener(),
        verify_two_thread_short_microphone_listener_does_not_auto_renew(),
        verify_general_lane_owns_batch_feeder(),
        verify_multithread_focus_is_window_only(),
        verify_multithread_camera_reseed_while_queues_active(),
        verify_multithread_l1_realtime_cuts_across_p2_slots(),
        verify_multithread_microphone_handoff_overlaps_p3_drain(),
        verify_p1_reset_preempts_multithread_lanes(),
        verify_exit_requests_shutdown_and_stops_peer_dispatch(),
        verify_mode_collapse_keeps_general_work_runnable(),
        verify_mode_collapse_replaces_persistent_listener_with_burst(),
    };

    bool all_passed = true;
    for (const CheckResult& result : results) {
        std::cout << (result.passed ? "PASS " : "FAIL ") << result.name << "\n";
        for (const std::string& detail : result.details) {
            std::cout << "  " << detail << "\n";
        }
        all_passed = all_passed && result.passed;
    }

    return all_passed ? 0 : 1;
}
