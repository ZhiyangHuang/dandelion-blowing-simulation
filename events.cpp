#include "thread.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> g_runtime_notes;
constexpr std::size_t kMaxRuntimeNotes = 120;

}  // namespace

void push_runtime_note(const std::string& note) {
    std::cout << note << '\n';
    g_runtime_notes.push_back(note);
    if (g_runtime_notes.size() > kMaxRuntimeNotes) {
        g_runtime_notes.erase(g_runtime_notes.begin(),
                              g_runtime_notes.begin() +
                                  static_cast<std::ptrdiff_t>(g_runtime_notes.size() - kMaxRuntimeNotes));
    }
}

const std::vector<std::string>& current_runtime_notes() {
    return g_runtime_notes;
}

std::vector<std::string> drain_runtime_notes() {
    std::vector<std::string> notes = g_runtime_notes;
    g_runtime_notes.clear();
    return notes;
}
