#pragma once

// Live controls: the parts that move together.
//
// A label is not just a caption. A pot and a rheostat sharing a label are one
// physical control, and a bank of SPDTs sharing a label is one changeover. The
// editor enforces that when it writes a value; the plugin needs the same
// grouping to build one control and drive every member. This is the one
// definition both read.
//
// An unlabelled part is its own group, named by its id. That preserves the old
// behaviour (two unlabelled pots are two knobs) without inventing a name.

#include <algorithm>
#include <string>
#include <vector>

#include "format/Document.h"

namespace wdf {
// ---------------------------------------------------------------------------
// Live controls: the parts that move together.
//
// A label is not just a caption. A pot and a rheostat sharing a label are one
// physical control, and a bank of SPDTs sharing a label is one changeover. The
// editor enforces that when it writes a value; the plugin needs the same
// grouping to build one control and drive every member. This is the one
// definition both read.
//
// An unlabelled part is its own group, named by its id. That preserves the old
// behaviour (two unlabelled pots are two knobs) without inventing a name.
// ---------------------------------------------------------------------------
struct LiveControlMember {
    std::string id;
    std::string type;  // "pot", "varres" or "switch_res"
};

struct LiveControlGroup {
    std::string name;
    std::vector<LiveControlMember> members;
};

/// Group the document's pots and rheostats by their visible name. The order is
/// the document's order, so the plugin's knob index is stable across a load.
inline std::vector<LiveControlGroup> potControlGroups(const LiveWDFDocument& doc) {
    std::vector<LiveControlGroup> groups;

    for (const auto& comp : doc.components) {
        if (comp.type != "pot" && comp.type != "varres")
            continue;

        const auto label = comp.properties.find("label");
        const std::string name =
            (label != comp.properties.end() && !label->second.empty()) ? label->second
                                                                       : comp.id;

        auto group = std::find_if(groups.begin(), groups.end(),
                                  [&](const LiveControlGroup& g) { return g.name == name; });
        if (group == groups.end()) {
            groups.push_back({name, {}});
            group = groups.end() - 1;
        }
        group->members.push_back({comp.id, comp.type});
    }

    // Custom user layout order takes precedence when configured
    if (!doc.potOrder.empty()) {
        std::vector<LiveControlGroup> ordered;
        for (const auto& name : doc.potOrder) {
            auto it = std::find_if(groups.begin(), groups.end(),
                                  [&](const LiveControlGroup& g) { return g.name == name; });
            if (it != groups.end()) {
                ordered.push_back(std::move(*it));
                groups.erase(it);
            }
        }
        for (auto& rem : groups)
            ordered.push_back(std::move(rem));
        return ordered;
    }

    return groups;
}

/// Group the document's SPDTs by their visible name. A bank of changeovers
/// sharing a label is one switch, exactly like the pot groups above.
inline std::vector<LiveControlGroup> switchControlGroups(const LiveWDFDocument& doc) {
    std::vector<LiveControlGroup> groups;

    for (const auto& comp : doc.components) {
        if (comp.type != "switch_res")
            continue;

        const auto label = comp.properties.find("label");
        const std::string name =
            (label != comp.properties.end() && !label->second.empty()) ? label->second
                                                                       : comp.id;

        auto group = std::find_if(groups.begin(), groups.end(),
                                  [&](const LiveControlGroup& g) { return g.name == name; });
        if (group == groups.end()) {
            groups.push_back({name, {}});
            group = groups.end() - 1;
        }
        group->members.push_back({comp.id, comp.type});
    }

    // Custom user layout order takes precedence when configured
    if (!doc.switchOrder.empty()) {
        std::vector<LiveControlGroup> ordered;
        for (const auto& name : doc.switchOrder) {
            auto it = std::find_if(groups.begin(), groups.end(),
                                  [&](const LiveControlGroup& g) { return g.name == name; });
            if (it != groups.end()) {
                ordered.push_back(std::move(*it));
                groups.erase(it);
            }
        }
        for (auto& rem : groups)
            ordered.push_back(std::move(rem));
        return ordered;
    }

    return groups;
}

} // namespace wdf