#include "thread.h"
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
            submit_task(make_single_particle_task(slot, index + 1, particle.ownership_token));
            particle.active = true;
            particle.attached = false;
            particle.status = "queued";
            created++;
        }

        scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
        data.ui.generate_task_status =
            "GENERATE created " + std::to_string(created) + " particle tasks";
        data.ui.particle_task_status =
            created > 0 ? "P3 queue populated" : "P3 queue unchanged";
    });

    if (created > 0) {
        scheduler_task_support::queue_batch_stage_if_needed();
    }
    push_runtime_note(
        "GenerateParticleTask: queued " + std::to_string(created) + " particle task records.");
    state = TaskState::FINISHED;
}

std::unique_ptr<Task> make_generate_particle_task() {
    return std::make_unique<GenerateParticleTask>();
}
