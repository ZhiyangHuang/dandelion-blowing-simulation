#include "thread.h"

static queue<Event> event_q;
static LatestValueBuffer latest;
static RenderState render_state = UI_START;

void emit_event(EventType type, int value, const string& label) {
    event_q.push({type, value, label});
}

bool poll_event(Event& event) {
    if (event_q.empty()) {
        return false;
    }

    event = event_q.front();
    event_q.pop();
    return true;
}

LatestValueBuffer& latest_buffer() {
    return latest;
}

RenderState current_render_state() {
    return render_state;
}

void set_render_state(RenderState state) {
    render_state = state;
}
