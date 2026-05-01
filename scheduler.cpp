#include "thread.h"
#include <iostream>
#include <vector>

using namespace std;

static queue<Thread*> high_q;
static queue<Thread*> mid_q;
static queue<Thread*> low_q;
static queue<Thread*> blocked_q;
static vector<Thread*> threads;

static void clear_ready_queue(queue<Thread*>& q) {
    while (!q.empty()) {
        q.pop();
    }
}

static Thread* current_thread = nullptr;
static int thread_id_counter = 0;
static bool need_preempt = false;
static bool scheduler_paused = false;

static void destroy_all_threads() {
    current_thread = nullptr;
    need_preempt = false;
    scheduler_paused = false;
    clear_ready_queue(high_q);
    clear_ready_queue(mid_q);
    clear_ready_queue(low_q);
    clear_ready_queue(blocked_q);

    for (Thread* t : threads) {
        delete t;
    }
    threads.clear();
    thread_id_counter = 0;
}

static void enqueue(Thread* t) {
    if (!t || t->state == FINISHED) {
        return;
    }

    if (t->priority == HIGH) {
        high_q.push(t);
    } else if (t->priority == MEDIUM) {
        mid_q.push(t);
    } else {
        low_q.push(t);
    }
}

static Thread* pick_next() {
    if (!high_q.empty()) {
        Thread* t = high_q.front();
        high_q.pop();
        return t;
    }
    if (!mid_q.empty()) {
        Thread* t = mid_q.front();
        mid_q.pop();
        return t;
    }
    if (!low_q.empty()) {
        Thread* t = low_q.front();
        low_q.pop();
        return t;
    }
    return nullptr;
}

static bool has_ready_task() {
    return !high_q.empty() || !mid_q.empty() || !low_q.empty();
}

static bool has_higher_priority_ready_task(Priority current_priority) {
    if (current_priority == LOW) {
        return !high_q.empty() || !mid_q.empty();
    }
    if (current_priority == MEDIUM) {
        return !high_q.empty();
    }
    return false;
}

static bool should_preempt(Thread* running) {
    if (!running || !has_ready_task()) {
        return false;
    }
    return has_higher_priority_ready_task(running->priority);
}

static void dispatch_pending_events() {
    Event event;
    while (poll_event(event)) {
        handle_event(event);
    }
}

Thread* create_thread(const string& name, function<void(Thread*)> func, Priority p) {
    Thread* t = new Thread();
    t->id = thread_id_counter++;
    t->name = name;
    t->state = READY;
    t->priority = p;
    t->func = func;
    threads.push_back(t);
    enqueue(t);
    return t;
}

void rebuild_runtime_threads(ThreadMode mode) {
    destroy_all_threads();
    set_thread_mode(mode);
    cam = nullptr;
    mic = nullptr;
    render_ui = nullptr;

    if (mode == THREAD_MODE_1) {
        cam = create_thread("camera", camera, LOW);
    } else if (mode == THREAD_MODE_2) {
        cam = create_thread("camera", camera, LOW);
        render_ui = create_thread("render", render_thread, MEDIUM);
    } else {
        cam = create_thread("camera", camera, LOW);
        mic = create_thread("mic", mic_thread, HIGH);
        render_ui = create_thread("render", render_thread, MEDIUM);
    }

    reset_simulation();
}

int created_thread_count() {
    return static_cast<int>(threads.size());
}

const vector<Thread*>& all_threads() {
    return threads;
}

void reset_scheduler_state() {
    clear_ready_queue(high_q);
    clear_ready_queue(mid_q);
    clear_ready_queue(low_q);
    clear_ready_queue(blocked_q);

    for (Thread* t : threads) {
        if (t->state == READY) {
            enqueue(t);
        }
    }
}

void yield() {
    current_thread->state = READY;
    current_thread->time_slice = 0;
    enqueue(current_thread);
}

void block() {
    current_thread->state = BLOCKED;
    blocked_q.push(current_thread);
}

void wakeup(Thread* t) {
    if (!t || t->state != BLOCKED) {
        return;
    }

    t->state = READY;
    enqueue(t);
}

void lock(Mutex* m) {
    if (!m->locked) {
        m->locked = true;
        return;
    }

    m->waiters.push(current_thread);
    current_thread->state = BLOCKED;
}

void unlock(Mutex* m) {
    if (!m->waiters.empty()) {
        Thread* t = m->waiters.front();
        m->waiters.pop();
        t->state = READY;
        enqueue(t);
        return;
    }

    m->locked = false;
}

void timer_event() {
    if (current_thread) {
        current_thread->time_slice++;
        if (current_thread->priority == LOW && current_thread->time_slice >= 2) {
            cout << "[TIMER IRQ] preempt low priority task\n";
            need_preempt = true;
        }
    }

    simulation_tick();
}

void event_loop() {
    while (visualization_running()) {
        process_visual_input();
        dispatch_pending_events();
        timer_event();

        if (scheduler_paused) {
            render_visual_frame();
            continue;
        }

        if (current_thread &&
            current_thread->state == RUNNING &&
            should_preempt(current_thread)) {
            cout << "[SCHED] higher priority task preempts lower priority task\n";
            current_thread->state = READY;
            current_thread->time_slice = 0;
            enqueue(current_thread);
            current_thread = nullptr;
        }

        Thread* next = pick_next();
        if (!next) {
            render_visual_frame();
            continue;
        }

        current_thread = next;
        current_thread->state = RUNNING;
        current_thread->started = true;
        current_thread->func(current_thread);
        render_visual_frame();

        if (need_preempt && current_thread && current_thread->state == RUNNING) {
            need_preempt = false;
            current_thread->state = READY;
            current_thread->time_slice = 0;
            enqueue(current_thread);
        } else if (current_thread && current_thread->state == RUNNING) {
            current_thread->state = READY;
            enqueue(current_thread);
        }
        current_thread = nullptr;

        if (simulation_done()) {
            set_render_state(UI_WAITING);
        }

        if (!visualization_running()) {
            cout << "Visualization closed\n";
            return;
        }

        render_visual_frame();
    }
    cout << "Visualization closed\n";
}
