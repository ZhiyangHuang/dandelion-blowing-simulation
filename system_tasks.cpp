#include "thread.h"
#include "scheduler_task_support.h"

#include <memory>
#include <utility>

namespace {

int g_dandelion_layout_revision = 0;

void mark_camera_bridge_unavailable_and_stop(const std::string& status_text) {
    CameraBridgeState unavailable_state = runtime_state().camera_bridge;
    unavailable_state.device_unavailable = true;
    unavailable_state.bridge_connected = false;
    unavailable_state.sample_ready = false;
    unavailable_state.status_text = status_text;
    stop_camera_bridge();
    RuntimeState& state_ref = runtime_state();
    state_ref.camera_bridge = unavailable_state;
    state_ref.camera_device_available = false;
    state_ref.camera_available = false;
}

void mark_microphone_bridge_unavailable_and_stop(const std::string& status_text) {
    MicrophoneBridgeState unavailable_state = runtime_state().microphone_bridge;
    unavailable_state.device_unavailable = true;
    unavailable_state.bridge_connected = false;
    unavailable_state.sample_ready = false;
    unavailable_state.status_text = status_text;
    stop_microphone_bridge();
    RuntimeState& state_ref = runtime_state();
    state_ref.microphone_bridge = unavailable_state;
    state_ref.microphone_device_available = false;
    state_ref.microphone_available = false;
}

void reset_runtime_input_devices() {
    stop_microphone_bridge();
    stop_camera_bridge();

    start_camera_bridge();
    start_microphone_bridge();
    refresh_bridge_inputs();

    RuntimeState& state_ref = runtime_state();
    if (state_ref.camera_bridge.device_unavailable) {
        mark_camera_bridge_unavailable_and_stop(state_ref.camera_bridge.status_text);
        push_runtime_note("SystemTasks: camera device unavailable during reset.");
    }
    if (state_ref.microphone_bridge.device_unavailable) {
        mark_microphone_bridge_unavailable_and_stop(state_ref.microphone_bridge.status_text);
        push_runtime_note("SystemTasks: microphone device unavailable during reset.");
    }
}

}  // namespace

PlaceholderTask::PlaceholderTask(TaskType task_type,
                                 PriorityLevel task_priority,
                                 std::string task_name,
                                 bool resumable) {
    type = task_type;
    priority = task_priority;
    name = std::move(task_name);
    support_resume = resumable;
}

void PlaceholderTask::execute() {
    state = TaskState::FINISHED;
}

StartTask::StartTask() {
    type = TaskType::START;
    priority = PriorityLevel::P1_SYSTEM;
    name = "StartTask";
    support_resume = false;
}

void StartTask::execute() {
    set_runtime_phase(RuntimePhase::STARTING);
    push_runtime_note("StartTask: runtime bootstrap started.");
    submit_task(make_reset_task());
    state = TaskState::FINISHED;
}

ResetTask::ResetTask() {
    type = TaskType::RESET;
    priority = PriorityLevel::P1_SYSTEM;
    name = "ResetTask";
    support_resume = false;
}

void ResetTask::execute() {
    set_runtime_phase(RuntimePhase::RESETTING);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    reset_simulation_world();
    reset_runtime_input_devices();
    set_runtime_phase(RuntimePhase::READY);
    scheduler_task_support::seed_input_entry_task_if_idle();
    push_runtime_note("ResetTask: world reset and input entry reseeded.");
    state = TaskState::FINISHED;
}

ExitTask::ExitTask() {
    type = TaskType::EXIT_APP;
    priority = PriorityLevel::P1_SYSTEM;
    name = "ExitTask";
    support_resume = false;
}

void ExitTask::execute() {
    stop_microphone_bridge();
    stop_camera_bridge();
    request_shutdown();
    set_visualization_running(false);
    push_runtime_note("ExitTask: shutdown requested.");
    state = TaskState::FINISHED;
}

BreezeTask::BreezeTask() {
    type = TaskType::BREEZE;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "BreezeTask";
    support_resume = false;
}

void BreezeTask::execute() {
    bool created_particle = false;
    with_shared_state_write(true, true, true, [&created_particle]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        state_ref.world.power = 5.0f;
        data.wind_layer.active = true;
        data.wind_layer.power = state_ref.world.power;
        data.wind_layer.status = "breeze-fallback";
        data.ui.power = state_ref.world.power;

        const std::vector<int> spawn_slots =
            scheduler_task_support::collect_spawnable_particle_slots(1);
        if (state_ref.world.remaining_particles > 0 &&
            !spawn_slots.empty() &&
            !has_queued_task(TaskType::SINGLE_PARTICLE, PriorityLevel::P3_PARTICLE)) {
            const int slot = spawn_slots.front();
            ParticleRenderData& particle = data.particles[static_cast<std::size_t>(slot)];
            particle.ownership_token++;
            submit_task(make_single_particle_task(slot, 1, particle.ownership_token));
            particle.active = true;
            particle.attached = false;
            particle.status = "breeze-queued";
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status = "BREEZE queued fallback particle";
            created_particle = true;
        } else {
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status = "BREEZE had no particle slot";
        }

        data.ui.generate_task_status =
            created_particle ? "BREEZE fallback created 1 particle task"
                             : "BREEZE fallback created 0 particle tasks";
    });

    if (created_particle) {
        scheduler_task_support::queue_batch_stage_if_needed();
    }
    push_runtime_note(created_particle
        ? "BreezeTask: fallback wind queued one particle task."
        : "BreezeTask: fallback wind ran without queuing a particle.");
    state = TaskState::FINISHED;
}

ChangeDandelionTask::ChangeDandelionTask() {
    type = TaskType::CHANGE_DANDELION;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "ChangeDandelionTask";
    support_resume = false;
}

void ChangeDandelionTask::execute() {
    ++g_dandelion_layout_revision;
    const int revision = g_dandelion_layout_revision % 3;

    with_shared_state_write(true, true, true, [revision]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        float next_x = 0.5f;
        float next_y = 0.5f;
        if (revision == 1) {
            next_x = 0.42f;
            next_y = 0.52f;
        } else if (revision == 2) {
            next_x = 0.58f;
            next_y = 0.48f;
        }

        state_ref.world.dandelion_x = next_x;
        state_ref.world.dandelion_y = next_y;
        state_ref.world.mouth_x = next_x;
        state_ref.world.mouth_y = next_y;
        state_ref.world.remaining_particles = 100;
        state_ref.world.queued_particle_tasks = 0;
        state_ref.world.power = 0.1f;

        data.camera_layer.mouth_x = next_x;
        data.camera_layer.mouth_y = next_y;
        data.wind_layer.active = false;
        data.wind_layer.power = state_ref.world.power;
        data.wind_layer.status = "idle";
        data.ui.power = state_ref.world.power;
        data.ui.generate_task_status = "CHANGE rebuilt dandelion";
        data.ui.particle_task_status = "CHANGE reset particle ring";

        scheduler_task_support::rebuild_particle_ring_for_world(next_x, next_y, 100);
        scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
    });

    clear_task_queue(PriorityLevel::P3_PARTICLE);
    push_runtime_note("ChangeDandelionTask: rebuilt dandelion cluster and reset particle state.");
    state = TaskState::FINISHED;
}

std::unique_ptr<Task> make_placeholder_task(TaskType type,
                                            PriorityLevel priority,
                                            const std::string& name,
                                            bool support_resume) {
    return std::make_unique<PlaceholderTask>(type, priority, name, support_resume);
}

std::unique_ptr<Task> make_start_task() {
    return std::make_unique<StartTask>();
}

std::unique_ptr<Task> make_reset_task() {
    return std::make_unique<ResetTask>();
}

std::unique_ptr<Task> make_exit_task() {
    return std::make_unique<ExitTask>();
}

std::unique_ptr<Task> make_breeze_task() {
    return std::make_unique<BreezeTask>();
}

std::unique_ptr<Task> make_change_dandelion_task() {
    return std::make_unique<ChangeDandelionTask>();
}
