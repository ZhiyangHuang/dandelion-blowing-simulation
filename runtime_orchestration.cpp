#include "runtime_orchestration.h"
#include "scheduler_task_support.h"

const char* orchestration_node_label(OrchestrationNode node) {
    switch (node) {
    case OrchestrationNode::CAMERA_DETECT:
        return "CameraDetectTask";
    case OrchestrationNode::BLOW_DETECT:
        return "BlowDetectTask";
    case OrchestrationNode::PARTICLE_GENERATE:
        return "ParticleGenerateTask";
    case OrchestrationNode::PARTICLE_BATCH:
        return "ParticleBatchTask";
    case OrchestrationNode::PARTICLE_MOVE:
        return "ParticleMoveTask";
    case OrchestrationNode::PARTICLE_FADE:
        return "ParticleFadeTask";
    case OrchestrationNode::CHANGE_DANDELION:
        return "ChangeDandelionTask";
    case OrchestrationNode::IDLE:
    default:
        return "idle";
    }
}

const char* orchestration_status_label(OrchestrationStatus status) {
    switch (status) {
    case OrchestrationStatus::WAITING:
        return "waiting";
    case OrchestrationStatus::ACTIVE:
        return "active";
    case OrchestrationStatus::HANDOFF:
        return "handoff";
    case OrchestrationStatus::FALLBACK:
        return "fallback";
    case OrchestrationStatus::DRAINED:
        return "drained";
    case OrchestrationStatus::COMPLETED:
        return "completed";
    case OrchestrationStatus::BLOCKED:
        return "blocked";
    case OrchestrationStatus::ERROR_STATE:
        return "error";
    case OrchestrationStatus::IDLE:
    default:
        return "idle";
    }
}

const char* orchestration_event_label(OrchestrationEvent event) {
    switch (event) {
    case OrchestrationEvent::CAMERA_DEVICE_WAIT:
        return "camera-device-wait";
    case OrchestrationEvent::CAMERA_MOUTH_WAIT:
        return "camera-mouth-wait";
    case OrchestrationEvent::CAMERA_GATE_HOLD:
        return "camera-gate-hold";
    case OrchestrationEvent::CAMERA_DEVICE_UNAVAILABLE:
        return "camera-device-unavailable";
    case OrchestrationEvent::MOUTH_OPEN:
        return "mouth-open";
    case OrchestrationEvent::MICROPHONE_DEVICE_UNAVAILABLE:
        return "microphone-device-unavailable";
    case OrchestrationEvent::MICROPHONE_DEVICE_WAIT:
        return "microphone-device-wait";
    case OrchestrationEvent::MICROPHONE_BRIDGE_DISCONNECTED:
        return "microphone-bridge-disconnected";
    case OrchestrationEvent::MICROPHONE_SAMPLE_WAIT:
        return "microphone-sample-wait";
    case OrchestrationEvent::MICROPHONE_GATE_WAIT:
        return "microphone-gate-wait";
    case OrchestrationEvent::MICROPHONE_SAMPLE_STALE:
        return "microphone-sample-stale";
    case OrchestrationEvent::VOICE_WAIT:
        return "voice-wait";
    case OrchestrationEvent::BLOW_FALLBACK:
        return "blow-fallback";
    case OrchestrationEvent::BLOW_DETECTED:
        return "blow-detected";
    case OrchestrationEvent::PARTICLE_GENERATE:
        return "particle-generate";
    case OrchestrationEvent::PARTICLE_GENERATE_EMPTY:
        return "particle-generate-empty";
    case OrchestrationEvent::PARTICLE_BATCH_TICK:
        return "particle-batch-tick";
    case OrchestrationEvent::PARTICLE_BATCH_HANDOFF:
        return "particle-batch-handoff";
    case OrchestrationEvent::PARTICLE_STAGE_DRAINED:
        return "particle-stage-drained";
    case OrchestrationEvent::PARTICLE_MOVE_EXECUTE:
        return "particle-move-execute";
    case OrchestrationEvent::PARTICLE_MOVE_BOUNDARY_STOP:
        return "particle-move-boundary-stop";
    case OrchestrationEvent::PARTICLE_MOVE_LOST_SLOT:
        return "particle-move-lost-slot";
    case OrchestrationEvent::PARTICLE_MOVE_LOST_OWNERSHIP:
        return "particle-move-lost-ownership";
    case OrchestrationEvent::PARTICLE_FADE_EXECUTE:
        return "particle-fade-execute";
    case OrchestrationEvent::PARTICLE_FADE_FINISHED:
        return "particle-fade-finished";
    case OrchestrationEvent::PARTICLE_FADE_EMPTY:
        return "particle-fade-empty";
    case OrchestrationEvent::PARTICLE_FADE_LOST_SLOT:
        return "particle-fade-lost-slot";
    case OrchestrationEvent::PARTICLE_FADE_LOST_OWNERSHIP:
        return "particle-fade-lost-ownership";
    case OrchestrationEvent::CHANGE_DANDELION:
        return "change-dandelion";
    case OrchestrationEvent::NONE:
    default:
        return "none";
    }
}

void set_orchestration_record(OrchestrationRecord& record,
                              bool active,
                              OrchestrationNode node,
                              OrchestrationStatus status,
                              OrchestrationEvent event,
                              TaskType leaf_type,
                              PriorityLevel leaf_priority,
                              int leaf_id,
                              int frame_index) {
    record.active = active;
    record.current_node = active ? node : OrchestrationNode::IDLE;
    record.current_status = status;
    record.last_event = event;
    record.current_leaf_type = leaf_type;
    record.current_leaf_priority = leaf_priority;
    record.current_leaf_id = leaf_id;
    record.last_updated_frame = frame_index;
}

namespace runtime_orchestration {

void advance_human_behavior_flow(RuntimeState& state_ref,
                                 TaskType leaf_type,
                                 int leaf_id,
                                 OrchestrationNode node,
                                 OrchestrationStatus status,
                                 OrchestrationEvent event,
                                 bool active) {
    set_orchestration_record(
        state_ref.human_behavior_flow,
        active,
        node,
        status,
        event,
        leaf_type,
        PriorityLevel::P2_FUNCTIONAL,
        leaf_id,
        scheduler_task_support::current_scheduler_frame_index());
}

void advance_particle_root_flow(RuntimeState& state_ref,
                                TaskType leaf_type,
                                PriorityLevel leaf_priority,
                                int leaf_id,
                                OrchestrationNode node,
                                OrchestrationStatus status,
                                OrchestrationEvent event,
                                bool active) {
    set_orchestration_record(
        state_ref.particle_root_flow,
        active,
        node,
        status,
        event,
        leaf_type,
        leaf_priority,
        leaf_id,
        scheduler_task_support::current_scheduler_frame_index());
}

}  // namespace runtime_orchestration
