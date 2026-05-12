#include "thread.h"
#include "runtime_orchestration.h"
#include "scheduler_task_support.h"

#include <algorithm>
#include <memory>
#include <string>

GenerateParticleTask::GenerateParticleTask() {
    type = TaskType::GENERATE_PARTICLE;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "GenerateParticleTask";
    support_resume = false;
}

int GenerateParticleTask::compute_spawn_count(float power) const {
    if (power < 0.20f) {
        return 1;
    }
    if (power < 0.30f) {
        return 2;
    }
    if (power < 0.40f) {
        return 3;
    }
    if (power < 0.50f) {
        return 4;
    }
    return 5;
}

void GenerateParticleTask::execute() {
    int created = 0;
    with_shared_state_write(true, true, true, [this, &created]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        const int spawn_target = compute_spawn_count(state_ref.world.power);
        const std::vector<int> spawn_slots =
            scheduler_task_support::collect_spawnable_particle_slots(
                std::min(spawn_target, state_ref.world.remaining_particles));

        for (int index = 0; index < static_cast<int>(spawn_slots.size()); ++index) {
            const int slot = spawn_slots[static_cast<std::size_t>(index)];
            ParticleRenderData& particle = data.particles[static_cast<std::size_t>(slot)];
            particle.ownership_token++;
            submit_task(make_single_particle_task(
                slot,
                index + 1,
                particle.ownership_token,
                state_ref.world.mouth_x,
                state_ref.world.mouth_y));
            particle.visual_alpha = 1.0f;
            particle.active = true;
            particle.attached = false;
            particle.fade_steps_remaining = 0;
            particle.status = "queued";
            created++;
        }

        scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
        runtime_orchestration::advance_particle_root_flow(
            state_ref,
            TaskType::GENERATE_PARTICLE,
            PriorityLevel::P2_FUNCTIONAL,
            id,
            OrchestrationNode::PARTICLE_GENERATE,
            created > 0 ? OrchestrationStatus::HANDOFF : OrchestrationStatus::COMPLETED,
            created > 0 ? OrchestrationEvent::PARTICLE_GENERATE
                        : OrchestrationEvent::PARTICLE_GENERATE_EMPTY,
            created > 0);
        data.ui.generate_task_status =
            "GENERATE queued " + std::to_string(created) + " P3 move tasks";
        data.ui.particle_task_status =
            created > 0 ? "P3 move queue populated" : "P3 move queue unchanged";
    });

    if (created > 0) {
        scheduler_task_support::queue_batch_stage_if_needed();
    }
    push_runtime_note(
        "GenerateParticleTask: queued " + std::to_string(created) + " P3 move tasks.");
    state = TaskState::FINISHED;
}

std::unique_ptr<Task> GenerateParticleTask::clone_for_requeue() const {
    return std::make_unique<GenerateParticleTask>(*this);
}

std::unique_ptr<Task> make_generate_particle_task() {
    return std::make_unique<GenerateParticleTask>();
}
