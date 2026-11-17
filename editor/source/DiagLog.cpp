#include "DiagLog.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace livewdf::editor {

namespace {

/// Whether the log is on. Off unless something asks for it.
bool enabled = false;

/// The file's path. Kept in one place so the log is one file and not one per
/// working directory the editor happens to be started from.
const char* logPath() {
    return "editor_diag.log";
}

/// The local time, portably. `std::localtime` is deprecated on Windows because
/// it shares a static buffer; `localtime_s` is the safe form there, and
/// `localtime_r` elsewhere.
const std::tm* localTime(std::time_t clock) {
    static thread_local std::tm out {};
#if defined(_WIN32)
    if (localtime_s(&out, &clock) != 0)
        return nullptr;
#else
    if (localtime_r(&clock, &out) == nullptr)
        return nullptr;
#endif
    return &out;
}

}  // namespace

void setDiagLogEnabled(bool on) {
    enabled = on;
}

bool diagLogEnabled() {
    return enabled;
}

void diagLog(const std::string& line) {
    if (!enabled)
        return;

    // Best effort, and quiet about it: a log that cannot be written is not
    // worth an error dialog, and it must never be the thing that takes the
    // editor down.
    try {
        std::ofstream out(logPath(), std::ios::app);
        if (!out)
            return;

        // A timestamp and a thread id, because the order of two events from
        // different threads is exactly the thing that is in doubt.
        const auto now = std::chrono::system_clock::now();
        const auto since = now.time_since_epoch();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(since);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since - secs);
        const std::time_t clock = std::chrono::system_clock::to_time_t(now);

        std::ostringstream stamp;
        stamp << std::put_time(localTime(clock), "%H:%M:%S")
              << '.' << std::setw(3) << std::setfill('0') << ms.count()
              << " [" << std::this_thread::get_id() << "] ";

        out << stamp.str() << line << '\n';
    } catch (...) {
        // Deliberately swallowed. See above.
    }
}

}  // namespace livewdf::editor
