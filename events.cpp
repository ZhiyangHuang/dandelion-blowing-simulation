#include "thread.h"

#include <string>
#include <vector>

namespace {

std::vector<std::string> g_runtime_notes;

}  // namespace

void push_runtime_note(const std::string& note) {
    g_runtime_notes.push_back(note);
}

const std::vector<std::string>& current_runtime_notes() {
    return g_runtime_notes;
}

std::vector<std::string> drain_runtime_notes() {
    std::vector<std::string> notes = g_runtime_notes;
    g_runtime_notes.clear();
    return notes;
}
