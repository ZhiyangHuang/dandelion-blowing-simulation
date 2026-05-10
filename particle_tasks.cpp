#include "thread.h"
#include "scheduler_task_support.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

BatchParticleExecutionTask::BatchParticleExecutionTask() {
    type = TaskType::BATCH_PARTICLE_EXECUTION;
    priority = PriorityLevel::P2_FUNCTIONAL;
    name = "BatchParticleExecutionTask";
    support_resume = true;
}

void BatchParticleExecutionTask::execute() {
    run_cycle(false);
}

void BatchParticleExecutionTask::resume() {
    run_cycle(true);
}

void BatchParticleExecutionTask::run_cycle(bool resumed) {
    ++tick_count_;
    if (!started_) {
        started_ = true;
    }
    decay_accumulator_ms_ += scheduler_task_support::kRrQuantumMs;
    bool stage_finished = false;
    with_shared_state_write(true, true, true, [this, resumed, &stage_finished]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();
        bool power_decayed = false;
        int cleaned_particles = 0;

        if (decay_accumulator_ms_ >= 1000 && state_ref.world.power > 0.1f) {
            state_ref.world.power = std::max(0.1f, state_ref.world.power - 0.01f);
            decay_accumulator_ms_ -= 1000;
            power_decayed = true;
        }

        for (ParticleRenderData& particle : data.particles) {
            const bool finished_detached =
                !particle.active &&
                !particle.attached &&
                particle.source_task_id == -1 &&
                (particle.status == "particle-finished" ||
                 particle.status == "particle-boundary-stop");
            if (!finished_detached) {
                continue;
            }

            particle.x = -1.0f;
            particle.y = -1.0f;
            particle.status = "particle-cleaned";
            cleaned_particles++;
        }

        scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);

        const bool has_cleanup_pending = std::any_of(
            data.particles.begin(),
            data.particles.end(),
            [](const ParticleRenderData& particle) {
                return !particle.active &&
                    !particle.attached &&
                    particle.source_task_id == -1 &&
                    (particle.status == "particle-finished" ||
                     particle.status == "particle-boundary-stop");
            });
        const bool has_active_particle_motion = std::any_of(
            data.particles.begin(),
            data.particles.end(),
            [](const ParticleRenderData& particle) {
                return !particle.attached && particle.active;
            });
        stage_finished =
            !has_cleanup_pending &&
            !has_active_particle_motion &&
            state_ref.world.queued_particle_tasks == 0;

        data.wind_layer.active = false;
        data.wind_layer.power = state_ref.world.power;
        data.wind_layer.tick_count = tick_count_;
        if (cleaned_particles > 0) {
            data.wind_layer.status = "world-tick-cleanup";
        } else if (power_decayed) {
            data.wind_layer.status = "world-tick-decay";
        } else if (tick_count_ == 1) {
            data.wind_layer.status = "world-tick-start";
        } else {
            data.wind_layer.status = resumed ? "batch-resume" : "batch-execute";
        }
        data.ui.power = state_ref.world.power;
        if (cleaned_particles > 0) {
            data.ui.batch_task_status =
                "BATCH cleaned " + std::to_string(cleaned_particles) + " particle render records";
        } else if (power_decayed) {
            data.ui.batch_task_status =
                "BATCH decayed power to " +
                std::to_string(static_cast<int>(state_ref.world.power * 100.0f) / 100.0f);
        } else if (tick_count_ == 1) {
            data.ui.batch_task_status = "BATCH recorded initial world tick";
        } else if (stage_finished) {
            data.ui.batch_task_status = "BATCH finished world pass";
        } else {
            data.ui.batch_task_status = "BATCH tick " + std::to_string(tick_count_);
        }
    });

    if (stage_finished) {
        push_runtime_note("BatchParticleExecutionTask: stage completed.");
    }

    state = stage_finished ? TaskState::FINISHED : TaskState::RUNNING;
}

SingleParticleTask::SingleParticleTask(int particle_slot,
                                       int generation_index,
                                       int ownership_token)
    : particle_slot_(particle_slot),
      generation_index_(generation_index),
      ownership_token_(ownership_token) {
    type = TaskType::SINGLE_PARTICLE;
    priority = PriorityLevel::P3_PARTICLE;
    name = "SingleParticleTask";
    support_resume = true;
}

void SingleParticleTask::execute() {
    run_cycle(false);
}

void SingleParticleTask::resume() {
    run_cycle(true);
}

void SingleParticleTask::run_cycle(bool resumed) {
    ++cycle_count_;
    completed_ = false;
    with_shared_state_write(true, false, true, [this, resumed]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        if (particle_slot_ >= 0 &&
            particle_slot_ < static_cast<int>(data.particles.size())) {
            ParticleRenderData& particle = data.particles[static_cast<std::size_t>(particle_slot_)];
            if (particle.ownership_token != ownership_token_) {
                completed_ = true;
                scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
                data.ui.particle_task_status =
                    "P3 task " + std::to_string(id) + " lost slot ownership";
                return;
            }
            if (!initialized_) {
                x_ = particle.x;
                y_ = particle.y;
                const float from_center_x = x_ - state_ref.world.dandelion_x;
                const float from_center_y = y_ - state_ref.world.dandelion_y;
                const float from_mouth_x = x_ - state_ref.world.mouth_x;
                const float from_mouth_y = y_ - state_ref.world.mouth_y;
                float base_x = from_center_x;
                float base_y = from_center_y;
                if (std::abs(base_x) + std::abs(base_y) < 0.001f) {
                    base_x = from_mouth_x;
                    base_y = from_mouth_y;
                }
                if (std::abs(base_x) + std::abs(base_y) < 0.001f) {
                    base_x = 1.0f;
                    base_y = 0.0f;
                }

                float length = std::sqrt(base_x * base_x + base_y * base_y);
                if (length <= 0.0001f) {
                    length = 1.0f;
                }
                base_x /= length;
                base_y /= length;

                const float spread = (static_cast<float>(generation_index_) - 2.0f) * 0.18f;
                dir_x_ = base_x + (-base_y * spread);
                dir_y_ = base_y + (base_x * spread);

                float dir_length = std::sqrt(dir_x_ * dir_x_ + dir_y_ * dir_y_);
                if (dir_length <= 0.0001f) {
                    dir_x_ = base_x;
                    dir_y_ = base_y;
                    dir_length = 1.0f;
                }
                dir_x_ /= dir_length;
                dir_y_ /= dir_length;

                const float velocity_pixels_per_second =
                    std::clamp(state_ref.world.power, 0.1f, 10.0f) *
                    scheduler_task_support::kVelocityPerPowerPixelsPerSecond;
                distance_per_tick_ =
                    velocity_pixels_per_second *
                    (static_cast<float>(scheduler_task_support::kRrQuantumMs) / 1000.0f) /
                    scheduler_task_support::kNormalizedWorldPixels;
                initialized_ = true;
            }

            x_ += dir_x_ * distance_per_tick_;
            y_ += dir_y_ * distance_per_tick_;
            particle.source_task_id = id;
            particle.x = x_;
            particle.y = y_;
            particle.active = true;
            particle.status = resumed ? "particle-resume" : "particle-execute";

            const bool boundary_stop =
                x_ < 0.0f || x_ > 1.0f || y_ < 0.0f || y_ > 1.0f;

            if (boundary_stop) {
                completed_ = true;
                particle.active = false;
                particle.attached = false;
                particle.source_task_id = -1;
                particle.status = "particle-boundary-stop";
                scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
                data.ui.particle_task_status =
                    "P3 task " + std::to_string(id) + " boundary stop cleanup";
            } else {
                scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
                data.ui.particle_task_status =
                    "P3 task " + std::to_string(id) +
                    " step " + std::to_string(cycle_count_);
            }
        } else {
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status =
                "P3 task " + std::to_string(id) + " lost particle slot";
        }
    });

    state = completed_ ? TaskState::FINISHED : TaskState::RUNNING;
}

std::unique_ptr<Task> make_batch_particle_execution_task() {
    return std::make_unique<BatchParticleExecutionTask>();
}

std::unique_ptr<Task> make_single_particle_task(int particle_slot,
                                                int generation_index,
                                                int ownership_token) {
    return std::make_unique<SingleParticleTask>(
        particle_slot,
        generation_index,
        ownership_token);
}
