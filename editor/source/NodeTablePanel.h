#pragma once

// The DC operating-point table.
//
// This is the highest-value check while drawing: after every edit the circuit is
// compiled and solved at DC, and every node's voltage is listed here. A node
// that should sit at 146 V and reads 0 V is a broken wire, and it shows up here
// before any audio is played.
//
// The panel owns no circuit and no engine -- the host solves and hands it a map.
// That keeps the "engine lives with the engine" rule intact, and means the table
// can be pointed at a Newton arm or a closed-form arm without changing a line.

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <string>
#include <vector>

namespace livewdf::editor {

class NodeTablePanel : public juce::Component {
public:
    NodeTablePanel();

    /// Replace the shown operating point. Node name -> volts.
    void setOperatingPoint(const std::map<std::string, double>& volts);

    /// Replace the shown per-root-device currents (plate current, diode current,
    /// ...). Row label -> amps. Shown under the node rows so a reader can see
    /// both what a node sits at and what flows through the devices.
    void setDeviceCurrents(const std::map<std::string, double>& amps);

    /// One-line status under the header: converge count, or the failure reason.
    /// Empty hides the line.
    void setStatus(const std::string& text, bool isError);

    void clear();

    void paint(juce::Graphics& g) override;

private:
    std::map<std::string, double> volts_;
    std::map<std::string, double> amps_;
    std::string status_;
    bool statusIsError_ = false;
};

}  // namespace livewdf::editor
