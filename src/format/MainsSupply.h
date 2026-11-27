#pragma once

// A mains supply without its ripple: each rectified output of a drawn supply
// becomes a DC source behind a resistance, fitted from the drawn supply itself.
//
// A rectifier fed DC would conduct all the time and be stiffer than the pulsed
// real one, so the source and resistance come from running the drawn winding,
// rectifier and reservoir alone into a load: once at the idle voltage the
// circuit is precharged to, once at four times that current, which is where
// the line through the two points lands on the drawn amps' full-power rail.
// Everything after the reservoir stays as drawn.

#include "format/AcSource.h"
#include "format/Document.h"
#include "wdf/Engine.h"

#include <cmath>
#include <map>
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

    // The winding side: everything the mains sources reach through resistors.
    std::set<std::string> winding;
    std::vector<std::string> todo;
    for (size_t i : mains)
        for (const auto& n : {c.src[i].a, c.src[i].b})
            if (n != ground && winding.insert(n).second) todo.push_back(n);
    while (!todo.empty()) {
        const auto n = todo.back();
        todo.pop_back();
        for (const auto& r : c.res)
            for (const auto& [x, y] : {std::pair{r.a, r.b}, std::pair{r.b, r.a}})
                if (x == n && y != ground && winding.insert(y).second) todo.push_back(y);
    }
    const auto onWinding = [&](const std::string& n) { return n == ground || winding.count(n); };
    std::set<std::string> outputs;
    for (const auto& d : c.diode)
        if (onWinding(d.a) != onWinding(d.b))
            outputs.insert(onWinding(d.a) ? d.b : d.a);
    const auto inSupply = [&](const std::string& n) { return onWinding(n) || outputs.count(n); };

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
    const auto removeWinding = [&] {
        std::erase_if(c.src, [&](const Src& s) { return mainsNames.count(s.name) != 0; });
        std::erase_if(c.res, [&](const Res& r) { return onWinding(r.a) && onWinding(r.b); });
        std::erase_if(c.cap, [&](const Cap& cap) { return winding.count(cap.a) && winding.count(cap.b); });
        std::erase_if(c.diode, [&](const Diode& d) { return onWinding(d.a) || onWinding(d.b); });
    };
    // Stiff: each reservoir and filter capacitor becomes a source at its precharge, so each supply node is a fixed
    // rail and the stages do not couple through the filter chain. The filter chain is walked from the rectifier
    // outputs through held capacitors and series resistors and chokes, stopping at any node that is neither a
    // filter node nor a plain series joint, so a precharged oscillator capacitor stays a capacitor.
    if (stiff) {
        removeWinding();
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
        const auto filterNode = [&](const std::string& n) {
            return std::ranges::any_of(c.cap, [&](const Cap& x) { return x.holdsDc && (x.a == n || x.b == n); });
        };
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
        std::set<std::pair<std::string, std::string>> rails;
        for (const auto& cap : c.cap)
            if (supplyCap(cap) && rails.insert(std::minmax(cap.a, cap.b)).second)
                c.addSrc(cap.a, cap.b, *cap.initialVolts, 0.0, {}, "$mains rail " + cap.name);
        std::erase_if(c.cap, supplyCap);
        return true;
    }

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
    std::vector<double> idleLoad(out.size(), 0.0);
    for (size_t k = 0; k < out.size(); ++k) idleLoad[k] = 1e4 * std::abs(held[out[k]]) / 400.0;
    for (int pass = 0; pass < 6; ++pass) {
        const auto v = average(idleLoad);
        for (size_t k = 0; k < out.size(); ++k)
            idleLoad[k] *= std::pow(std::abs(held[out[k]] / v[k]), 4.0);
    }
    auto fullLoad = idleLoad;
    for (auto& r : fullLoad) r /= 4.0;
    const auto vIdle = average(idleLoad), vFull = average(fullLoad);

    removeWinding();
    for (size_t k = 0; k < out.size(); ++k) {
        const double iIdle = vIdle[k] / idleLoad[k], iFull = vFull[k] / fullLoad[k];
        const double r = (vIdle[k] - vFull[k]) / (iFull - iIdle);
        c.addSrc("$mains " + out[k], ground, vIdle[k] + r * iIdle, 0.0, {}, "$mains source " + out[k]);
        c.addRes("$mains " + out[k], out[k], r, "$mains resistance " + out[k]);
    }
    return true;
}

}  // namespace wdf
