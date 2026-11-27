#pragma once

// A mains supply without its ripple: each rectified output of a drawn supply
// becomes a DC source behind a resistance, fitted from the drawn supply itself.
//
// A rectifier fed DC would conduct all the time and be stiffer than the pulsed
// real one, so the source and resistance come from running the drawn winding,
// rectifier and reservoir alone into a load: once at the idle voltage the
// circuit is precharged to, once at the current the amp draws at full drive.
// Everything after the reservoir stays as drawn.

#include "format/AcSource.h"
#include "format/Document.h"
#include "format/PortImpedance.h"
#include "wdf/Engine.h"

#include <cmath>
#include <map>
#include <mutex>
#include <numbers>
#include <set>
#include <string>
#include <vector>

namespace wdf {

inline bool replaceMainsSupplies(Circuit& c, const LiveWDFDocument& doc, double fs, bool stiff = false) {
    const std::string ground = doc.groundNode.empty() ? "0" : doc.groundNode;
    const auto settings = doc.sourceAcSettings(c);
    std::set<size_t> mains;
    for (size_t i = 0; i < c.src.size(); ++i)
        if (settings[i].ac != 0.0 && settings[i].freq != 0.0 && doc.hasAcPowerSupply()) {
            const auto part = std::ranges::find(doc.components, c.src[i].name, &SchematicComponent::id);
            if (part != doc.components.end() && part->properties.count("powerAmp"))
                mains.insert(i);
        }
    if (mains.empty())
        return false;

    // The winding side: everything the mains sources reach through resistors, and through diodes into
    // nodes that hold no DC (series rectifier diodes, a limiter between rectifier and reservoir).
    const auto filterNode = [&](const std::string& n) {
        return std::ranges::any_of(c.cap, [&](const Cap& x) { return x.holdsDc && (x.a == n || x.b == n); });
    };
    std::set<std::string> winding, outputs;
    std::vector<std::string> todo;
    for (size_t i : mains)
        for (const auto& n : {c.src[i].a, c.src[i].b})
            if (n != ground && winding.insert(n).second) todo.push_back(n);
    const auto spread = [&](const std::string& y) {
        if (y == ground || winding.count(y)) return;
        if (filterNode(y)) outputs.insert(y);
        else if (winding.insert(y).second) todo.push_back(y);
    };
    while (!todo.empty()) {
        const auto n = todo.back();
        todo.pop_back();
        for (const auto& r : c.res)
            for (const auto& [x, y] : {std::pair{r.a, r.b}, std::pair{r.b, r.a}})
                if (x == n) spread(y);
        for (const auto& d : c.diode)
            for (const auto& [x, y] : {std::pair{d.a, d.b}, std::pair{d.b, d.a}})
                if (x == n) spread(y);
    }
    const auto onWinding = [&](const std::string& n) { return n == ground || winding.count(n); };
    const auto terminals = [&](const std::string& n) {
        long count = 0;
        for (const auto& r : c.res) count += (r.a == n) + (r.b == n);
        for (const auto& l : c.ind) count += (l.a == n) + (l.b == n);
        for (const auto& x : c.cap) count += (x.a == n) + (x.b == n);
        for (const auto& x : c.src) count += (x.a == n) + (x.b == n);
        for (const auto& x : c.diode) count += (x.a == n) + (x.b == n);
        for (const auto& t : c.tri) count += (t.g == n) + (t.p == n) + (t.k == n);
        for (const auto& t : c.pent) count += (t.g == n) + (t.p == n) + (t.k == n) + (t.s == n);
        return count;
    };
    // A node inside a series reservoir stack (two capacitors with balance resistors) belongs to the supply.
    std::set<std::string> stack;
    const auto inStack = [&](const std::string& m) {
        const auto out = [&](const std::string& n) { return n == ground || outputs.count(n); };
        long links = 0;
        for (const auto& r : c.res) links += (r.a == m && out(r.b)) + (r.b == m && out(r.a));
        for (const auto& x : c.cap) links += (x.a == m && out(x.b)) + (x.b == m && out(x.a));
        return links == terminals(m);
    };
    for (const auto& x : c.cap)
        for (const auto& [o, m] : {std::pair{x.a, x.b}, std::pair{x.b, x.a}})
            if (x.holdsDc && outputs.count(o) && m != ground && !outputs.count(m) && inStack(m)) stack.insert(m);
    const auto inSupply = [&](const std::string& n) { return onWinding(n) || outputs.count(n) || stack.count(n); };

    // Idle voltage of each output: its precharge, summed along held capacitors from ground.
    std::map<std::string, double> held{{ground, 0.0}};
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& cap : c.cap)
            if (cap.holdsDc)
                for (const auto& [from, to, sign] : {std::tuple{cap.b, cap.a, 1.0}, std::tuple{cap.a, cap.b, -1.0}})
                    if (held.count(from) && !held.count(to)) {
                        held[to] = held[from] + sign * *cap.initialVolts;
                        grew = true;
                    }
    }
    std::vector<std::string> out(outputs.begin(), outputs.end());
    for (const auto& o : out)
        if (!held.count(o) || held[o] == 0.0)
            return false;

    std::set<std::string> mainsNames;
    for (size_t i : mains) mainsNames.insert(c.src[i].name);
    const auto removeWinding = [&](Circuit& x) {
        std::erase_if(x.src, [&](const Src& s) { return mainsNames.count(s.name) != 0; });
        std::erase_if(x.res, [&](const Res& r) { return (onWinding(r.a) && onWinding(r.b)) || winding.count(r.a) || winding.count(r.b); });
        std::erase_if(x.cap, [&](const Cap& cap) { return winding.count(cap.a) && winding.count(cap.b); });
        std::erase_if(x.diode, [&](const Diode& d) { return onWinding(d.a) || onWinding(d.b); });
    };
    const auto addSupply = [&](Circuit& x, const std::vector<std::pair<double, double>>& fitted) {
        removeWinding(x);
        for (size_t k = 0; k < out.size(); ++k) {
            x.addSrc("$mains " + out[k], ground, fitted[k].first, 0.0, {}, "$mains source " + out[k]);
            x.addRes("$mains " + out[k], out[k], fitted[k].second, "$mains resistance " + out[k]);
        }
    };

    // The drawn supply alone, loaded by one resistor per output.
    Circuit test;
    for (size_t i : mains)
        test.src.push_back(c.src[i]);
    for (const auto& r : c.res)
        if (inSupply(r.a) && inSupply(r.b)) test.res.push_back(r);
    for (const auto& cap : c.cap)
        if (inSupply(cap.a) && inSupply(cap.b)) test.cap.push_back(cap);
    for (const auto& d : c.diode)
        if (inSupply(d.a) && inSupply(d.b)) test.diode.push_back(d);
    const auto average = [&](const std::vector<double>& loads) {
        auto t = test;
        for (size_t k = 0; k < out.size(); ++k)
            t.addRes(out[k], ground, loads[k], "$mains load " + out[k]);
        Engine engine(t, fs);
        std::vector<double> values(t.src.size()), sum(out.size());
        const long settle = long(0.3 * fs), period = long(std::lround(fs / settings[*mains.begin()].freq));
        const long span = 6 * period;
        for (long n = 0; n < settle + span; ++n) {
            size_t s = 0;
            for (size_t i : mains)
                values[s++] = sourceValueAt(c.src[i].dc, settings[i].ac, settings[i].freq, fs, n);
            const auto& nodes = engine.step(values);
            if (n >= settle)
                for (size_t k = 0; k < out.size(); ++k) sum[k] += nodes[size_t(engine.nodeIndex(out[k]))] / double(span);
        }
        return sum;
    };

    // Fitted once per supply drawing and rate, since the drive burst below costs a whole-amp run.
    static std::mutex cacheMutex;
    static std::map<std::string, std::vector<std::pair<double, double>>> cache;
    std::string key = std::to_string(fs);
    for (size_t i : mains) key += "\n" + c.src[i].name + " " + std::to_string(c.src[i].dc) + " " + std::to_string(settings[i].ac);
    for (const auto& r : test.res) key += "\n" + r.name + " " + r.a + " " + r.b + " " + std::to_string(r.value);
    for (const auto& x : test.cap) key += "\n" + x.name + " " + x.a + " " + x.b + " " + std::to_string(x.value);
    for (const auto& d : test.diode) key += "\n" + d.name + " " + d.a + " " + d.b;
    const std::lock_guard lock(cacheMutex);
    auto& fitted = cache[key];
    if (fitted.empty()) {
        std::vector<double> idleLoad(out.size(), 0.0);
        for (size_t k = 0; k < out.size(); ++k) idleLoad[k] = 1e4 * std::abs(held[out[k]]) / 400.0;
        for (int pass = 0; pass < 6; ++pass) {
            const auto v = average(idleLoad);
            for (size_t k = 0; k < out.size(); ++k)
                idleLoad[k] *= std::pow(std::abs(held[out[k]] / v[k]), 4.0);
        }
        const auto vIdle = average(idleLoad);
        const auto line = [&](const std::vector<double>& fullLoad) {
            const auto vFull = average(fullLoad);
            std::vector<std::pair<double, double>> fit;
            for (size_t k = 0; k < out.size(); ++k) {
                const double iIdle = vIdle[k] / idleLoad[k], iFull = vFull[k] / fullLoad[k];
                const double r = (vIdle[k] - vFull[k]) / (iFull - iIdle);
                fit.emplace_back(vIdle[k] + r * iIdle, r);
            }
            return fit;
        };
        auto fullLoad = idleLoad;
        for (auto& r : fullLoad) r /= 4.0;
        const auto first = line(fullLoad);

        // Refit through the mean current the amp itself draws on a full-scale 110 Hz burst, so the line is exact where
        // the rail sits when played hard, whatever the rectifier's curve. The amp runs at the document's control
        // positions, the ones the level trims are fitted at, so the cached fit does not depend on the knobs at load.
        auto amp = circuitWithPortImpedances(doc);
        addSupply(amp, first);
        Engine engine(amp, fs);
        auto values = engine.dcValues();
        for (const auto& s : amp.isrc) values.push_back(s.dc);
        const auto input = size_t(std::ranges::find(amp.src, doc.inputId(), &Src::name) - amp.src.begin());
        const long settle = long(0.01 * fs), span = long(0.02 * fs);
        std::vector<double> current(out.size());
        for (long n = 0; n < settle + span; ++n) {
            if (input < amp.src.size())
                values[input] = amp.src[input].dc + doc.inputVoltsPerFullScale() * std::sin(2.0 * std::numbers::pi * 110.0 * double(n) / fs);
            const auto& nodes = engine.step(values);
            if (n >= settle)
                for (size_t k = 0; k < out.size(); ++k)
                    current[k] += (first[k].first - nodes[size_t(engine.nodeIndex(out[k]))]) / first[k].second / double(span);
        }
        for (size_t k = 0; k < out.size(); ++k) {
            const double iIdle = vIdle[k] / idleLoad[k], i = std::max(current[k] / iIdle, 2.0) * iIdle;
            if (const double load = (first[k].first - first[k].second * i) / i; load > 0.0) fullLoad[k] = load;
        }
        fitted = line(fullLoad);
    }
    addSupply(c, fitted);

    // Stiff: each reservoir and filter capacitor becomes a source at its precharge that the engine steps from the
    // current drawn in the previous sample (Src::heldFarads), so the rails sag but the stages do not couple through
    // the filter chain inside a solve. The filter chain is walked from the rectifier
    // outputs through held capacitors and series resistors and chokes, stopping at any node that is neither a
    // filter node nor a plain series joint, so a precharged oscillator capacitor stays a capacitor.
    if (stiff) {
        std::set<std::string> chain(out.begin(), out.end());
        std::vector<std::string> next(out.begin(), out.end());
        while (!next.empty()) {
            const auto n = next.back();
            next.pop_back();
            const auto reach = [&](const std::string& m, bool series) {
                if (m != ground && !chain.count(m) && (filterNode(m) || (series && terminals(m) == 2)) && chain.insert(m).second)
                    next.push_back(m);
            };
            for (const auto& x : c.cap)
                if (x.holdsDc) { if (x.a == n) reach(x.b, false); if (x.b == n) reach(x.a, false); }
            for (const auto& r : c.res) { if (r.a == n) reach(r.b, true); if (r.b == n) reach(r.a, true); }
            for (const auto& l : c.ind) { if (l.a == n) reach(l.b, true); if (l.b == n) reach(l.a, true); }
        }
        const auto supplyCap = [&](const Cap& x) {
            return x.holdsDc && (chain.count(x.a) || x.a == ground) && (chain.count(x.b) || x.b == ground);
        };
        // A capacitor in parallel with a rail adds to it; one the rails already join across a stack adds no rail.
        std::map<std::pair<std::string, std::string>, size_t> rails;
        std::map<std::string, std::string> joined;
        const auto root = [&](std::string n) { while (joined.count(n)) n = joined[n]; return n; };
        for (const auto& cap : c.cap) {
            if (!supplyCap(cap)) continue;
            if (const auto it = rails.find(std::minmax(cap.a, cap.b)); it != rails.end()) {
                c.src[it->second].heldFarads += cap.value;
            } else if (root(cap.a) != root(cap.b)) {
                joined[root(cap.a)] = root(cap.b);
                rails[std::minmax(cap.a, cap.b)] = c.src.size();
                const bool flip = cap.a == ground;
                c.addSrc(flip ? cap.b : cap.a, flip ? cap.a : cap.b, flip ? -*cap.initialVolts : *cap.initialVolts, 0.0, {},
                         "$mains rail " + cap.name);
                c.src.back().heldFarads = cap.value;
            }
        }
        // A small capacitor across a rail adds to it too; left in the solve, its current through the held source runs away.
        const auto acrossRail = [&](const Cap& x) { return rails.count(std::minmax(x.a, x.b)) != 0; };
        for (const auto& cap : c.cap)
            if (!supplyCap(cap) && acrossRail(cap)) c.src[rails[std::minmax(cap.a, cap.b)]].heldFarads += cap.value;
        std::erase_if(c.cap, [&](const Cap& x) { return supplyCap(x) || acrossRail(x); });
    }
    return true;
}

}  // namespace wdf
