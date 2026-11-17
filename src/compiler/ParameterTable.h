#pragma once

// The compiled circuit's user-facing parameters: the things a control can move
// without changing topology.
//
// Hardware handles, not names: the plugin's audio thread resolves a control to
// a `ParameterHandle` once, then every block hands the handle and a value to the
// runtime. `find()` is for load/prepare; `setParameters()` must never see a
// string.
//
// What is a parameter:
//   - every potentiometer             (position in [0, 1])
//   - every variable resistor         (position in [0, 1])
//   - every named resistor            (ohms)
//
// What is not:
//   - the two resistors a potentiometer expands into, and the one a variable
//     resistor expands into. They are derived values: exposing them as well
//     would give one control two names and let a caller desynchronise the pair.
//   - unnamed resistors (the synthetic leads a device model adds).
//
// An SPDT switch is two named resistors in the current model (`<id>_A`,
// `<id>_B`, one closed at 0.01 ohm and one open at 1 Gohm), so it appears as two
// `Resistance` parameters. That is the model's real shape, not a temporary
// adapter decision.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace livewdf::compiler {

enum class ParameterKind {
    Resistance,
    PotPosition,
    VarResPosition,
};

struct ParameterHandle {
    int value = -1;

    explicit operator bool() const noexcept { return value >= 0; }
    friend bool operator==(ParameterHandle a, ParameterHandle b) noexcept {
        return a.value == b.value;
    }
    friend bool operator!=(ParameterHandle a, ParameterHandle b) noexcept {
        return a.value != b.value;
    }
};

struct ParameterValue {
    ParameterHandle handle;
    double value = 0.0;
};

class ParameterTable {
public:
    struct Entry {
        std::string name;
        ParameterKind kind = ParameterKind::Resistance;
        /// Index into `Circuit::res`, `Circuit::pots`, or `Circuit::varRes`.
        int componentIndex = -1;
        /// The value the circuit was compiled with.
        double defaultValue = 0.0;
    };

    ParameterTable() = default;

    explicit ParameterTable(std::vector<Entry> entries) : entries_(std::move(entries)) {
        handles_.reserve(entries_.size());
        byName_.reserve(entries_.size() * 2);
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            handles_.push_back({i});
            // First wins. The compiler reports a duplicate name as a warning,
            // so a caller sees the collision at load time rather than silently
            // addressing whichever entry happened to be inserted last.
            byName_.emplace(entries_[static_cast<std::size_t>(i)].name, i);
        }
    }

    std::span<const ParameterHandle> handles() const noexcept { return handles_; }

    std::optional<ParameterHandle> find(std::string_view name) const noexcept {
        const auto it = byName_.find(std::string(name));
        if (it == byName_.end())
            return std::nullopt;
        return ParameterHandle{it->second};
    }

    std::string_view name(ParameterHandle handle) const noexcept {
        return entry(handle).name;
    }

    ParameterKind kind(ParameterHandle handle) const noexcept {
        return entry(handle).kind;
    }

    /// The value the circuit was compiled with. Current values live in the
    /// runtime, which is the object that owns the mutable engine state.
    double value(ParameterHandle handle) const noexcept {
        return entry(handle).defaultValue;
    }

    const Entry& entry(ParameterHandle handle) const noexcept {
        static const Entry invalid{};
        if (!handle || handle.value >= static_cast<int>(entries_.size()))
            return invalid;
        return entries_[static_cast<std::size_t>(handle.value)];
    }

    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<Entry> entries_;
    std::vector<ParameterHandle> handles_;
    std::unordered_map<std::string, int> byName_;
};

}  // namespace livewdf::compiler
