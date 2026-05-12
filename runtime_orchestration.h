#ifndef RUNTIME_ORCHESTRATION_H
#define RUNTIME_ORCHESTRATION_H

#include "thread.h"

namespace runtime_orchestration {

void advance_human_behavior_flow(RuntimeState& state_ref,
                                 TaskType leaf_type,
                                 int leaf_id,
                                 OrchestrationNode node,
                                 OrchestrationStatus status,
                                 OrchestrationEvent event,
                                 bool active);

void advance_particle_root_flow(RuntimeState& state_ref,
                                TaskType leaf_type,
                                PriorityLevel leaf_priority,
                                int leaf_id,
                                OrchestrationNode node,
                                OrchestrationStatus status,
                                OrchestrationEvent event,
                                bool active);

}  // namespace runtime_orchestration

#endif
