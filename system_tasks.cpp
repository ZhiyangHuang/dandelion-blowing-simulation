#include "thread.h"
#include "runtime_orchestration.h"
#include "scheduler_task_support.h"

#include <memory>
#include <utility>

namespace {

int g_dandelion_layout_revision = 0;

void reset_runtime_input_devices() {
    push_runtime_note("ResetTask: stopping microphone listener.");
    stop_microphone_bridge();
    push_runtime_note("ResetTask: stopping camera listener.");
    stop_camera_bridge();

    push_runtime_note("ResetTask: starting camera listener.");
    start_camera_bridge();
    push_runtime_note("ResetTask: starting microphone listener.");
    start_microphone_bridge();
    refresh_bridge_inputs();

    const RuntimeState& state = current_runtime_state();
    push_runtime_note(
        "ResetTask: listeners refreshed -> CAM " +
        std::string(state.camera_listener.enabled ? "enabled" : "disabled") +
        " / MIC " +
        std::string(state.microphone_listener.enabled ? "enabled" : "disabled"));
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

std::unique_ptr<Task> PlaceholderTask::clone_for_requeue() const {
    return std::make_unique<PlaceholderTask>(*this);
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

std::unique_ptr<Task> StartTask::clone_for_requeue() const {
    return std::make_unique<StartTask>(*this);
}

ResetTask::ResetTask() {
    type = TaskType::RESET;
    priority = PriorityLevel::P1_SYSTEM;
    name = "ResetTask";
    support_resume = false;
}

void ResetTask::execute() {
    set_runtime_phase(RuntimePhase::RESETTING);
    push_runtime_note("ResetTask: clearing P2/P3/P4 queues.");
    clear_task_queue(PriorityLevel::P2_REALTIME);
    clear_task_queue(PriorityLevel::P2_FUNCTIONAL);
    clear_task_queue(PriorityLevel::P3_PARTICLE);
    push_runtime_note("ResetTask: resetting world state.");
    reset_simulation_world();
    reset_runtime_input_devices();
    set_runtime_phase(RuntimePhase::READY);
    scheduler_task_support::seed_input_entry_task_if_idle();
    scheduler_task_support::seed_mode_specific_short_listener_if_needed("reset");
    push_runtime_note("ResetTask: world reset complete and input entry reseeded.");
    state = TaskState::FINISHED;
}

std::unique_ptr<Task> ResetTask::clone_for_requeue() const {
    return std::make_unique<ResetTask>(*this);
}

ExitTask::ExitTask() {
    type = TaskType::EXIT_APP;
    priority = PriorityLevel::P1_SYSTEM;
    name = "ExitTask";
    support_resume = false;
}

void ExitTask::execute() {
    push_runtime_note("ExitTask: stopping microphone listener.");
    stop_microphone_bridge();
    push_runtime_note("ExitTask: stopping camera listener.");
    stop_camera_bridge();
    push_runtime_note("ExitTask: requesting runtime shutdown.");
    request_shutdown();
    set_visualization_running(false);
    push_runtime_note("ExitTask: visualization loop stop requested.");
    state = TaskState::FINISHED;
}

std::unique_ptr<Task> ExitTask::clone_for_requeue() const {
    return std::make_unique<ExitTask>(*this);
}

BreezeTask::BreezeTask() {
    type = TaskType::BREEZE;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "BreezeTask";
    support_resume = false;
}

void BreezeTask::execute() {
    bool created_particle = false;
    with_shared_state_write(true, true, true, [this, &created_particle]() {
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
            submit_task(make_single_particle_task(
                slot,
                1,
                particle.ownership_token,
                state_ref.world.mouth_x,
                state_ref.world.mouth_y));
            particle.visual_alpha = 1.0f;
            particle.active = true;
            particle.attached = false;
            particle.fade_steps_remaining = 0;
            particle.status = "breeze-queued";
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status = "BREEZE queued fallback P3 move task";
            created_particle = true;
        } else {
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status = "BREEZE had no particle slot";
        }

        runtime_orchestration::advance_particle_root_flow(
            state_ref,
            TaskType::BREEZE,
            PriorityLevel::P2_FUNCTIONAL,
            id,
            OrchestrationNode::PARTICLE_GENERATE,
            created_particle ? OrchestrationStatus::FALLBACK : OrchestrationStatus::COMPLETED,
            created_particle ? OrchestrationEvent::BLOW_FALLBACK
                             : OrchestrationEvent::PARTICLE_GENERATE_EMPTY,
            created_particle);

        data.ui.generate_task_status =
            created_particle ? "BREEZE fallback queued 1 P3 move task"
                             : "BREEZE fallback queued 0 P3 move tasks";
    });

    if (created_particle) {
        scheduler_task_support::queue_batch_stage_if_needed();
    }
    push_runtime_note(created_particle
        ? "BreezeTask: fallback wind queued one P3 move task."
        : "BreezeTask: fallback wind ran without queuing a P3 move task.");
    state = TaskState::FINISHED;
}

std::unique_ptr<Task> BreezeTask::clone_for_requeue() const {
    return std::make_unique<BreezeTask>(*this);
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

    with_shared_state_write(true, true, true, [this, revision]() {
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
        runtime_orchestration::advance_particle_root_flow(
            state_ref,
            TaskType::CHANGE_DANDELION,
            PriorityLevel::P2_FUNCTIONAL,
            id,
            OrchestrationNode::CHANGE_DANDELION,
            OrchestrationStatus::COMPLETED,
            OrchestrationEvent::CHANGE_DANDELION,
            false);

        scheduler_task_support::rebuild_particle_ring_for_world(next_x, next_y, 100);
        scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
    });

    clear_task_queue(PriorityLevel::P3_PARTICLE);
    push_runtime_note("ChangeDandelionTask: rebuilt dandelion cluster and reset particle state.");
    state = TaskState::FINISHED;
}

std::unique_ptr<Task> ChangeDandelionTask::clone_for_requeue() const {
    return std::make_unique<ChangeDandelionTask>(*this);
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
