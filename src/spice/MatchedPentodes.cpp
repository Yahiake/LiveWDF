#include "spice/MatchedPentodes.h"

#include "spice/Circuit.h"

#include <string_view>

namespace wdf {
namespace {

struct Feed {
    size_t resistor;
    std::string_view rail;
};

std::optional<Feed> fixedFeed(const Circuit& circuit, const std::string& terminal, size_t references) {
    if (references != 2 || terminal.empty() || terminal == kGroundName)
        return {};

    for (size_t i = 0; i < circuit.res.size(); ++i) {
        const auto& res = circuit.res[i];
        if (res.a != terminal && res.b != terminal)
            continue;
        if (!std::isfinite(res.value) || !(res.value / 2.0 > 0.0) || res.a == res.b)
            return {};
        // Live SPDT contacts are stamped as ordinary resistors by the document.
        if (res.name.ends_with("_A") || res.name.ends_with("_B"))
            return {};
        for (const auto& pot : circuit.pots)
            if (res.name == pot.name + "_1" || res.name == pot.name + "_2")
                return {};
        for (const auto& variable : circuit.varRes)
            if (res.name == variable.name)
                return {};
        return Feed{i, res.a == terminal ? res.b : res.a};
    }
    return {};
}

std::optional<double> fixedRailVoltage(const Circuit& circuit, std::string_view rail,
                                       std::span<const size_t> varyingSources) {
    for (size_t i = 0; i < circuit.src.size(); ++i) {
        const auto& source = circuit.src[i];
        const bool positive = source.a == rail && source.b == kGroundName;
        const bool negative = source.b == rail && source.a == kGroundName;
        if (!(positive || negative))
            continue;
        if (!std::isfinite(source.dc) || source.ac != 0.0 || !source.sin.empty() ||
            std::ranges::find(varyingSources, i) != varyingSources.end())
            return {};
        return positive ? source.dc : -source.dc;
    }
    return {};
}

bool sameScreenSupply(const Circuit& circuit, std::string_view left, std::string_view right,
                      std::span<const size_t> varyingSources) {
    if (left == right)
        return true;
    const auto leftVoltage = fixedRailVoltage(circuit, left, varyingSources);
    return leftVoltage && leftVoltage == fixedRailVoltage(circuit, right, varyingSources);
}

} // namespace

std::size_t collapseMatchedPentodes(Circuit& circuit, std::span<const size_t> varyingSources) {
    if (circuit.pent.size() < 2 || !circuit.nodeAliases.empty())
        return 0;

    std::unordered_map<std::string, size_t> references;
    const auto count = [&](const std::string& node) { ++references[node]; };
    circuit.visitStampedNodes(count);
    for (const auto& res : circuit.ctrlRes) {
        if (res.vca) {
            count(res.vca->control);
            count(res.vca->reference);
        }
        if (res.ota) {
            count(res.ota->bias);
            count(res.ota->biasDiode);
        }
    }
    for (const auto& bias : circuit.dbx200Biases) {
        count(bias.ni);
        count(bias.pi);
    }
    for (const auto& delay : circuit.bucketBrigades) {
        count(delay.input);
        count(delay.control);
        count(delay.common);
    }
    for (const auto& detector : circuit.rmsDetectors) {
        count(detector.input);
        count(detector.common);
    }
    for (const auto& opamp : circuit.opamp) {
        count(opamp.vcc);
        count(opamp.vee);
    }

    struct LocalFeeds { std::optional<Feed> grid, screen; };
    std::vector<LocalFeeds> feeds;
    feeds.reserve(circuit.pent.size());
    for (const auto& tube : circuit.pent)
        feeds.push_back({fixedFeed(circuit, tube.g, references.at(tube.g)),
                         fixedFeed(circuit, tube.s, references.at(tube.s))});

    const auto sameResistance = [&](const Feed& a, const Feed& b) {
        return circuit.res[a.resistor].value == circuit.res[b.resistor].value;
    };
    std::vector<bool> removedRes(circuit.res.size());
    std::vector<bool> removedTube(circuit.pent.size());
    size_t removed = 0;
    for (size_t i = 0; i < circuit.pent.size(); ++i) {
        if (removedTube[i] || !feeds[i].grid || !feeds[i].screen)
            continue;
        auto& lead = circuit.pent[i];
        const auto& a = feeds[i];
        for (size_t j = i + 1; j < circuit.pent.size(); ++j) {
            const auto& other = circuit.pent[j];
            const auto& b = feeds[j];
            if (removedTube[j] || !b.grid || !b.screen || lead.p != other.p || lead.k != other.k ||
                lead.model != other.model || !std::isfinite(2 * lead.model.count) ||
                !std::isfinite(2 * lead.model.reefman.count) || !std::isfinite(2 * lead.model.koren.count) ||
                a.grid->rail != b.grid->rail || !sameResistance(*a.grid, *b.grid) ||
                !sameResistance(*a.screen, *b.screen) ||
                !sameScreenSupply(circuit, a.screen->rail, b.screen->rail, varyingSources))
                continue;

            // The two isolated branches have identical terminal voltages. For
            // the sum of their currents, each feed resistance is halved.
            lead.model.count *= 2.0;
            lead.model.reefman.count *= 2.0;
            lead.model.koren.count *= 2.0;
            circuit.res[a.grid->resistor].value *= 0.5;
            circuit.res[a.screen->resistor].value *= 0.5;
            removedRes[b.grid->resistor] = true;
            removedRes[b.screen->resistor] = true;
            removedTube[j] = true;
            circuit.nodeAliases.emplace_back(other.g, lead.g);
            circuit.nodeAliases.emplace_back(other.s, lead.s);
            ++removed;
            break;
        }
    }
    for (size_t i = circuit.res.size(); i > 0; --i)
        if (removedRes[i - 1])
            circuit.res.erase(circuit.res.begin() + static_cast<std::ptrdiff_t>(i - 1));
    for (size_t i = circuit.pent.size(); i > 0; --i)
        if (removedTube[i - 1])
            circuit.pent.erase(circuit.pent.begin() + static_cast<std::ptrdiff_t>(i - 1));
    return removed;
}

} // namespace wdf
