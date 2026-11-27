#include "format/Document.h"

#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "devices/Bjt.h"
#include "devices/Diode.h"
#include "devices/Jfet.h"
#include "devices/Mosfet.h"
#include "devices/MosfetPresets.h"
#include "devices/OpAmp.h"
#include "devices/Pentode.h"
#include "devices/QuadricTriode.h"
#include "devices/Triode.h"
#include "format/TransistorPolarity.h"

namespace wdf {

Circuit LiveWDFDocument::toCircuit() const {
    Circuit c;
    struct DocumentBucketBrigade {
        std::string input, output, clock, common, name;
        BucketBrigadeModel model;
    };
    std::vector<DocumentBucketBrigade> bucketBrigades;
    struct DocumentRmsDetector {
        std::string input, output, common, name;
        RmsDetectorModel model;
    };
    std::vector<DocumentRmsDetector> rmsDetectors;

    // Rails, keyed by the net they drive: (dc volts, ac volts, part id).
    //
    // A rail is a supply *declaration*, not an independent generator, and a
    // schematic often draws the same supply beside more than one stage rather
    // than running a long wire. Those repeats are one source. The engine's
    // symbolic source tracking cannot tell a repeat from a contradiction -- it
    // compares source symbols, not their values -- so collapse the repeats here
    // and let only a genuine value conflict reach the user.
    std::map<std::string, std::tuple<double, double, std::string>> railSupplies;

        // **A part joined to nothing is not in the circuit.**
        //
        // A part whose every terminal is on a net of its own -- an island --
        // is not connected to the circuit it is drawn on. Compiling it in makes
        // the solver's matrix singular: the part's own two terminals are
        // joined by nothing but the part, which is a loop with no reference to
        // ground in it, and the DC solve cannot find a voltage for it.
        //
        // The user drew that part, so it stays on the sheet and in the file;
        // it is simply not part of the circuit, and the editor says so.
        const auto islands = isolatedParts();

        for (const auto& comp : components) {
            if (islands.count(comp.id) != 0)
                continue;
            const auto getPin = [&](const std::string& pinName, const std::string& fallback = "0") -> std::string {
                auto it = comp.pins.find(pinName);
                return (it != comp.pins.end() && !it->second.empty()) ? it->second : fallback;
            };
            const auto getVal = [&](const std::string& valName, double fallback = 0.0) -> double {
                auto it = comp.values.find(valName);
                return (it != comp.values.end()) ? it->second : fallback;
            };
            const auto getProp = [&](const std::string& propName, const std::string& fallback = "") -> std::string {
                auto it = comp.properties.find(propName);
                return (it != comp.properties.end()) ? it->second : fallback;
            };

            if (getProp("preampOnly") == "1")
                continue;
            if (comp.type == "res") {
                c.addRes(getPin("a"), getPin("b"), getVal("value", 1000.0), comp.id);
            } else if (comp.type == "cap") {
                const auto initial = comp.values.find("initialVolts");
                c.addCap(getPin("a"), getPin("b"), getVal("value", 1e-6), comp.id,
                         initial != comp.values.end() ? std::optional<double>(initial->second)
                                                      : std::nullopt);
            } else if (comp.type == "ind") {
                c.addInd(getPin("a"), getPin("b"), getVal("value", 1e-3), comp.id);
            } else if (comp.type == "pot") {
                const std::string taperStr = getProp("taper", "Linear");
                PotTaper taper = PotTaper::Linear;
                if (taperStr == "Log") taper = PotTaper::Log;
                else if (taperStr == "RevLog") taper = PotTaper::RevLog;
                c.addPot(getPin("high"), getPin("wiper"), getPin("low"),
                         getVal("totalR", 100000.0), getVal("pos", 0.5), taper, comp.id);
            } else if (comp.type == "varres") {
                const std::string taperStr = getProp("taper", "Linear");
                PotTaper taper = PotTaper::Linear;
                if (taperStr == "Log") taper = PotTaper::Log;
                else if (taperStr == "RevLog") taper = PotTaper::RevLog;
                c.addVarRes(getPin("a"), getPin("b"), getVal("maxR", 10000.0), getVal("pos", 0.5), taper, comp.id);
            } else if (comp.type == "ctrlres") {
                c.addControlledRes(getPin("a"), getPin("b"), getVal("initialOhms", 1000.0), comp.id);
            } else if (comp.type == "vca") {
                c.addLinearVca(getPin("in"), getPin("out"), getVal("inputOhms", 2200.0),
                               {getPin("control"), getPin("reference"),
                                getVal("fullGainVolts", 1.0)}, comp.id);
            } else if (comp.type == "dbx200") {
                c.addDbx200(getPin("in"), getPin("out"), getPin("pi"), getPin("ni"),
                            getPin("po"), getPin("no"), getPin("common"), comp.id);
            } else if (comp.type == "that2180") {
                c.addThat2180(getPin("in"), getPin("out"), getPin("ecPlus"),
                              getPin("ecMinus"), getPin("reference"),
                              getVal("inputOhms", 25.0), comp.id);
            } else if (comp.type == "photores") {
                PhotoresistorControl photo;
                photo.sensePos = getPin("sensePos");
                photo.senseNeg = getPin("senseNeg");
                photo.brightOhms = getVal("brightOhms", photo.brightOhms);
                photo.referenceVolts = getVal("referenceVolts", photo.referenceVolts);
                photo.gamma = getVal("gamma", photo.gamma);
                photo.attackSeconds = getVal("attackSeconds", photo.attackSeconds);
                photo.releaseSeconds = getVal("releaseSeconds", photo.releaseSeconds);
                c.addPhotoresistor(getPin("a"), getPin("b"), getVal("darkOhms", 1e6),
                                   photo, comp.id);
            } else if (comp.type == "ca3080") {
                c.addCa3080(getPin("out"), getPin("neg"), getPin("pos"), getPin("vcc"),
                           getPin("vee"), getPin("bias"), comp.id);
            } else if (comp.type == "diode") {
                const std::string modelName = getProp("model", "1N4148");
                const auto* entry = findDiodeByName(modelName);
                DiodeModel dm = entry ? entry->model()
                                      : DiodeModel(getVal("isat", 1e-14), getVal("n", 1.0),
                                                   getVal("rs", 0.0), 1e6, getVal("bv", 0.0),
                                                   getVal("ibv", 1e-3));
                c.addDiode(getPin("anode"), getPin("cathode"), dm, comp.id);
            } else if (comp.type == "triode") {
                const std::string modelName = getProp("model", "12AX7");
                const bool isCustom = modelName == "Custom";
                const auto hasQuadric = [&comp](const char* key) {
                    return comp.values.find(key) != comp.values.end();
                };
                const bool hasQuadricValues = hasQuadric("kp2") && hasQuadric("kpg") && hasQuadric("kp");
                const auto* qp = quadricTriodeByName(modelName.c_str());
                if (qp && hasQuadricValues) {
                    // Recognized card with edited values -- build model from them.
                    TriodeModel tm = quadricTriode(wdf::QuadricTriodeParams{
                        comp.values.at("kp2"), comp.values.at("kpg"), comp.values.at("kp")});
                    c.addTri(getPin("grid"), getPin("plate"), getPin("cathode"),
                             getPin("internalGrid", getPin("grid")), tm);
                } else if (qp) {
                    // Recognized card, no edited values -- use card defaults.
                    c.addTri(getPin("grid"), getPin("plate"), getPin("cathode"),
                             getPin("internalGrid", getPin("grid")), quadricTriode(*qp));
                } else if (isCustom && hasQuadricValues) {
                    // Custom model with its three quadric values provided.
                    TriodeModel tm = quadricTriode(wdf::QuadricTriodeParams{
                        comp.values.at("kp2"), comp.values.at("kpg"), comp.values.at("kp")});
                    c.addTri(getPin("grid"), getPin("plate"), getPin("cathode"),
                             getPin("internalGrid", getPin("grid")), tm);
                } else {
                    // Fallback to 12AX7 paper fit.
                    c.addTri(getPin("grid"), getPin("plate"), getPin("cathode"),
                             getPin("internalGrid", getPin("grid")), k12ax7Quadric());
                }
            } else if (comp.type == "pentode") {
                const std::string modelName = getProp("model", "EL34");
                const bool isCustom = modelName == "Custom";
                const auto hasPentode = [&comp](const char* key) {
                    return comp.values.find(key) != comp.values.end();
                };
                const bool hasPentodeValues = hasPentode("mu") && hasPentode("ex") && hasPentode("kg1")
                    && hasPentode("kg2") && hasPentode("kp") && hasPentode("kvb")
                    && hasPentode("vg2") && hasPentode("vmax") && hasPentode("count")
                    && hasPentode("gridis") && hasPentode("gridvt");
                const auto* pm = findPentodeByName(modelName);
                if (pm && hasPentodeValues) {
                    // Recognized card with edited values -- build model from them.
                    wdf::Pentode pmModel;
                    pmModel.mu = comp.values.at("mu");
                    pmModel.ex = comp.values.at("ex");
                    pmModel.kg1 = comp.values.at("kg1");
                    pmModel.kg2 = comp.values.at("kg2");
                    pmModel.kp = comp.values.at("kp");
                    pmModel.kvb = comp.values.at("kvb");
                    pmModel.vg2 = comp.values.at("vg2");
                    pmModel.v_max = comp.values.at("vmax");
                    pmModel.count = comp.values.at("count");
                    pmModel.grid_is = comp.values.at("gridis");
                    pmModel.grid_vt = comp.values.at("gridvt");
                    c.addPentode(getPin("plate"), getPin("screen"), getPin("grid"), getPin("cathode"), pmModel, comp.id);
                } else if (pm) {
                    // Recognized card, no edited values -- use card defaults.
                    wdf::Pentode model = *pm;
                    const std::string kneeProp = getProp("knee", "");
                    if (kneeProp == "derke" || kneeProp == "DerkE")
                        model.reefman.knee = wdf::PentodeKneeType::DerkE;
                    else if (kneeProp == "derkd" || kneeProp == "DerkD")
                        model.reefman.knee = wdf::PentodeKneeType::DerkD;
                    else if (kneeProp == "koren" || kneeProp == "Koren") {
                        model.modelKind = wdf::PentodeModelKind::Koren;
                        model.useReefman = false;
                    }
                    const std::string modelProp = getProp("pentode_model", getProp("pentodeModel", ""));
                    if (modelProp == "koren" || modelProp == "Koren") {
                        model.modelKind = wdf::PentodeModelKind::Koren;
                        model.useReefman = false;
                    }
                    const std::string seProp = getProp("secondary_emission", getProp("se", ""));
                    if (seProp == "true" || seProp == "1" || seProp == "on")
                        model.reefman.secondaryEmission = true;
                    else if (seProp == "false" || seProp == "0" || seProp == "off")
                        model.reefman.secondaryEmission = false;
                    c.addPentode(getPin("plate"), getPin("screen"), getPin("grid"), getPin("cathode"), model, comp.id);
                } else if (isCustom && hasPentodeValues) {
                    // Custom model with its parameters provided.
                    wdf::Pentode pmModel;
                    pmModel.mu = comp.values.at("mu");
                    pmModel.ex = comp.values.at("ex");
                    pmModel.kg1 = comp.values.at("kg1");
                    pmModel.kg2 = comp.values.at("kg2");
                    pmModel.kp = comp.values.at("kp");
                    pmModel.kvb = comp.values.at("kvb");
                    pmModel.vg2 = comp.values.at("vg2");
                    pmModel.v_max = comp.values.at("vmax");
                    pmModel.count = comp.values.at("count");
                    pmModel.grid_is = comp.values.at("gridis");
                    pmModel.grid_vt = comp.values.at("gridvt");
                    c.addPentode(getPin("plate"), getPin("screen"), getPin("grid"), getPin("cathode"), pmModel, comp.id);
                } else {
                    // Fallback to EL34.
                    c.addPentode(getPin("plate"), getPin("screen"), getPin("grid"), getPin("cathode"), kEL34, comp.id);
                }
            } else if (comp.type == "jfet") {
                const std::string modelName = getProp("model", "2N5952");
                const TransistorPolarity polarity = transistorPolarity(comp);
                const bool pChannel = polarity == TransistorPolarity::PType;
                // If parameter values have been edited (present in values),
                // build the model from them and keep the preset identity.
                const bool hasBeta = comp.values.find("beta") != comp.values.end();
                const bool hasVto  = comp.values.find("vto")  != comp.values.end();
                const bool hasLambda = comp.values.find("lambda") != comp.values.end();
                if (hasBeta && hasVto && hasLambda) {
                    const double beta = comp.values.at("beta");
                    const double vto  = comp.values.at("vto");
                    const double lambda = comp.values.at("lambda");
                    const double rd = comp.values.count("rd") ? comp.values.at("rd") : 1.0;
                    const double rs = comp.values.count("rs") ? comp.values.at("rs") : 1.0;
                    const double cgs = comp.values.count("cgs") ? comp.values.at("cgs") : 0.0;
                    const double cgd = comp.values.count("cgd") ? comp.values.at("cgd") : 0.0;
                    const double cds = comp.values.count("cds") ? comp.values.at("cds") : 0.0;
                    const wdf::JfetModel userModel = wdf::JfetModel::fromSpice(
                        beta, vto, lambda, rd, rs, cgs, cgd, cds, 1.0e9, pChannel);
                    c.addJfet(getPin("drain"), getPin("gate"), getPin("source"), userModel, comp.id);
                } else {
                    const auto* jm = wdf::findJfetByName(modelName.c_str());
                    if (!jm)
                        throw std::runtime_error("unknown JFET model '" + modelName +
                                                 "' on part '" + comp.id + "'");
                    c.addJfet(getPin("drain"), getPin("gate"), getPin("source"), jm->model, comp.id);
                }
            } else if (comp.type == "mosfet") {
                // A recognized preset supplies the nominal numbers and the
                // polarity. Numbers stored on the part override those defaults,
                // so editing K/Vth/lambda in the inspector actually changes the
                // device; the preset still decides polarity, which is why an
                // explicit polarity property cannot relabel a named card.
                // "Custom" (or any unknown name) requires all three values and
                // reads the explicit polarity property.
                const std::string preset = getProp("preset", "Custom");
                const MosfetPreset* card = findMosfetPreset(preset);
                double kVal = 1.0e-3, vthVal = 1.0, lambdaVal = 0.01;
                bool pChannel = false;
                if (card) {
                    kVal = card->K;
                    vthVal = card->Vth;
                    lambdaVal = card->lambda;
                    pChannel = card->pChannel;
                } else {
                    const auto requireVal = [&](const std::string& valName) -> double {
                        const auto it = comp.values.find(valName);
                        if (it == comp.values.end())
                            throw std::runtime_error("MOSFET '" + comp.id + "' is missing '" + valName + "'");
                        return it->second;
                    };
                    kVal = requireVal("k");
                    vthVal = requireVal("vth");
                    lambdaVal = requireVal("lambda");
                    pChannel = transistorPolarity(comp) == TransistorPolarity::PType;
                }
                if (const auto it = comp.values.find("k"); it != comp.values.end())
                    kVal = it->second;
                if (const auto it = comp.values.find("vth"); it != comp.values.end())
                    vthVal = it->second;
                if (const auto it = comp.values.find("lambda"); it != comp.values.end())
                    lambdaVal = it->second;
                const std::string source = getPin("source");
                const std::string bulk = getPin("bulk", source);
                if (bulk != source)
                    throw std::runtime_error("MOSFET '" + comp.id +
                                             "' has a bulk pin different from its source; "
                                             "this model has no body effect");
                const MosfetModel model =
                    MosfetModel::fromK(kVal, vthVal, lambdaVal, pChannel);
                if (!model.valid())
                    throw std::runtime_error("MOSFET '" + comp.id +
                                             "' has invalid K/Vth/lambda values");
                c.addMosfet(getPin("drain"), getPin("gate"), source, model, comp.id);
            } else if (comp.type == "bjt") {
                // Polarity is carried by the model card. Both NPN and PNP are
                // supported. A recognized card supplies the defaults, and
                // edited Is/Bf/Br/Nf/Nr values override its transport values.
                // "Custom" requires the five Ebers-Moll values explicitly.
                // Low-current corrections come from the named opt-in cards.
                const std::string modelName = getProp("model", "2N3904");
                const auto* entry = wdf::findBjtByName(modelName);
                const bool isCustom = modelName == "Custom";
                const auto hasEm = [&comp](const char* key) {
                    return comp.values.find(key) != comp.values.end();
                };
                const bool hasEmValues = hasEm("is") && hasEm("bf") && hasEm("br") &&
                                         hasEm("nf") && hasEm("nr");
                if (entry && hasEmValues) {
                    // Recognized card with edited values -- build model from them.
                    wdf::BjtModel userModel = entry->model;
                    userModel.Is = comp.values.at("is");
                    userModel.Bf = comp.values.at("bf");
                    userModel.Br = comp.values.at("br");
                    userModel.Nf = comp.values.at("nf");
                    userModel.Nr = comp.values.at("nr");
                    userModel.prepare();
                    if (!userModel.valid())
                        throw std::runtime_error("BJT '" + comp.id + "' has invalid model values");
                    c.addBjt(getPin("collector"), getPin("base"), getPin("emitter"),
                             userModel, comp.id);
                } else if (entry) {
                    // Recognized card, no edited values -- use card defaults.
                    c.addBjt(getPin("collector"), getPin("base"), getPin("emitter"),
                             entry->model, comp.id);
                } else if (isCustom && hasEmValues) {
                    // Custom model with its five EM values provided.
                    const bool pnp = transistorPolarity(comp) == TransistorPolarity::PType;
                    const wdf::BjtModel userModel(
                        comp.values.at("is"), comp.values.at("bf"), comp.values.at("br"),
                        comp.values.at("nf"), comp.values.at("nr"), pnp);
                    if (!userModel.valid())
                        throw std::runtime_error("BJT '" + comp.id + "' has invalid model values");
                    c.addBjt(getPin("collector"), getPin("base"), getPin("emitter"),
                             userModel, comp.id);
                } else {
                    throw std::runtime_error("unsupported BJT model '" + modelName +
                                             "' on part '" + comp.id + "'");
                }
            } else if (comp.type == "dbx209") {
                const auto model = RmsDetectorModel::dbx209(getVal("referenceAmps", 23.34e-6),
                                                            getVal("timingFarads", 22e-6));
                rmsDetectors.push_back({getPin("in"), getPin("out"), getPin("common"),
                                        comp.id, model});
            } else if (comp.type == "rmsdet") {
                RmsDetectorModel model;
                model.referenceVolts = getVal("referenceVolts", model.referenceVolts);
                model.timeConstantSeconds = getVal("timeConstantSeconds", model.timeConstantSeconds);
                model.attackTimeConstantSeconds = getVal("attackTimeConstantSeconds", 0.0);
                model.releaseTimeConstantSeconds = getVal("releaseTimeConstantSeconds", 0.0);
                model.voltsPerDb = getVal("voltsPerDb", model.voltsPerDb);
                model.inputOhms = getVal("inputOhms", model.inputOhms);
                model.outputOhms = getVal("outputOhms", model.outputOhms);
                rmsDetectors.push_back({getPin("in"), getPin("out"), getPin("common"),
                                        comp.id, model});
            } else if (comp.type == "bbd") {
                BucketBrigadeModel model;
                const double stages = getVal("stages", model.stages);
                if (!std::isfinite(stages) || stages < 2 || stages > 32768 ||
                    std::floor(stages) != stages)
                    throw std::invalid_argument("Invalid bucket-brigade stage count: " + comp.id);
                model.stages = static_cast<int>(stages);
                model.clockHz = getVal("clockHz", model.clockHz);
                model.periodSecondsPerVolt = getVal("periodMicrosecondsPerVolt", 0.0) * 1.0e-6;
                model.inputBias = getVal("inputBias", model.inputBias);
                model.outputBias = getVal("outputBias", model.outputBias);
                model.inputSwing = getVal("inputSwing", model.inputSwing);
                model.gain = getVal("gain", model.gain);
                model.outputOhms = getVal("outputOhms", model.outputOhms);
                model.transferLoss = getVal("transferLoss", model.transferLoss);
                model.kneeWidth = getVal("kneeWidth", model.kneeWidth);
                model.evenOrder = getVal("evenOrder", model.evenOrder);
                bucketBrigades.push_back({getPin("in"), getPin("out"), getPin("clock"),
                                          getPin("common"), comp.id, model});
            } else if (comp.type == "vcvs") {
                // The gain block has **two** terminals, in and out; the
                // reference is ground. It used to have four -- in+, in-, out+,
                // out- -- which is the same circuit written out in full, and it
                // drew as a symbol with four dots on it. Two is what a gain
                // block looks like, and the two references are ground.
                c.addVCVS(getPin("out"), "0", getPin("in"), "0",
                          getVal("gain", 1.0), getVal("ro", 1e-4), comp.id);
            } else if (comp.type == "opamp") {
                const std::string modelName = getProp("model", "TL072");
                const OpAmpType type = opAmpTypeFromString(modelName);
                OpAmpModel model = OpAmpModel::fromType(type);
                if (comp.values.count("vRailMin") || comp.values.count("vRailMax")) {
                    const double low = getVal("vRailMin", model.railLow());
                    const double high = getVal("vRailMax", model.railHigh());
                    if (!std::isfinite(low) || !std::isfinite(high) || low > high)
                        throw std::runtime_error("Op-amp '" + comp.id + "' has invalid rail bounds");
                    model.setVRailBounds(low, high);
                } else if (comp.values.count("vRailPos") && comp.values.count("vRailNeg")) {
                    model.setVRails(comp.values.at("vRailPos"), comp.values.at("vRailNeg"));
                }
                c.addOpAmp(getPin("out"), getPin("neg"), getPin("pos"),
                           getPin("vcc", "0"), getPin("vee", "0"), model, comp.id);
            } else if (comp.type == "idealopamp") {
                // The ideal op-amp is the limit of the op-amp model with no
                // supply pins to read and no headroom to clip against. The VCVS
                // is exactly that element: a transconductance from (pos, neg)
                // to (out, ground) behind a negligible output resistance, and
                // memoryless, so there is no pole, no slew and no clip.
                //
                // 120 dB open-loop gain and 0.1 ohm output resistance: far
                // beyond any loop a pedal closes, and still tiny beside the
                // 10k-and-up loads a pedal presents. The test in
                // `test_element_access.cpp` pins the resulting closed-loop
                // gain against the document converter.
                const std::string gnd = groundNode.empty() ? std::string("0") : groundNode;
                c.addVCVS(getPin("out"), gnd, getPin("pos"), getPin("neg"),
                          1.0e5, 0.1, comp.id);
            } else if (comp.type == "transformer") {
                TransformerSpec spec;
                spec.name = comp.id;
                spec.turnsPrimary = getVal("turnsPrimary", 1.0);
                spec.turnsSecondary = getVal("turnsSecondary", 1.0);
                spec.model = getProp("model", "Real — Auto");
                spec.Lm = getVal("Lm", 0.0);
                spec.leakage = getVal("leakage", 0.0);
                spec.dcrPrimary = getVal("dcrPrimary", 0.0);
                spec.dcrSecondary = getVal("dcrSecondary", 0.0);
                spec.cp = getVal("cp", 0.0);
                spec.rc = getVal("rc", 0.0);
                c.addTransformerElement(getPin("p1"), getPin("p2"),
                                        getPin("s1"), getPin("s2"), spec, comp.id);
            } else if (comp.type == "transformer_ct") {
                // The visible part is three primary terminals (with the middle
                // one at the tap) and two secondary terminals. Like the plain
                // transformer, it compiles to one coupled inductance matrix, not
                // to a loose netlist of ordinary parts.
                TransformerSpec spec;
                spec.name = comp.id;
                spec.turnsPrimary = getVal("turnsPrimary", 1.0);
                spec.turnsSecondary = getVal("turnsSecondary", 1.0);
                spec.model = getProp("model", "Real — Auto");
                spec.Lm = getVal("Lm", 0.0);
                spec.leakage = getVal("leakage", 0.0);
                spec.dcrPrimary = getVal("dcrPrimary", 0.0);
                spec.dcrSecondary = getVal("dcrSecondary", 0.0);
                spec.cp = getVal("cp", 0.0);
                spec.rc = getVal("rc", 0.0);
                c.addCenterTapTransformerElement(getPin("p1"), getPin("pt"), getPin("p2"),
                                                 getPin("s1"), getPin("s2"), spec, comp.id);
            } else if (comp.type == "src") {
                // The signal source: a DC voltage, or an AC one.
                //
                // **A source is one or the other.** In DC mode the AC
                // amplitude is not merely hidden in the inspector -- it is *not
                // in the circuit*, so the engine sees a plain DC source and the
                // two can never both apply.
                //
                // Only the *amplitude* goes into the compiled circuit. The
                // frequency is the signal's, not the network's: it lives in the
                // document's `values` and the driver applies it (see
                // `format/AcSource.h`). The compiled circuit stays a topology
                // description, which is what the engine wants.
                const bool ac = sourceModeOf(comp) == SourceMode::AC;
                c.addSrc(getPin("pos"), getPin("neg"), getVal("dc", 0.0),
                         ac ? getVal("ac", 0.0) : 0.0, {}, comp.id);
            } else if (comp.type == "isrc") {
                const bool ac = sourceModeOf(comp) == SourceMode::AC;
                c.addIsrc(getPin("pos"), getPin("neg"), getVal("dc", 0.0),
                          ac ? getVal("ac", 0.0) : 0.0, {}, comp.id);
            } else if (comp.type == "rail") {
                // Single-terminal voltage source: a rail whose negative terminal
                // is implicit ground. Supports DC and AC like "src".
                //
                // **Repeats are one supply; conflicting values are an error.**
                // A user who draws a 4.5 V bias rail at the input and another
                // 4.5 V rail at the next stage has declared one supply twice.
                // A user who puts 4.5 V and 9 V on the same net has drawn a
                // contradiction, and the old message -- "inconsistent ideal
                // sources: v(VDC1)-v(0)" -- named neither part. Both are caught
                // here; only the second throws.
                const bool ac = sourceModeOf(comp) == SourceMode::AC;
                const std::string gnd = groundNode.empty() ? std::string("0") : groundNode;
                const std::string net = getPin("pos");
                const double dc = getVal("dc", 0.0);
                const double acVolts = ac ? getVal("ac", 0.0) : 0.0;

                const auto found = railSupplies.find(net);
                if (found == railSupplies.end()) {
                    railSupplies.emplace(net, std::make_tuple(dc, acVolts, comp.id));
                    c.addSrc(net, gnd, dc, acVolts, {}, comp.id);
                    continue;
                }

                const auto& [haveDc, haveAc, haveId] = found->second;
                const bool sameDc = std::abs(haveDc - dc) <= 1e-12;
                const bool sameAc = std::abs(haveAc - acVolts) <= 1e-12;
                if (sameDc && sameAc)
                    continue;

                const auto describe = [](double vdc, double vac) {
                    std::ostringstream s;
                    s << vdc << " V DC";
                    if (vac != 0.0)
                        s << " + " << vac << " V AC";
                    return s.str();
                };
                throw std::runtime_error(
                    "rails '" + haveId + "' (" + describe(haveDc, haveAc) + ") and '" +
                    comp.id + "' (" + describe(dc, acVolts) +
                    ") both drive net '" + net + "' with different values");
            } else if (comp.type == "in") {
                // The Input terminal becomes a voltage source between its pin
                // and ground, named after the part.
                //
                // A WDF engine injects signal at *sources*, not at arbitrary
                // nodes, so a terminal that is not a source cannot carry the
                // audio. Naming the source after the part is what lets the
                // plugin find *this* one: the old arrangement guessed, by
                // looking for a source called "Vin" or one with a non-zero AC
                // amplitude, and the signal source was wearing the input's hat.
                const std::string gnd = groundNode.empty() ? std::string("0") : groundNode;
                c.addSrc(getPin("sig"), gnd, 0.0, 0.0, {}, comp.id);
            } else if (comp.type == "switch_res") {
                // A changeover (SPDT) switch: common connects to one throw or
                // the other, never both. Celine models each contact as an
                // ordinary resistor, <id>_A and <id>_B, in opposition:
                // `select` "0" closes A, "1" closes B.
                //
                // **An ordinary resistor, not a controlled one.** A controlled
                // resistor is a root port, and the engine's root network cannot
                // solve a node whose only element is that port -- which is
                // exactly the node an unused throw, or a switch feeding only
                // the Output terminal, would have. A regular resistor is a
                // tree element, so a dangling contact is simply a gigaohm lead
                // to the common; Celine's own Switch.h makes the same choice.
                //
                // These are Celine's constants: 0.01 ohm closed, 1 Gohm open.
                const bool selectB = switchResSelectsB(comp);
                const double rA = selectB ? kSpdtOpenOhms : kSpdtClosedOhms;
                const double rB = selectB ? kSpdtClosedOhms : kSpdtOpenOhms;

                const std::string common = getPin("in", "");
                const std::string throwA = getPin("outA", "");
                const std::string throwB = getPin("outB", "");
                if (!common.empty()) {
                    if (!throwA.empty())
                        c.addRes(common, throwA, rA, comp.id + "_A");
                    if (!throwB.empty())
                        c.addRes(common, throwB, rB, comp.id + "_B");
                }
            }
            // The Output terminal stamps nothing: it is a marker that names the
            // node the plugin reads, and a node it names is a node some other
            // part's pin is on anyway.
        }
        // Internal drives follow explicit current sources to preserve the
        // slots used by the source driver.
        for (const auto& bbd : bucketBrigades)
            c.addBucketBrigade(bbd.input, bbd.output, bbd.clock, bbd.common, bbd.model, bbd.name);
        for (const auto& rms : rmsDetectors)
            c.addRmsDetector(rms.input, rms.output, rms.common, rms.model, rms.name);
        return c;
    }

} // namespace wdf
