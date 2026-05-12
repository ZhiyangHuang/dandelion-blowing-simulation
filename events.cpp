#include "thread.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> g_runtime_notes;
constexpr std::size_t kMaxRuntimeNotes = 120;
std::ofstream g_runtime_log_stream;

}  // namespace

void push_runtime_note(const std::string& note) {
    std::cout << note << '\n';
    if (g_runtime_log_stream.is_open()) {
        g_runtime_log_stream << note << '\n';
        g_runtime_log_stream.flush();
    }
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

bool set_runtime_log_file(const std::string& path) {
    close_runtime_log_file();
    g_runtime_log_stream.open(path, std::ios::out | std::ios::trunc);
    return g_runtime_log_stream.is_open();
}

void close_runtime_log_file() {
    if (g_runtime_log_stream.is_open()) {
        g_runtime_log_stream.flush();
        g_runtime_log_stream.close();
    }
}
