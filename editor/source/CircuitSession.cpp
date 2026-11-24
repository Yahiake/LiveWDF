#include "CircuitSession.h"

#include "format/PartLibrary.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace livewdf::editor {

namespace {

/// The part's kind in lowercase, for a sentence: "the capacitor C1 ...".
std::string partKind(const std::string& type) {
    const PartSpec* spec = findPart(type);
    if (spec == nullptr || spec->label.empty())
        return "part";

    std::string label = spec->label;
    label[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(label[0])));
    return label;
}

/// The sentence the editor shows instead of the engine's own.
///
/// **The engine's message is true and useless.** "rootTransfer failed
/// (singular linear network)" says that the solver gave up, and not why -- and
/// the why is nearly always one part that is joined to nothing, which is a
/// drawing mistake the user can see and fix.
///
/// So the diagnosis comes first, and the engine's message is the fallback for
/// the failures it cannot explain (two voltage sources across the same nodes,
/// say).
std::string describeRejection(const wdf::LiveWDFDocument& doc,
                               const std::vector<std::pair<std::string, std::string>>& unconnected,
                               const std::string& engineMessage) {
    if (unconnected.empty())
        return "circuit rejected: " + engineMessage;

    const auto& [partId, pin] = unconnected.front();

    // The part's kind, from the palette, and the id the drawing shows.
    std::string kind = "part";
    for (const auto& comp : doc.components)
        if (comp.id == partId) {
            kind = partKind(comp.type);
            break;
        }

    return "the " + kind + " " + partId + " is not connected to anything"
           + " -- the circuit cannot be solved";
}

}  // namespace

bool CircuitSession::compile(const wdf::LiveWDFDocument& doc, double fs) {
    clear();
    fs_ = fs;

    try {
        circuit_ = doc.toCircuit();
    } catch (const std::exception& e) {
        error_ = std::string("circuit rejected: ") + e.what();
        engine_.reset();
        return false;
    } catch (...) {
        error_ = "circuit rejected (unknown error)";
        engine_.reset();
        return false;
    }

    // **The diagnosis, before the engine's verdict.** Which parts are joined
    // to nothing is a question about the drawing, and it is the answer to
    // "why did it say that?" when the engine refuses the circuit.
    unconnected_ = doc.unconnectedPins();

    const double effectiveFs = (osFactor_ == wdf::OversamplingFactor::X1)
                                   ? fs
                                   : (fs * static_cast<double>(osFactor_));
    try {
        engine_ = std::make_unique<wdf::Engine>(circuit_, effectiveFs);
    } catch (const std::exception& e) {
        error_ = describeRejection(doc, unconnected_, e.what());
        errorPartId_ = unconnected_.empty() ? std::string() : unconnected_.front().first;
        errorPin_ = unconnected_.empty() ? std::string() : unconnected_.front().second;
        engine_.reset();
        return false;
    } catch (...) {
        error_ = "circuit rejected (unknown error)";
        engine_.reset();
        return false;
    }

    refreshNodeNames();

    // **Every probe on the sheet, with the node it watches.** The scope draws
    // one trace per probe, each on its own colour, so a sheet with several
    // meters shows several signals at once.
    probeTraces_.clear();
    for (const auto& [id, net] : doc.allProbes()) {
        const int idx = engine_->nodeIndex(net);
        if (idx >= 0)
            probeTraces_.emplace_back(id, idx);
    }

    // Source bookkeeping. `toCircuit` puts the sources in `circuit_.src`; pick
    // the audio drive exactly the way the plugin does, so a document that plays
    // in the plugin also plays here.
    baseSources_.resize(circuit_.src.size());
    for (size_t i = 0; i < circuit_.src.size(); ++i)
        baseSources_[i] = circuit_.src[i].dc;

    // The AC settings, in the same order as the sources above. The frequency is
    // the signal's, not the network's, so it lives in the document and is read
    // from there -- the compiled circuit stays a topology description.
    acSettings_ = doc.sourceAcSettings();

    // The Input terminal's source, found by the part's id -- the Input
    // converts to a source *named after itself*, which is what makes this a
    // lookup rather than a guess.
    //
    // The fallback is **by name**: a source the user has called "Vin" or "in" is
    // them saying "this is the input", and the editor drives it.
    //
    // What the fallback is *not* is "a source with a non-zero AC amplitude" or
    // "the first source there is". Those were how a *signal source the user had
    // put in AC mode* ended up being driven by the editor's 440 Hz test signal,
    // every sample: the editor's scope showed a trace the user had not asked
    // for, and the circuit was being excited by something they could not see.
    //
    // So a sheet with no Input terminal and no "Vin" is excited by **its own**
    // sources -- a DC supply, an AC generator -- and the editor watches. That is
    // the editor being a lab rather than a demo.
    inSrcIdx_ = -1;
    inSrcName_.clear();

    const std::string inputId = doc.inputId();
    if (!inputId.empty()) {
        for (size_t i = 0; i < circuit_.src.size(); ++i) {
            if (circuit_.src[i].name == inputId) {
                inSrcIdx_ = static_cast<int>(i);
                inSrcName_ = circuit_.src[i].name;
                break;
            }
        }
    }

    if (inSrcIdx_ < 0) {
        for (size_t i = 0; i < circuit_.src.size(); ++i) {
            const auto& name = circuit_.src[i].name;
            if (name == "Vin" || name == "in") {
                inSrcIdx_ = static_cast<int>(i);
                inSrcName_ = name;
                break;
            }
        }
    }

    // **Which source's value is "what is going into the circuit"?** The one the
    // editor drives, or -- when it drives none -- the first source there is,
    // because that is the one whose value is going in.
    inputSrcIdx_ = inSrcIdx_;
    if (inputSrcIdx_ < 0 && !circuit_.src.empty())
        inputSrcIdx_ = 0;

    outIdx_ = engine_->nodeIndex(doc.outputNet());

    // The scope probe's node, when the sheet has one. The probe stamps nothing
    // into the circuit -- it only names a net -- and the editor's scope traces
    // this node instead of the output, so the probe is where you put your eye.
    resolveWatchedProbe(doc);

    solveDcNow();
    reset();
    return true;
}

CircuitSession::OutputCalibration CircuitSession::measureOutputReference(
    const wdf::LiveWDFDocument& doc, const std::atomic_bool& cancel) {
    constexpr int rate = 48000;
    if (cancel.load()) return {0., "Cancelled"};
    CircuitSession session;
    if (!session.compile(doc, rate)) return {0., session.lastError()};
    if (session.inputSourceIndex() < 0 || session.outputIndex() < 0)
        return {0., "Connect the audio input and output."};
    if (!session.dcConverged()) return {0., "DC operating point did not converge."};

    // One period of a broadband, -9 dBFS peak drive. Integer cycles give an
    // exact RMS reference and identical measurements on repeated runs.
    constexpr int period = rate / 125;
    std::array<double, period> signal{};
    constexpr int harmonics[] = {1, 3, 7, 15, 31, 63};
    constexpr double weights[] = {1., .8, .6, .45, .32, .22};
    for (int i = 0; i < period; ++i) {
        const double phase = 2. * std::acos(-1.) * i / period;
        for (int k = 0; k < 6; ++k)
            signal[i] += weights[k] * std::cos(harmonics[k] * phase);
        signal[i] *= std::pow(10., -9. / 20.) / 3.39;
    }

    constexpr int warm = 32 * period;
    constexpr int active = 96 * period;
    constexpr int measureFrom = warm + 16 * period;
    constexpr int tail = 64 * period;
    const double inputScale = doc.inputVoltsPerFullScale();
    double inputPower = 0., outputPower = 0., mean = 0., peak = 0.;
    for (int n = 0; n < warm + active + tail; ++n) {
        if (n % 64 == 0 && cancel.load()) return {0., "Cancelled"};
        const int i = n - warm;
        double dry = 0.;
        if (i >= 0 && i < active) {
            const double envelope = std::min({1., double(i) / period, double(active - i) / period});
            dry = signal[i % period] * envelope;
        }
        const double wet = session.step(inputScale * dry)[session.outputIndex()];
        if (session.engine_->lastStateRejected() || !std::isfinite(wet))
            return {0., "Circuit rejected a test sample."};
        peak = std::max(peak, std::abs(wet));
        if (n >= measureFrom && n < warm + active - period) {
            inputPower += dry * dry;
            // Welford's AC energy excludes the quiescent output voltage.
            const double delta = wet - mean;
            mean += delta / (n - measureFrom + 1);
            outputPower += delta * (wet - mean);
        }
    }
    if (!(outputPower > (warm + active - period - measureFrom) * 1e-18))
        return {0., "No measurable AC output."};
    const double divisor = std::max(std::sqrt(outputPower / inputPower),
                                    peak * std::pow(10., 1. / 20.));
    if (!std::isfinite(divisor)) return {0., "Output level is out of range."};
    return {divisor, {}};
}

void CircuitSession::clear() {
    engine_.reset();
    circuit_ = {};
    nodeNames_.clear();
    baseSources_.clear();
    scratch_.clear();
    dcVolts_.clear();
    dcAmps_.clear();
    dcStatus_.clear();
    dcConverged_ = false;
    errorPartId_.clear();
    errorPin_.clear();
    unconnected_.clear();
    outIdx_ = -1;
    inSrcIdx_ = -1;
    inSrcName_.clear();
    probeIdx_ = -1;
    probeLabel_.clear();
    probeTraces_.clear();
    inputNodeIdx_ = -1;
    error_.clear();
}

void CircuitSession::refreshNodeNames() {
    nodeNames_ = engine_ ? engine_->nodeNames() : std::vector<std::string>{};
}

void CircuitSession::watchProbe(const std::string& componentId, const std::string& net) {
    // The editor says which probe the user is looking at, and hands over the net
    // it watches -- the editor has the sheet, the session has the compiled
    // circuit, and this is the one fact that crosses between them.
    //
    // An empty net means "no probe": the scope goes back to the output.
    watchedProbeId_ = componentId;
    watchedProbeNet_ = net;

    probeIdx_ = -1;
    probeLabel_.clear();

    if (!engine_ || net.empty())
        return;

    probeIdx_ = engine_->nodeIndex(net);
    if (probeIdx_ >= 0)
        probeLabel_ = componentId;
}

void CircuitSession::resolveWatchedProbe(const wdf::LiveWDFDocument& doc) {
    probeIdx_ = -1;
    probeLabel_.clear();

    // The named probe if there is one, else the first probe on the sheet.
    std::string net = doc.probeNetFor(watchedProbeId_);
    std::string id = watchedProbeId_;

    if (net.empty()) {
        net = doc.probeNet();
        id = doc.probeId();
    }
    if (net.empty())
        return;

    probeIdx_ = engine_->nodeIndex(net);

    // The label is the probe's id, so the scope says *which* probe it is
    // looking at when the sheet has more than one.
    probeLabel_ = id;
}

void CircuitSession::setOversampling(wdf::OversamplingFactor factor) {
    if (factor != osFactor_) {
        osFactor_ = factor;
        oversampler_.setFactor(factor);
        oversampler_.reset();
        if (engine_) {
            const double effectiveFs = (osFactor_ == wdf::OversamplingFactor::X1)
                                           ? fs_
                                           : (fs_ * static_cast<double>(osFactor_));
            try {
                engine_ = std::make_unique<wdf::Engine>(circuit_, effectiveFs);
            } catch (...) {}
        }
    }
}

void CircuitSession::reset() {
    if (engine_) {
        engine_->reset();
        for (int p = 0; p < 64; ++p) {
            engine_->step(baseSources_);
        }
    }
    oversampler_.reset();

    // The phase restarts with the circuit, so a fresh test signal begins at a
    // zero crossing rather than halfway through a cycle.
    sampleIndex_ = 0;
}

const std::vector<double>& CircuitSession::stepInternal(double inputVolts) {
    std::vector<double> sv = baseSources_;

    // The AC sources: each one that has an amplitude and a frequency is
    // running its own sine, and the sample index says where in the cycle it
    // is. This is what makes "a voltage source AC of 1 Hz" a thing you can put
    // in a circuit and watch.
    //
    // The settings come from the document, in the same order as the sources
    // above, and the *current* sources sit after the voltage ones in the
    // engine's value vector.
    for (size_t i = 0; i < acSettings_.size(); ++i) {
        const auto& ac = acSettings_[i];
        if (ac.ac == 0.0 || ac.freq == 0.0)
            continue;

        // The settings list is the voltage sources then the current ones, and
        // the engine's value vector is laid out the same way: the voltage
        // sources first, the current ones after them.
        const size_t nVoltage = acSettings_.size() - circuit_.isrc.size();
        const size_t slot = (i < nVoltage) ? i : circuit_.src.size() + (i - nVoltage);
        if (slot >= sv.size())
            continue;

        const double effectiveFs = (osFactor_ == wdf::OversamplingFactor::X1)
                                       ? fs_
                                       : (fs_ * static_cast<double>(osFactor_));
        sv[slot] = wdf::sourceValueAt(baseSources_[slot], ac.ac, ac.freq, effectiveFs, sampleIndex_);
    }

    // The input source is the audio, which the caller supplies; it wins over
    // whatever the source's own DC or AC would have said.
    if (inSrcIdx_ >= 0 && inSrcIdx_ < static_cast<int>(sv.size()))
        sv[static_cast<size_t>(inSrcIdx_)] = inputVolts;

    // **What is going into the circuit.** The value on the input source, which
    // is the test signal when the editor is driving one, and the source's own
    // value when it is not.
    lastInputValue_ = 0.0;
    if (inputSrcIdx_ >= 0 && inputSrcIdx_ < static_cast<int>(sv.size()))
        lastInputValue_ = sv[static_cast<size_t>(inputSrcIdx_)];

    ++sampleIndex_;

    return engine_->step(sv);
}

const std::vector<double>& CircuitSession::step(double inputVolts) {
    if (!engine_) {
        scratch_.assign(nodeNames_.size(), 0.0);
        return scratch_;
    }

    if (osFactor_ == wdf::OversamplingFactor::X1) {
        return stepInternal(inputVolts);
    }

    oversampler_.process(inputVolts, [&](double up) -> double {
        const auto& nodes = stepInternal(up);
        lastStepNodes_ = nodes;
        return (outIdx_ >= 0 && outIdx_ < static_cast<int>(nodes.size()))
                   ? nodes[static_cast<size_t>(outIdx_)]
                   : 0.0;
    });

    return lastStepNodes_;
}

void CircuitSession::solveDcNow() {
    dcVolts_.clear();
    dcAmps_.clear();
    dcStatus_.clear();
    dcConverged_ = false;

    if (!engine_) {
        dcStatus_ = error_.empty() ? "no circuit" : error_;
        return;
    }

    std::vector<double> node, rootV;
    const bool ok = engine_->solveDc(node, rootV);

    dcConverged_ = ok;
    for (size_t i = 0; i < node.size() && i < nodeNames_.size(); ++i) {
        // `solveDc` reports ground as 0 by omission (ground is not a free node),
        // so only free nodes appear here -- which is exactly the list a reader
        // needs.
        dcVolts_[nodeNames_[i]] = node[i];
    }

    // Device currents, from the solved root-port voltages. The port order is
    // `EngineCore::rootLayout()`, not a second copy of the arithmetic: a new
    // root device kind must not require the editor to learn its offset. We
    // report plate/drain and diode currents, which are the ones a reader checks
    // while drawing; grid/cathode currents are small and would only add noise.
    {
        std::vector<double> i, J;
        if (!rootV.empty())
            engine_->currents(rootV, i, J);

        int triodeIndex = 0, diodeIndex = 0, jfetIndex = 0;
        for (const auto& dev : engine_->rootLayout().devices) {
            const size_t o = static_cast<size_t>(dev.portBase);
            if (o >= i.size())
                continue;
            switch (dev.kind) {
            case wdf::RootKind::Triode:
                dcAmps_["triode " + std::to_string(++triodeIndex) + " plate"] = i[o];
                break;
            case wdf::RootKind::Diode:
                dcAmps_["diode " + std::to_string(++diodeIndex)] = i[o];
                break;
            case wdf::RootKind::Jfet:
                dcAmps_["jfet " + std::to_string(++jfetIndex) + " drain"] = i[o];
                break;
            case wdf::RootKind::ControlledRes:
            case wdf::RootKind::Pentode:
                break;
            }
        }
    }

    if (ok) {
        dcStatus_ = "converged";
    } else {
        dcStatus_ = "DC did not converge -- check for a floating node or a "
                    "missing ground path";
    }
}

}  // namespace livewdf::editor
