#pragma once

// Netlist -> Circuit, ported from proto/spice_net.py.
//
// The preamp is a *netlist*, not logic: its topology is settled by the schematic.
// Reading it here rather than hand-transcribing it is what keeps the C++ engine
// honest against the one circuit ngspice actually simulated.
//
// Understood: `R`/`C` with SPICE suffixes (`150K`, `0.1U`, `750P`, `10MEG`) and
// Qucs-S pot expressions (`R='(250K)*(80)/(100.0)'`); `V` sources with `DC`, `AC`
// and a raw `SIN(...)`; `X` tube instantiations of `Tubes_12AX7`; `D` diodes whose
// `IS`/`N`/`RS` come from their own top-level `.model` line; and `.param`,
// `.model`, `.include` and `.control` blocks. The tube's *passive* parameters --
// `rgi`, `vct`, `ccg`, `cgp`, `ccp` -- are read back out of the netlist's own
// `12AX7_model`, so the network matches the reference. The plate-current law is
// always the D'Angelo quadric 12AX7; the `Tubes.lib` Koren
// parameters are not used.
//
// The subckt wiring (`Tubes_12AX7 gnd _net1 _net0 _net2` -> `12AX7_model 1 6 3`)
// fixes the pin meaning: pin 1 cathode, pin 2 grid, pin 3 plate. Inside,
// c1 = ccg, c2 = cgp, c3 = ccp, r1 = rgi, v1 = vct -- so one `X` line becomes
// the same handful of elements the prototype's `add_tube` builds. The subckt's
// grid diode is not added: the quadric plate law carries no grid current.
//
// Anything else is refused, never skipped: an element letter this parser does not
// know, a known letter with the wrong shape, or an `X` naming anything but
// `Tubes_12AX7` all throw with the offending line attached. A parser that drops
// what it does not understand compiles the netlist to a *different* circuit with no
// sign of it -- which is how the first draft of the extra fixtures lost a supply and
// the engine then ran an unintended floating topology. So a component is added to the
// checker before it is added here (PLAN section 6, item #0). Lines beginning `.` are
// the one deliberate read-past: `.param` and `.subckt` are consumed above and
// `.model` / `.tran` / `.end` are statements a `Circuit` does not need -- directives,
// not elements.

#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "spice/Circuit.h"

namespace wdf {

// --------------------------------------------------------------------------- //
// Values
// --------------------------------------------------------------------------- //

/// SPICE multipliers. Order matters: MEG and MIL must be tested before M.
inline const std::vector<std::pair<const char*, double>>& spiceSuffixes() {
    static const std::vector<std::pair<const char*, double>> k = {
        {"MEG", 1e6}, {"MIL", 25.4e-6}, {"T", 1e12}, {"G", 1e9}, {"K", 1e3},
        {"M", 1e-3},  {"U", 1e-6},      {"N", 1e-9}, {"P", 1e-12}, {"F", 1e-15}};
    return k;
}

/// Recursive-descent evaluator for a SPICE scalar or a Qucs-S pot expression
/// (`(250K)*(1.0-(80)/(100.0))`). Handles unary +/-, the four operators, parens,
/// and numbers with SPICE suffixes. Nothing else is accepted, which is what makes
/// it safe where the prototype used Python's `eval`.
class ExprParser {
public:
    explicit ExprParser(const std::string& s) : s_(s) {}

    double parse() {
        const double v = expr();
        skip();
        if (i_ != static_cast<int>(s_.size()))
            throw std::runtime_error("trailing text in expression: " + s_);
        return v;
    }

private:
    void skip() {
        while (i_ < static_cast<int>(s_.size()) && std::isspace(static_cast<unsigned char>(s_[i_])))
            ++i_;
    }
    bool eat(char c) {
        skip();
        if (i_ < static_cast<int>(s_.size()) && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    double expr() {
        double v = term();
        for (;;) {
            skip();
            if (eat('+'))
                v += term();
            else if (eat('-'))
                v -= term();
            else
                return v;
        }
    }
    double term() {
        double v = factor();
        for (;;) {
            skip();
            if (eat('*'))
                v *= factor();
            else if (eat('/'))
                v /= factor();
            else
                return v;
        }
    }
    double factor() {
        skip();
        if (eat('+'))
            return factor();
        if (eat('-'))
            return -factor();
        if (eat('(')) {
            const double v = expr();
            eat(')');
            return v;
        }
        return number();
    }
    double number() {
        skip();
        const size_t start = static_cast<size_t>(i_);
        while (i_ < static_cast<int>(s_.size()) &&
               (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.'))
            ++i_;
        if (i_ == static_cast<int>(start))
            throw std::runtime_error("expected a number in: " + s_);
        // an exponent only if it is followed by digits
        if (i_ < static_cast<int>(s_.size()) && (s_[i_] == 'e' || s_[i_] == 'E')) {
            const int save = i_;
            ++i_;
            if (i_ < static_cast<int>(s_.size()) && (s_[i_] == '+' || s_[i_] == '-'))
                ++i_;
            if (i_ < static_cast<int>(s_.size()) && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
                while (i_ < static_cast<int>(s_.size()) && std::isdigit(static_cast<unsigned char>(s_[i_])))
                    ++i_;
            } else {
                i_ = save;
            }
        }
        double v = std::stod(s_.substr(start, static_cast<size_t>(i_) - start));
        // suffix: letters after the number, first match wins (MEG before M)
        std::string suf;
        while (i_ < static_cast<int>(s_.size()) && std::isalpha(static_cast<unsigned char>(s_[i_])))
            suf += static_cast<char>(std::toupper(static_cast<unsigned char>(s_[i_++])));
        if (!suf.empty())
            for (const auto& [name, mul] : spiceSuffixes())
                if (suf.compare(0, std::strlen(name), name) == 0) {
                    v *= mul;
                    break;
                }
        return v;
    }

    const std::string& s_;
    int i_ = 0;
};

inline double parseExpr(const std::string& tok) {
    std::string body = tok;
    // strip surrounding quotes and whitespace
    auto trim = [](std::string& x) {
        const char* ws = " \t\r\n'\"";
        const size_t b = x.find_first_not_of(ws);
        const size_t e = x.find_last_not_of(ws);
        x = (b == std::string::npos) ? std::string() : x.substr(b, e - b + 1);
    };
    trim(body);
    return ExprParser(body).parse();
}

/// A component value; a Qucs-S pot line arrives as `R='(250K)*(80)/(100.0)'`, so
/// a leading `word=` is dropped before parsing.
inline double componentValue(const std::string& tok) {
    static const std::regex lead("^[A-Za-z]\\w*=");
    std::smatch m;
    if (std::regex_search(tok, m, lead))
        return parseExpr(tok.substr(static_cast<size_t>(m.length())));
    return parseExpr(tok);
}

// --------------------------------------------------------------------------- //
// Netlist lines
// --------------------------------------------------------------------------- //

struct Subckt {
    std::string name;
    std::vector<std::string> pins;
    std::vector<std::string> lines;
    std::map<std::string, double> params;
};

inline std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string t;
    while (is >> t)
        out.push_back(t);
    return out;
}

inline std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline void parseLinesFromStream(std::istream& in, std::map<std::string, Subckt>& subckts,
                                 std::vector<std::string>& top) {
    Subckt* cur = nullptr;
    bool inControl = false;
    std::string raw;
    while (std::getline(in, raw)) {
        // `$` starts a comment; `;` and `*` start a whole-line comment
        std::string line = raw.substr(0, raw.find('$'));
        const size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            continue;
        line = line.substr(b, line.find_last_not_of(" \t\r\n") - b + 1);
        if (line.empty() || line[0] == '*' || line[0] == ';')
            continue;
        const std::string low = lower(line);
        if (low.rfind(".control", 0) == 0) {
            inControl = true;
            continue;
        }
        if (low.rfind(".endc", 0) == 0) {
            inControl = false;
            continue;
        }
        if (inControl)
            continue;
        if (low.rfind(".subckt", 0) == 0) {
            const auto parts = splitWs(line);
            Subckt s;
            s.name = parts.size() > 1 ? parts[1] : "";
            for (size_t k = 2; k < parts.size(); ++k)
                s.pins.push_back(parts[k]);
            subckts[lower(s.name)] = s;
            cur = &subckts[lower(s.name)];
            continue;
        }
        if (low.rfind(".ends", 0) == 0) {
            cur = nullptr;
            continue;
        }
        if (low.rfind(".param", 0) == 0) {
            const std::string body = line.substr(6);
            static const std::regex kv("([A-Za-z_]\\w*)\\s*=\\s*(\\S+)");
            for (auto it = std::sregex_iterator(body.begin(), body.end(), kv);
                 it != std::sregex_iterator(); ++it) {
                if (cur)
                    cur->params[lower((*it)[1].str())] = parseExpr((*it)[2].str());
            }
            continue;
        }
        // everything else (including `.model`) belongs to the current subckt or the top level
        (cur ? cur->lines : top).push_back(line);
    }
}

inline void parseLines(const std::string& path, std::map<std::string, Subckt>& subckts,
                       std::vector<std::string>& top) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    parseLinesFromStream(in, subckts, top);
}

/// Build the triode model for a parsed `12AX7_model` subckt. The subckt's
/// Koren `.param`s are deliberately ignored: the engine has one triode law, the
/// D'Angelo quadric, and the subckt is used only for its passive grid network
/// (`rgi`/`vct`) and inter-electrode capacitances. `sub` is accepted so the call
/// site states what the model belongs to; the law itself has no per-instance
/// data to read from it.
inline TriodeModel modelFromSubckt(const Subckt&) {
    return k12ax7Quadric();
}

/// One `X` line -> the pieces the TriodeModel does not own.
inline void addTube(Circuit& c, const std::string& name, const std::vector<std::string>& pins,
                    const TriodeModel& model, const std::map<std::string, double>& prm) {
    if (pins.size() < 4)
        throw std::runtime_error("malformed tube instance: " + name);
    const std::string& cathode = pins[1];
    const std::string& grid = pins[2];
    const std::string& plate = pins[3];
    const std::string gi = name + "$gi";   // internal grid, behind rgi + vct
    const std::string vt = name + "$vct";  // the node vct sits on
    auto p = [&](const char* k, double dflt) {
        const auto it = prm.find(k);
        return it == prm.end() ? dflt : it->second;
    };
    // v(vt) - v(grid) = vct, written with the *physical* grid node first so the
    // union-find keeps it as the representative of that equipotential set. Done
    // the natural way round, the synthetic `$vct` node wins and the real grid node
    // (e.g. vout_lead_2cp) disappears from Compiled::nodes.
    c.addSrc(grid, vt, -p("vct", 0.0));
    c.addRes(gi, vt, p("rgi", 2000.0), name + "$rgi");
    c.addRes(plate, cathode, 1e9, name + "$rcp");  // the 1G leak
    c.addCap(gi, cathode, p("ccg", 2.3e-12), name + "$ccg");
    c.addCap(plate, gi, p("cgp", 2.4e-12), name + "$cgp");
    c.addCap(plate, cathode, p("ccp", 0.9e-12), name + "$ccp");
    c.addTri(grid, plate, cathode, gi, model);
}

/// Parse `path` into a `Circuit`.
inline Circuit buildFromParsed(const std::map<std::string, Subckt>& subckts,
                               const std::vector<std::string>& top,
                               const std::string& sourceName) {
    // Tube model extraction is lazy: it is only required when the netlist
    // actually instantiates an X tube line (LiveWDF phase-1 change: passive-only
    // netlists must parse without the Mesa tube subckts present).
    std::optional<TriodeModel> model;
    std::map<std::string, double> prm;
    if (subckts.find("tubes_12ax7") != subckts.end()) {
        const auto innerIt = subckts.find("12ax7_model");
        if (innerIt == subckts.end())
            throw std::runtime_error("no 12AX7_model subckt in " + sourceName);
        const Subckt& inner = innerIt->second;
        model = modelFromSubckt(inner);
        for (const char* k : {"rgi", "vct", "ccg", "cgp", "ccp"}) {
            const auto it = inner.params.find(k);
            if (it != inner.params.end())
                prm[k] = it->second;
        }
    }

    // Shared DC/AC/SIN parsing for independent sources (`V...` and `I...`).
    auto parseSource = [&](const std::vector<std::string>& rest) {
        std::vector<std::string> up;
        for (const auto& t : rest)
            up.push_back(lower(t));
        auto findKw = [&](const char* kw) -> int {
            for (size_t i = 0; i < up.size(); ++i)
                if (up[i] == kw)
                    return static_cast<int>(i);
            return -1;
        };
        double dc = 0.0, ac = 0.0;
        bool haveDc = false;
        if (const int i = findKw("dc"); i >= 0 && i + 1 < static_cast<int>(rest.size())) {
            dc = parseExpr(rest[i + 1]);
            haveDc = true;
        }
        if (const int i = findKw("ac"); i >= 0 && i + 1 < static_cast<int>(rest.size()))
            ac = parseExpr(rest[i + 1]);
        std::vector<double> sin;
        std::string joined;
        for (const auto& t : rest)
            joined += t + " ";
        static const std::regex reSin(R"(SIN\s*\(([^)]*)\))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(joined, m, reSin)) {
            std::string body = m[1].str();
            for (char& ch : body)
                if (ch == ',')
                    ch = ' ';
            for (const auto& t : splitWs(body))
                sin.push_back(parseExpr(t));
        }
        if (!haveDc) {
            // SPICE shorthand `V1 a b 5` / `I1 a b 1m`: the first bare numeric
            // token is the DC value. Tokens that begin a waveform or an AC
            // spec end the search.
            for (const auto& t : rest) {
                const std::string lt = lower(t);
                if (lt == "dc" || lt == "ac" || lt.rfind("sin", 0) == 0 || lt.rfind("pulse", 0) == 0 ||
                    lt.rfind("exp", 0) == 0 || lt.rfind("pwl", 0) == 0 || lt.rfind("sffm", 0) == 0)
                    break;
                try {
                    dc = parseExpr(t);
                    haveDc = true;
                    break;
                } catch (const std::exception&) {
                    break;
                }
            }
        }
        // A SIN source with no explicit DC value biases at its offset (SPICE's
        // operating point uses SIN's VO), which also seeds the reactances.
        if (!haveDc && !sin.empty())
            dc = sin.front();
        return std::make_tuple(dc, ac, sin);
    };

    // Diode models live in top-level `.model NAME D (...)` lines. They are
    // collected before the element pass so that a `D` line can resolve its name,
    // but they are *not* removed: the dispatch already reads past every `.` line,
    // and resolution stays lazy. That distinction matters -- a `.model` line
    // nothing references is still read past exactly as it was before this
    // element existed, while a diode that actually depends on an unsupported
    // parameter fails loudly instead of being simulated without it.
    std::map<std::string, std::string> diodeModelLines;
    static const std::regex reDiodeModel(R"(^\.model\s+(\S+)\s+d\s*(?:\((.*)\))?\s*$)",
                                         std::regex::icase);
    for (const auto& line : top) {
        std::smatch m;
        if (std::regex_match(line, m, reDiodeModel))
            diodeModelLines[lower(m[1].str())] = m[2].matched ? m[2].str() : std::string{};
    }

    // N-channel JFET cards. The subset is exactly the part of an LTspice
    // `standard.jft` card this model reads. Junction, temperature, noise and
    // gate-current parameters are rejected rather than silently dropped: they
    // change the device in ways this simplified law does not reproduce.
    std::map<std::string, std::string> jfetModelLines;
    static const std::regex reJfetModel(R"(^\.model\s+(\S+)\s+njf\s*(?:\((.*)\))?\s*$)",
                                        std::regex::icase);
    for (const auto& line : top) {
        std::smatch m;
        if (std::regex_match(line, m, reJfetModel))
            jfetModelLines[lower(m[1].str())] = m[2].matched ? m[2].str() : std::string{};
    }

    // N-channel MOSFET cards. This is a deliberately small Level-1 subset:
    // KP, VTO, LAMBDA, W and L. There are no built-in MOSFET presets, so an
    // unresolved model name is an error rather than a fallback device.
    std::map<std::string, std::string> mosfetModelLines;
    static const std::regex reMosfetModel(R"(^\.model\s+(\S+)\s+nmos\s*(?:\((.*)\))?\s*$)",
                                          std::regex::icase);
    for (const auto& line : top) {
        std::smatch m;
        if (std::regex_match(line, m, reMosfetModel))
            mosfetModelLines[lower(m[1].str())] = m[2].matched ? m[2].str() : std::string{};
    }

    /// The `D` element's model, built from its own `.model` line. SPICE's defaults
    /// apply when a parameter is absent (`IS=1e-14`, `N=1`, `RS=0`), but a
    /// parameter the engine does not model is an error, not something to drop:
    ///
    ///   * `IS`, `N`, `RS`, `RP` are the whole extended junction law the engine
    ///     solves; `RP` is the Werner et al parallel resistor and defaults to
    ///     1e6. `RP=0` asks for the plain SPICE junction law;
    ///   * everything else (`CJO`, `TT`, `VJ`, `M`, ...) changes the circuit in
    ///     ways this engine cannot reproduce, so honouring the line by silently
    ///     ignoring them would be the increment-8 bug wearing a nicer name.
    ///
    /// `RS=0` is the common case (`diode_clipper`'s `D1N914` is `IS=1n N=1`) and
    /// is supported: with no series drop the law is explicit in the junction
    /// voltage, and `DiodeModel` evaluates it directly.
    const auto diodeModel = [&](const std::string& name) -> DiodeModel {
        const auto found = diodeModelLines.find(lower(name));
        if (found == diodeModelLines.end()) {
            if (const auto* entry = findDiodeByName(name))
                return DiodeModel(entry->isat, entry->n, entry->rs, entry->rp);
            throw std::runtime_error("unknown diode model '" + name + "' in " + sourceName);
        }
        static const std::regex reKv(R"((\w+)\s*=\s*([^\s,]+))");
        std::map<std::string, std::string> kv;
        for (auto it = std::sregex_iterator(found->second.begin(), found->second.end(), reKv);
             it != std::sregex_iterator(); ++it)
            kv[lower((*it)[1].str())] = (*it)[2].str();
        for (const auto& entry : kv)
            if (entry.first != "is" && entry.first != "n" && entry.first != "rs" && entry.first != "rp")
                throw std::runtime_error("diode model '" + name + "' sets '" + entry.first +
                                         "', which this engine does not model; refusing rather "
                                         "than simulating without it");
        const auto num = [&](const char* key, double dflt) {
            const auto it = kv.find(key);
            return it == kv.end() ? dflt : parseExpr(it->second);
        };
        const double isat = num("is", 1e-14);
        const double emission = num("n", 1.0);
        const double series = num("rs", 0.0);
        const double parallel = num("rp", 1e6);  // the paper's extension, carried
        if (!(isat > 0.0) || !(emission > 0.0) || !(series >= 0.0) || !(parallel >= 0.0))
            throw std::runtime_error("diode model '" + name + "' in " + sourceName +
                                     " has a non-physical parameter (need IS>0, N>0, RS>=0, RP>=0)");
        return DiodeModel(isat, emission, series, parallel);
    };

    /// The `J` element's model. A `.model NAME NJF(...)` card overrides the
    /// built-in preset of the same name. Supported keys are the ones this
    /// implementation actually reads:
    ///
    ///   Beta, Vto, Lambda, Rd, Rs, Cgs, Cgd, Cds
    ///
    /// `Beta` and `Vto` are required in a card; `Lambda` and the linear
    /// parasitics default to zero. `Is0 = Beta*Vto^2` is derived here, not
    /// accepted as a card parameter. `Rgate` is an engine-level convenience and
    /// is not a SPICE JFET parameter, so it is not accepted from a card.
    const auto jfetModel = [&](const std::string& name) -> JfetModel {
        const auto found = jfetModelLines.find(lower(name));
        if (found == jfetModelLines.end()) {
            if (const auto* entry = findJfetByName(name))
                return entry->model;
            throw std::runtime_error("unknown JFET model '" + name + "' in " + sourceName);
        }
        static const std::regex reKv(R"((\w+)\s*=\s*([^\s,]+))");
        std::map<std::string, std::string> kv;
        for (auto it = std::sregex_iterator(found->second.begin(), found->second.end(), reKv);
             it != std::sregex_iterator(); ++it)
            kv[lower((*it)[1].str())] = (*it)[2].str();
        for (const auto& entry : kv)
            if (entry.first != "beta" && entry.first != "vto" && entry.first != "lambda" &&
                entry.first != "rd" && entry.first != "rs" && entry.first != "cgs" &&
                entry.first != "cgd" && entry.first != "cds")
                throw std::runtime_error("JFET model '" + name + "' sets '" + entry.first +
                                         "', which this engine does not model; refusing rather "
                                         "than simulating without it");
        const auto num = [&](const char* key, bool required, double dflt) {
            const auto it = kv.find(key);
            if (it == kv.end()) {
                if (required)
                    throw std::runtime_error("JFET model '" + name + "' in " + sourceName +
                                             " is missing required parameter '" + key + "'");
                return dflt;
            }
            return parseExpr(it->second);
        };
        const double beta = num("beta", true, 0.0);
        const double vto = num("vto", true, 0.0);
        const double lambda = num("lambda", false, 0.0);
        const double rd = num("rd", false, 0.0);
        const double rs = num("rs", false, 0.0);
        const double cgs = num("cgs", false, 0.0);
        const double cgd = num("cgd", false, 0.0);
        const double cds = num("cds", false, 0.0);
        if (!(beta > 0.0) || !(vto < 0.0) || !(lambda >= 0.0) || !(rd >= 0.0) ||
            !(rs >= 0.0) || !(cgs >= 0.0) || !(cgd >= 0.0) || !(cds >= 0.0))
            throw std::runtime_error("JFET model '" + name + "' in " + sourceName +
                                     " has a non-physical parameter (need Beta>0, Vto<0, "
                                     "Lambda>=0, Rd/Rs/Cgs/Cgd/Cds>=0)");
        return JfetModel::fromSpice(beta, vto, lambda, rd, rs, cgs, cgd, cds);
    };

    /// The `M` element's Level-1 model. A `.model NAME NMOS(...)` card is the
    /// only source of MOSFET parameters; there are no built-in presets.
    ///
    /// Supported keys are `LEVEL` (must be 1 when present), `KP`, `VTO`,
    /// `LAMBDA`, `W` and `L`. `KP` and `VTO` are required. W and L default to
    /// 1 metre only when neither the card nor the instance supplies them; this
    /// avoids silently adopting an unstated SPICE default and keeps the
    /// paper's `K = KP*W/L` mapping explicit. `W=`/`L=` on the `M` line
    /// override the card values. The body terminal must be tied to the source
    /// because the paper's law has no body effect.
    const auto mosfetModel = [&](const std::string& name, bool hasW, double w,
                                 bool hasL, double l) -> MosfetModel {
        const auto found = mosfetModelLines.find(lower(name));
        if (found == mosfetModelLines.end())
            throw std::runtime_error("unknown NMOS model '" + name + "' in " + sourceName);
        static const std::regex reKv(R"((\w+)\s*=\s*([^\s,]+))");
        std::map<std::string, std::string> kv;
        for (auto it = std::sregex_iterator(found->second.begin(), found->second.end(), reKv);
             it != std::sregex_iterator(); ++it)
            kv[lower((*it)[1].str())] = (*it)[2].str();
        for (const auto& entry : kv)
            if (entry.first != "level" && entry.first != "kp" && entry.first != "vto" &&
                entry.first != "lambda" && entry.first != "w" && entry.first != "l")
                throw std::runtime_error("NMOS model '" + name + "' sets '" + entry.first +
                                         "', which this engine does not model; refusing rather "
                                         "than simulating without it");
        const auto num = [&](const char* key, bool required, double dflt) {
            const auto it = kv.find(key);
            if (it == kv.end()) {
                if (required)
                    throw std::runtime_error("NMOS model '" + name + "' in " + sourceName +
                                             " is missing required parameter '" + key + "'");
                return dflt;
            }
            return parseExpr(it->second);
        };
        const double level = num("level", false, 1.0);
        if (level != 1.0)
            throw std::runtime_error("NMOS model '" + name + "' in " + sourceName +
                                     " uses LEVEL=" + std::to_string(level) +
                                     "; this engine implements Level 1 only");
        const double kp = num("kp", true, 0.0);
        const double vto = num("vto", true, 0.0);
        const double lambda = num("lambda", false, 0.0);
        const double cardW = num("w", false, 1.0);
        const double cardL = num("l", false, 1.0);
        const double totalW = hasW ? w : cardW;
        const double totalL = hasL ? l : cardL;
        if (!(kp > 0.0) || !(lambda >= 0.0) || !(totalW > 0.0) || !(totalL > 0.0) ||
            !std::isfinite(kp) || !std::isfinite(vto) || !std::isfinite(lambda) ||
            !std::isfinite(totalW) || !std::isfinite(totalL))
            throw std::runtime_error("NMOS model '" + name + "' in " + sourceName +
                                     " has a non-physical parameter (need KP>0, W>0, L>0, "
                                     "Lambda>=0)");
        return MosfetModel::fromSpice(kp, vto, lambda, totalW, totalL);
    };

    Circuit c;
    for (const auto& line : top) {
        const auto toks = splitWs(line);
        if (toks.empty())
            continue;
        const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(toks[0][0])));
        // A directive is not an element: `.param` / `.subckt` / `.control` were
        // consumed above, and `.model` / `.tran` / `.end` are statements a
        // `Circuit` does not need. This is the one deliberate read-past, and it
        // happens before the token-count check because directives carry their own
        // syntax (`.end` is a single token).
        if (kind == '.')
            continue;
        if (toks.size() < 2)  // an element line with no second terminal
            throw std::runtime_error(std::string("malformed '") + kind + "' element in " +
                                     sourceName + ": " + line);
        if (lower(toks[0]).rfind("xp", 0) == 0) {
            // Pentode: XP<name> <plate> <screen> <grid> <cathode> <model>
            if (toks.size() < 6)
                throw std::runtime_error(std::string("malformed 'XP' element in ") +
                                         sourceName + ": " + line);
            const auto* pentModel = findPentodeByName(toks[5]);
            if (!pentModel)
                throw std::runtime_error("unknown pentode model '" + toks[5] + "' in " +
                                         sourceName + ": " + line);
            c.addPentode(toks[1], toks[2], toks[3], toks[4], *pentModel, toks[0]);
        } else if (kind == 'X') {
            if (const auto* pentModel = findPentodeByName(toks.back())) {
                if (toks.size() < 6)
                    throw std::runtime_error(std::string("malformed pentode instance in ") +
                                             sourceName + ": " + line);
                c.addPentode(toks[1], toks[2], toks[3], toks[4], *pentModel, toks[0]);
            } else if (lower(toks.back()) == "tubes_12ax7") {
                if (!model)
                    throw std::runtime_error("no Tubes_12AX7 subckt in " + sourceName);
                std::vector<std::string> pins(toks.begin() + 1, toks.end() - 1);
                addTube(c, toks[0], pins, *model, prm);
            } else {
                throw std::runtime_error("unsupported subcircuit in " + sourceName +
                                         " (only Tubes_12AX7 or pentode models are elaborated): " + line);
            }
        } else if (kind == 'R' && toks.size() > 3) {
            c.addRes(toks[1], toks[2], componentValue(toks[3]), toks[0]);
        } else if (lower(toks[0]).rfind("cr", 0) == 0) {
            // Controlled resistor: CR<name> a b [initialOhms]
            if (toks.size() < 3)
                throw std::runtime_error(std::string("malformed 'CR' element in ") +
                                         sourceName + ": " + line);
            const double initialR = toks.size() > 3 ? componentValue(toks[3]) : 1e3;
            c.addControlledRes(toks[1], toks[2], initialR, toks[0]);
        } else if (kind == 'C' && toks.size() > 3) {
            c.addCap(toks[1], toks[2], componentValue(toks[3]), toks[0]);
        } else if (kind == 'L' && toks.size() > 3) {
            c.addInd(toks[1], toks[2], componentValue(toks[3]), toks[0]);
        } else if (lower(toks[0]).rfind("vr", 0) == 0) {
            // Variable resistor (rheostat): VR<name> a b maxResistance [pos=0.5] [LIN|LOG|REVLOG]
            if (toks.size() < 4)
                throw std::runtime_error(std::string("malformed 'VR' element in ") +
                                         sourceName + ": " + line);
            const double maxR = componentValue(toks[3]);
            double pos = 0.5;
            PotTaper taper = PotTaper::Linear;
            for (size_t t = 4; t < toks.size(); ++t) {
                std::string lowTok = lower(toks[t]);
                if (lowTok.rfind("pos=", 0) == 0) {
                    pos = parseExpr(toks[t].substr(4));
                } else if (lowTok == "lin" || lowTok == "linear") {
                    taper = PotTaper::Linear;
                } else if (lowTok == "log" || lowTok == "audio") {
                    taper = PotTaper::Log;
                } else if (lowTok == "revlog" || lowTok == "reverse_log") {
                    taper = PotTaper::RevLog;
                }
            }
            c.addVarRes(toks[1], toks[2], maxR, pos, taper, toks[0]);
        } else if (kind == 'V' && toks.size() > 2) {
            auto [dc, ac, sin] = parseSource(std::vector<std::string>(toks.begin() + 3, toks.end()));
            c.addSrc(toks[1], toks[2], dc, ac, std::move(sin), toks[0]);
        } else if (kind == 'I' && toks.size() > 2) {
            auto [dc, ac, sin] = parseSource(std::vector<std::string>(toks.begin() + 3, toks.end()));
            c.addIsrc(toks[1], toks[2], dc, ac, std::move(sin), toks[0]);
        } else if (kind == 'D' && toks.size() > 3) {
            c.addDiode(toks[1], toks[2], diodeModel(toks[3]), toks[0]);
        } else if (kind == 'J' && toks.size() > 4) {
            // J<name> <drain> <gate> <source> <model>
            c.addJfet(toks[1], toks[2], toks[3], jfetModel(toks[4]), toks[0]);
        } else if (kind == 'M' && toks.size() >= 6) {
            // M<name> <drain> <gate> <source> <bulk> <model> [W=...] [L=...].
            // The bulk terminal is accepted only when it is the source; the
            // paper's model has no body effect and silently ignoring a
            // different bulk node would simulate a different device.
            if (lower(toks[4]) != lower(toks[3]))
                throw std::runtime_error("MOSFET '" + toks[0] + "' in " + sourceName +
                                         " has bulk node '" + toks[4] +
                                         "' different from its source; this model has no "
                                         "body effect, so tie bulk to source");
            bool hasW = false, hasL = false;
            double w = 1.0, l = 1.0;
            for (size_t t = 6; t < toks.size(); ++t) {
                const std::string lowTok = lower(toks[t]);
                if (lowTok.rfind("w=", 0) == 0) {
                    w = parseExpr(toks[t].substr(2));
                    hasW = true;
                } else if (lowTok.rfind("l=", 0) == 0) {
                    l = parseExpr(toks[t].substr(2));
                    hasL = true;
                } else {
                    throw std::runtime_error("MOSFET '" + toks[0] + "' in " + sourceName +
                                             " has unsupported instance parameter '" + toks[t] +
                                             "'");
                }
            }
            c.addMosfet(toks[1], toks[2], toks[3], mosfetModel(toks[5], hasW, w, hasL, l),
                        toks[0]);
        } else if (kind == 'E' && toks.size() >= 6) {
            // Linear gain block / VCVS: E<name> <out+> <out-> <in+> <in-> <gain> [ro]
            const double gain = componentValue(toks[5]);
            const double ro = toks.size() > 6 ? componentValue(toks[6]) : 1e-4;
            c.addVCVS(toks[1], toks[2], toks[3], toks[4], gain, ro, toks[0]);
        } else if (kind == 'P' && toks.size() > 4) {
            // Potentiometer: P1 n1 wiper n2 totalResistance [pos=0.5] [LIN|LOG|REVLOG]
            const double totalR = componentValue(toks[4]);
            double pos = 0.5;
            PotTaper taper = PotTaper::Linear;
            for (size_t t = 5; t < toks.size(); ++t) {
                std::string lowTok = lower(toks[t]);
                if (lowTok.rfind("pos=", 0) == 0) {
                    pos = parseExpr(toks[t].substr(4));
                } else if (lowTok == "lin" || lowTok == "linear") {
                    taper = PotTaper::Linear;
                } else if (lowTok == "log" || lowTok == "audio") {
                    taper = PotTaper::Log;
                } else if (lowTok == "revlog" || lowTok == "reverse_log") {
                    taper = PotTaper::RevLog;
                }
            }
            c.addPot(toks[1], toks[2], toks[3], totalR, pos, taper, toks[0]);
        } else if (kind == 'K' && toks.size() >= 4) {
            // Magnetically coupled inductors / transformer: K<name> <L1> <L2> ... <coupling>
            const double kVal = parseExpr(toks.back());
            if (std::abs(kVal) >= 1.0)
                throw std::runtime_error("coupling coefficient |k| must be strictly less than 1.0 in " +
                                         sourceName + ": " + line);
            std::vector<std::string> indNames(toks.begin() + 1, toks.end() - 1);
            c.addMutualInd(toks[0], indNames, kVal);
        } else if (std::strchr("RCLVIDPKEJM", kind) == nullptr) {
            // The whole point of the check (PLAN section 6, item #0): this used to
            // fall off the end and drop the line, and the netlist then compiled to a
            // circuit that had never been written down.
            throw std::runtime_error(std::string("unsupported element '") + kind + "' in " +
                                     sourceName + " (the parser understands R C L V I X D J M P K E VR CR XP): " +
                                     line);
        } else {
            // The other way a line used to vanish: a letter this parser knows, with
            // the shape of an element it cannot build (`R1 a b`, no value).
            throw std::runtime_error(std::string("malformed '") + kind + "' element in " +
                                     sourceName + ": " + line);
        }
    }
    return c;
}

inline Circuit build(const std::string& path) {
    std::map<std::string, Subckt> subckts;
    std::vector<std::string> top;
    parseLines(path, subckts, top);
    return buildFromParsed(subckts, top, path);
}

inline Circuit buildFromString(const std::string& content) {
    std::istringstream in(content);
    std::map<std::string, Subckt> subckts;
    std::vector<std::string> top;
    parseLinesFromStream(in, subckts, top);
    return buildFromParsed(subckts, top, "<string>");
}

}  // namespace wdf
