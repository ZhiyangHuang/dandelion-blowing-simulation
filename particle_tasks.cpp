#include "thread.h"
#include "runtime_orchestration.h"
#include "scheduler_task_support.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace {

constexpr int kParticleFadeSteps = 6;

std::string batch_host_status_label(int tick_count,
                                    bool resumed,
                                    bool ran_particle_rr,
                                    bool stage_finished) {
    if (stage_finished) {
        return "BATCH host drained P3 stage";
    }
    if (ran_particle_rr) {
        return std::string("BATCH host RR handoff ") +
            (resumed ? "resume " : "execute ") +
            "tick " + std::to_string(tick_count);
    }
    if (tick_count == 1) {
        return "BATCH host opened throughput stage";
    }
    return std::string("BATCH host tick ") + std::to_string(tick_count);
}

bool particle_fade_active(const ParticleRenderData& particle) {
    return !particle.active &&
        !particle.attached &&
        (particle.status == "particle-fade-queued" ||
         particle.status == "particle-fade-execute" ||
         particle.status == "particle-fade-resume");
}

}  // namespace

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

std::unique_ptr<Task> BatchParticleExecutionTask::clone_for_requeue() const {
    return std::make_unique<BatchParticleExecutionTask>(*this);
}

void BatchParticleExecutionTask::run_cycle(bool resumed) {
    ++tick_count_;
    if (!started_) {
        started_ = true;
    }
    const long long now_ms = scheduler_task_support::current_time_ms();
    if (last_decay_timestamp_ms_ == 0) {
        last_decay_timestamp_ms_ = now_ms;
    }
    decay_accumulator_ms_ += std::max(0LL, now_ms - last_decay_timestamp_ms_);
    last_decay_timestamp_ms_ = now_ms;
    bool power_decayed = false;
    with_shared_state_write(true, true, true, [this, &power_decayed]() {
        RuntimeState& state_ref = runtime_state();

        if (decay_accumulator_ms_ >= 1000 && state_ref.world.power > 0.1f) {
            const int decay_steps = static_cast<int>(decay_accumulator_ms_ / 1000);

            state_ref.world.power = std::max(
                0.1f,
                state_ref.world.power - 0.01f * static_cast<float>(decay_steps));

            decay_accumulator_ms_  -= decay_steps * 1000;
            power_decayed = true;
        }
    });

    const bool ran_particle_rr = scheduler_task_support::run_particle_rr_slice_from_batch();
    bool stage_finished = false;
    with_shared_state_write(true, true, true, [this, resumed, power_decayed, ran_particle_rr, &stage_finished]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);

        stage_finished = state_ref.world.queued_particle_tasks == 0;

        data.wind_layer.active = false;
        data.wind_layer.power = state_ref.world.power;
        data.wind_layer.tick_count = tick_count_;
        if (power_decayed) {
            data.wind_layer.status = "world-tick-decay";
        } else if (ran_particle_rr) {
            data.wind_layer.status = resumed ? "batch-rr-resume" : "batch-rr-execute";
        } else if (tick_count_ == 1) {
            data.wind_layer.status = "world-tick-start";
        } else {
            data.wind_layer.status = resumed ? "batch-resume" : "batch-execute";
        }
        data.ui.power = state_ref.world.power;
        data.ui.batch_task_status = batch_host_status_label(
            tick_count_,
            resumed,
            ran_particle_rr,
            stage_finished);
        runtime_orchestration::advance_particle_root_flow(
            state_ref,
            TaskType::BATCH_PARTICLE_EXECUTION,
            PriorityLevel::P2_FUNCTIONAL,
            id,
            OrchestrationNode::PARTICLE_BATCH,
            stage_finished ? OrchestrationStatus::DRAINED : OrchestrationStatus::ACTIVE,
            stage_finished ? OrchestrationEvent::PARTICLE_STAGE_DRAINED
                           : (ran_particle_rr ? OrchestrationEvent::PARTICLE_BATCH_HANDOFF
                                              : OrchestrationEvent::PARTICLE_BATCH_TICK),
            !stage_finished);
        if (power_decayed) {
            data.ui.particle_task_status =
                "P3 throughput stage power decay -> " +
                std::to_string(static_cast<int>(state_ref.world.power * 100.0f) / 100.0f);
        }
    });

    if (stage_finished) {
        push_runtime_note("BatchParticleExecutionTask: feeder drained current P3 stage.");
    }

    state = stage_finished ? TaskState::FINISHED : TaskState::INTERRUPTED;
}

SingleParticleTask::SingleParticleTask(int particle_slot,
                                       int generation_index,
                                       int ownership_token,
                                       float mouth_x,
                                       float mouth_y)
    : particle_slot_(particle_slot),
      generation_index_(generation_index),
      ownership_token_(ownership_token),
      mouth_x_(mouth_x),
      mouth_y_(mouth_y) {
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

std::unique_ptr<Task> SingleParticleTask::clone_for_requeue() const {
    return std::make_unique<SingleParticleTask>(*this);
}

void SingleParticleTask::run_cycle(bool resumed) {
    ++cycle_count_;
    completed_ = false;
    bool should_queue_fade_task = false;
    with_shared_state_write(true, false, true, [this, resumed, &should_queue_fade_task]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        if (particle_slot_ >= 0 &&
            particle_slot_ < static_cast<int>(data.particles.size())) {
            ParticleRenderData& particle = data.particles[static_cast<std::size_t>(particle_slot_)];
            if (particle.ownership_token != ownership_token_) {
                completed_ = true;
                runtime_orchestration::advance_particle_root_flow(
                    state_ref,
                    TaskType::SINGLE_PARTICLE,
                    PriorityLevel::P3_PARTICLE,
                    id,
                    OrchestrationNode::PARTICLE_MOVE,
                    OrchestrationStatus::ERROR_STATE,
                    OrchestrationEvent::PARTICLE_MOVE_LOST_OWNERSHIP,
                    false);
                scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
                data.ui.particle_task_status =
                    "P3 move task " + std::to_string(id) + " lost slot ownership";
                return;
            }
            if (!initialized_) {
                x_ = particle.x;
                y_ = particle.y;
                // Primary movement should follow the blow direction away from the mouth
                // snapshot that created this task, not just the radial flower layout.
                float base_x = x_ - mouth_x_;
                float base_y = y_ - mouth_y_;
                if (std::abs(base_x) + std::abs(base_y) < 0.001f) {
                    base_x = x_ - state_ref.world.dandelion_x;
                    base_y = y_ - state_ref.world.dandelion_y;
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

                const float radial_x = x_ - state_ref.world.dandelion_x;
                const float radial_y = y_ - state_ref.world.dandelion_y;
                float tangent_x = -radial_y;
                float tangent_y = radial_x;
                float tangent_length = std::sqrt(tangent_x * tangent_x + tangent_y * tangent_y);
                if (tangent_length <= 0.0001f) {
                    tangent_x = -base_y;
                    tangent_y = base_x;
                    tangent_length = 1.0f;
                }
                tangent_x /= tangent_length;
                tangent_y /= tangent_length;

                const float spread = (static_cast<float>(generation_index_) - 2.0f) * 0.12f;
                dir_x_ = base_x + (tangent_x * spread);
                dir_y_ = base_y + (tangent_y * spread);

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
                particle.fade_steps_remaining = kParticleFadeSteps;
                particle.visual_alpha = 1.0f;
                particle.status = "particle-fade-queued";
                runtime_orchestration::advance_particle_root_flow(
                    state_ref,
                    TaskType::SINGLE_PARTICLE,
                    PriorityLevel::P3_PARTICLE,
                    id,
                    OrchestrationNode::PARTICLE_FADE,
                    OrchestrationStatus::HANDOFF,
                    OrchestrationEvent::PARTICLE_MOVE_BOUNDARY_STOP,
                    true);
                scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
                data.ui.particle_task_status =
                    "P3 move task " + std::to_string(id) + " boundary stop -> fade lane";
                should_queue_fade_task = true;
            } else {
                runtime_orchestration::advance_particle_root_flow(
                    state_ref,
                    TaskType::SINGLE_PARTICLE,
                    PriorityLevel::P3_PARTICLE,
                    id,
                    OrchestrationNode::PARTICLE_MOVE,
                    OrchestrationStatus::ACTIVE,
                    OrchestrationEvent::PARTICLE_MOVE_EXECUTE,
                    true);
                scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
                data.ui.particle_task_status =
                    "P3 move task " + std::to_string(id) +
                    " step " + std::to_string(cycle_count_);
            }
        } else {
            runtime_orchestration::advance_particle_root_flow(
                state_ref,
                TaskType::SINGLE_PARTICLE,
                PriorityLevel::P3_PARTICLE,
                id,
                OrchestrationNode::PARTICLE_MOVE,
                OrchestrationStatus::ERROR_STATE,
                OrchestrationEvent::PARTICLE_MOVE_LOST_SLOT,
                false);
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status =
                "P3 move task " + std::to_string(id) + " lost particle slot";
        }
    });

    if (should_queue_fade_task) {
        submit_task(make_particle_fade_task(particle_slot_, ownership_token_));
    }

    state = completed_ ? TaskState::FINISHED : TaskState::INTERRUPTED;
}

ParticleFadeTask::ParticleFadeTask(int particle_slot, int ownership_token)
    : particle_slot_(particle_slot),
      ownership_token_(ownership_token) {
    type = TaskType::FADE_PARTICLE;
    priority = PriorityLevel::P3_PARTICLE;
    name = "ParticleFadeTask";
    support_resume = true;
}

void ParticleFadeTask::execute() {
    run_cycle(false);
}

void ParticleFadeTask::resume() {
    run_cycle(true);
}

std::unique_ptr<Task> ParticleFadeTask::clone_for_requeue() const {
    return std::make_unique<ParticleFadeTask>(*this);
}

void ParticleFadeTask::run_cycle(bool resumed) {
    ++cycle_count_;
    completed_ = false;
    with_shared_state_write(true, false, true, [this, resumed]() {
        RuntimeState& state_ref = runtime_state();
        RenderData& data = render_data();

        if (particle_slot_ < 0 ||
            particle_slot_ >= static_cast<int>(data.particles.size())) {
            completed_ = true;
            runtime_orchestration::advance_particle_root_flow(
                state_ref,
                TaskType::FADE_PARTICLE,
                PriorityLevel::P3_PARTICLE,
                id,
                OrchestrationNode::PARTICLE_FADE,
                OrchestrationStatus::ERROR_STATE,
                OrchestrationEvent::PARTICLE_FADE_LOST_SLOT,
                false);
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status =
                "P3 fade task " + std::to_string(id) + " lost particle slot";
            return;
        }

        ParticleRenderData& particle = data.particles[static_cast<std::size_t>(particle_slot_)];
        if (particle.ownership_token != ownership_token_) {
            completed_ = true;
            runtime_orchestration::advance_particle_root_flow(
                state_ref,
                TaskType::FADE_PARTICLE,
                PriorityLevel::P3_PARTICLE,
                id,
                OrchestrationNode::PARTICLE_FADE,
                OrchestrationStatus::ERROR_STATE,
                OrchestrationEvent::PARTICLE_FADE_LOST_OWNERSHIP,
                false);
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status =
                "P3 fade task " + std::to_string(id) + " lost slot ownership";
            return;
        }

        particle.source_task_id = id;
        particle.active = false;
        particle.attached = false;

        if (!particle_fade_active(particle) && particle.status != "particle-fade-pending") {
            completed_ = true;
            runtime_orchestration::advance_particle_root_flow(
                state_ref,
                TaskType::FADE_PARTICLE,
                PriorityLevel::P3_PARTICLE,
                id,
                OrchestrationNode::PARTICLE_FADE,
                OrchestrationStatus::COMPLETED,
                OrchestrationEvent::PARTICLE_FADE_EMPTY,
                false);
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status =
                "P3 fade task " + std::to_string(id) + " found no fade work";
            return;
        }

        if (particle.fade_steps_remaining <= 0) {
            particle.x = -1.0f;
            particle.y = -1.0f;
            particle.visual_alpha = 0.0f;
            particle.status = "particle-cleaned";
            particle.source_task_id = -1;
            completed_ = true;
            runtime_orchestration::advance_particle_root_flow(
                state_ref,
                TaskType::FADE_PARTICLE,
                PriorityLevel::P3_PARTICLE,
                id,
                OrchestrationNode::PARTICLE_FADE,
                OrchestrationStatus::COMPLETED,
                OrchestrationEvent::PARTICLE_FADE_FINISHED,
                false);
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status =
                "P3 fade task " + std::to_string(id) + " final cleanup";
            return;
        }

        particle.fade_steps_remaining = std::max(0, particle.fade_steps_remaining - 1);
        particle.visual_alpha = std::clamp(
            static_cast<float>(particle.fade_steps_remaining) /
                static_cast<float>(kParticleFadeSteps),
            0.0f,
            1.0f);

        if (particle.fade_steps_remaining == 0) {
            particle.x = -1.0f;
            particle.y = -1.0f;
            particle.visual_alpha = 0.0f;
            particle.status = "particle-cleaned";
            particle.source_task_id = -1;
            completed_ = true;
            runtime_orchestration::advance_particle_root_flow(
                state_ref,
                TaskType::FADE_PARTICLE,
                PriorityLevel::P3_PARTICLE,
                id,
                OrchestrationNode::PARTICLE_FADE,
                OrchestrationStatus::COMPLETED,
                OrchestrationEvent::PARTICLE_FADE_FINISHED,
                false);
            scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
            data.ui.particle_task_status =
                "P3 fade task " + std::to_string(id) + " cleaned faded particle";
            return;
        }

        particle.status = resumed ? "particle-fade-resume" : "particle-fade-execute";
        runtime_orchestration::advance_particle_root_flow(
            state_ref,
            TaskType::FADE_PARTICLE,
            PriorityLevel::P3_PARTICLE,
            id,
            OrchestrationNode::PARTICLE_FADE,
            OrchestrationStatus::ACTIVE,
            OrchestrationEvent::PARTICLE_FADE_EXECUTE,
            true);
        scheduler_task_support::reconcile_particle_bookkeeping(state_ref, data);
        data.ui.particle_task_status =
            "P3 fade task " + std::to_string(id) +
            " step " + std::to_string(cycle_count_);
    });

    state = completed_ ? TaskState::FINISHED : TaskState::INTERRUPTED;
}

std::unique_ptr<Task> make_batch_particle_execution_task() {
    return std::make_unique<BatchParticleExecutionTask>();
}

std::unique_ptr<Task> make_single_particle_task(int particle_slot,
                                                int generation_index,
                                                int ownership_token,
                                                float mouth_x,
                                                float mouth_y) {
    return std::make_unique<SingleParticleTask>(
        particle_slot,
        generation_index,
        ownership_token,
        mouth_x,
        mouth_y);
}

std::unique_ptr<Task> make_particle_fade_task(int particle_slot,
                                              int ownership_token) {
    return std::make_unique<ParticleFadeTask>(particle_slot, ownership_token);
}
