#include "format/ValueFormat.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace livewdf::editor {

namespace {

/// `text` with its trailing zeros (and a bare trailing '.') trimmed: "4.70" ->
/// "4.7", "1.00" -> "1", "1." -> "1". (Celine trims this inline in three
/// places; it is one loop, so it lives once.)
std::string trimTrailingZeros(std::string text) {
    if (text.find('.') == std::string::npos)
        return text;

    while (!text.empty() && text.back() == '0')
        text.pop_back();

    if (!text.empty() && text.back() == '.')
        text.pop_back();

    return text;
}

/// A fixed-point string with at most `places` decimals, trailing zeros trimmed.
std::string fixed(double value, int places) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", places, value);
    return trimTrailingZeros(buffer);
}

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string uppered(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size()
           && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Every character of `s` is in `allowed`.
bool containsOnly(const std::string& s, const std::string& allowed) {
    if (s.empty())
        return false;
    for (char c : s)
        if (allowed.find(c) == std::string::npos)
            return false;
    return true;
}

/// `std::strtod`, with the end pointer checked so a trailing character that the
/// caller's allow-list missed fails rather than silently truncating. JUCE's
/// `getDoubleValue` is what Celine uses; this is the same "parse the front,
/// refuse the rest" contract without the JUCE type.
double parseDouble(const std::string& text, bool& ok) {
    ok = false;

    const char* begin = text.c_str();
    char* end = nullptr;
    const double value = std::strtod(begin, &end);

    if (end == begin || *end != '\0')
        return 0.0;

    ok = true;
    return value;
}

/// The marking a potentiometer wears, from the taper name our document stores.
///
/// **Adapted, not ported.** Celine keys this off a `Taper` enum and saves the
/// enum's integer; our `.livewdf` saves the *name* (`properties["taper"]` is
/// "Linear"/"Log"/"RevLog" -- see `LiveWDFFile.h`), which is also what the engine
/// reads back into `PotTaper`. So the table is keyed on those three names and
/// returns the same trade markings Celine does: B linear, A audio (log), C
/// reverse audio. Keeping a Celine taper enum here would be a second source of
/// truth for something the file already carries.
///
/// This is how a real part is sold -- an "A500K" is the log pot in a Les Paul --
/// which is why it is written down rather than inferred.
std::string taperCode(const std::string& taper) {
    const std::string name = lowered(trimmed(taper));

    if (name == "log" || name == "logarithmic" || name == "audio")
        return "A";
    if (name == "revlog" || name == "reverse logarithmic" || name == "reverse log")
        return "C";

    // Linear, and anything unrecognised: the plain part.
    return "B";
}

/// The inverse: the taper name our document stores, from a marking code.
/// `false` when the code names no taper we have.
bool taperFromCode(const std::string& code, std::string& nameOut) {
    const std::string c = uppered(trimmed(code));

    if (c == "A") {
        nameOut = "Log";
        return true;
    }
    if (c == "C") {
        nameOut = "RevLog";
        return true;
    }
    if (c == "B") {
        nameOut = "Linear";
        return true;
    }

    return false;
}

}  // namespace

std::string formatValue(double value, const std::string& unit) {
    if (value == 0.0)
        return "0" + unit;

    struct Prefix {
        double scale;
        const char* suffix;
    };

    // Engineering notation, the way a schematic writes it. (Celine's table,
    // verbatim, so a value formats the same here as there.)
    static const Prefix prefixes[] = {
        {1.0e12, "T"}, {1.0e9, "G"}, {1.0e6, "M"}, {1.0e3, "k"}, {1.0, ""},
        {1.0e-3, "m"}, {1.0e-6, "u"}, {1.0e-9, "n"}, {1.0e-12, "p"},
    };

    const double magnitude = std::abs(value);

    for (const auto& prefix : prefixes) {
        if (magnitude >= prefix.scale * 0.999) {
            const double scaled = value / prefix.scale;

            // Two significant decimals at most, and no trailing zeros:
            // "4.7k", not "4.70k".
            return fixed(scaled, scaled < 10.0 ? 2 : 1) + prefix.suffix + unit;
        }
    }

    // Below 1p. Celine falls through to JUCE's default double formatting, which
    // is exponent-but-plain; we say it plainly with enough places that a real
    // value is not rounded away.
    return fixed(value, 15) + unit;
}

std::string formatRatio(double primary, double secondary) {
    // Plainly, with no engineering prefixes: a turns ratio is dimensionless, so
    // "100m:1" is nonsense where "1:10" is what anyone would write.
    auto number = [](double v) { return fixed(v, 3); };

    return number(primary) + ":" + number(secondary);
}

std::string formatPotValue(double value, const std::string& taper) {
    // Nothing to mark if there is no value yet. The bare "0" keeps the
    // unset-value colouring meaning what it means everywhere else, and a "B0"
    // reads like a part number rather than like something missing.
    if (value <= 0.0)
        return "0";

    const std::string code = taperCode(taper);

    // A space only for the numbered tapers. "A250K" is how the part is marked and
    // stays exactly that; "10A250K" is not a marking of anything, and run
    // together it reads as one long number.
    const std::string separator = code.size() > 1 ? " " : "";

    // Uppercase K: a pot is marked "250K", not "250k". (Celine's
    // `replaceCharacter('k', 'K')`; upper-casing the whole string is the same
    // thing for a value that is only digits, a K and maybe a decimal point.)
    return code + separator + uppered(formatValue(value, ""));
}

bool parsePotValue(const std::string& text, double& value, std::string& taper,
                   bool& taperGiven) {
    std::string body = trimmed(text);
    taperGiven = false;

    // The code is an optional run of digits followed by one letter -- "A", "C",
    // "10A". Peeled only when something follows it, so "A" on its own stays what
    // it always was: not a value, and refused.
    //
    // Taking the digits first is what keeps "10A250K" from being read as the
    // number 10: a leading number is part of the code only when a taper letter
    // closes it, and otherwise the whole string goes to `parseValue` untouched.
    // That is also why "M2" still parses as 2 megohms -- M names no taper, so
    // nothing is peeled.
    size_t split = 0;
    while (split < body.size() && std::isdigit(static_cast<unsigned char>(body[split])))
        ++split;

    // `[digits][letter]` with at least one character after it. (Celine's exact
    // condition: `split < length - 1` -- so "A" alone is not peeled, because
    // there is no value behind the code.)
    if (split < body.size() && split + 1 < body.size()) {
        const std::string code = body.substr(0, split + 1);
        std::string name;
        if (taperFromCode(code, name)) {
            taperGiven = true;
            taper = name;
            body = trimmed(body.substr(split + 1));
        }
    }

    bool ok = false;
    value = parseValue(body, &ok);
    return ok;
}

double parseValue(const std::string& text, bool* ok) {
    if (ok) *ok = false;

    std::string source = trimmed(text);
    if (source.empty())
        return 0.0;

    // Upper-case M is mega, lower-case m is milli, and the distinction has to
    // survive the case fold below -- so it is spelled out first.
    //
    // This is not a nicety. `formatValue` *writes* an upper-case M for mega, so
    // without this a 2.2M resistor read straight back off its own editor parsed
    // as 2.2 milliohms: opening the inspector on a part and pressing return,
    // changing nothing, turned a 2M2 grid leak into a short. "meg" keeps meaning
    // mega whatever case it arrives in, since that is what a netlist writes.
    //
    // Only the *first* M, as Celine does (`replace("M", "meg", false)`), so a
    // value that arrives already spelling out "meg" is left alone.
    if (lowered(source).find("meg") == std::string::npos) {
        const size_t at = source.find('M');
        if (at != std::string::npos)
            source.replace(at, 1, "meg");
    }

    std::string lower = lowered(source);

    if (lower.empty())
        return 0.0;

    // Drop a trailing unit letter so "10k ohm", "100nF", "9V" and "40ms" all
    // work.
    //
    // "s" goes last, after "ohms", because the loop stops at the first match and
    // stripping the s off "10 ohms" would leave "10 ohm" for the suffix scan to
    // choke on. It has to be here at all because `formatValue` *writes* seconds
    // -- a scope's time span is displayed as "40ms", and without this that text
    // did not read back as anything.
    for (const char* unit : {"ohm", "ohms", ":1", "f", "h", "v", "s"}) {
        const std::string u(unit);
        if (endsWith(lower, u) && lower.size() > u.size()) {
            lower = trimmed(lower.substr(0, lower.size() - u.size()));
            break;
        }
    }

    struct Suffix {
        const char* letter;
        double scale;
    };

    static const Suffix suffixes[] = {
        {"t", 1.0e12}, {"g", 1.0e9}, {"meg", 1.0e6}, {"m", 1.0e-3}, {"k", 1.0e3},
        {"u", 1.0e-6}, {"n", 1.0e-9}, {"p", 1.0e-12},
    };

    // "meg" before "m", since SPICE means milli by "m" and mega by "meg" -- and
    // a capacitor typed as "10m" is far more likely to be millifarads.
    for (const auto& suffix : suffixes) {
        const std::string letter(suffix.letter);
        const size_t at = lower.find(letter);

        if (at == std::string::npos)
            continue;

        const std::string before = lower.substr(0, at);
        const std::string after = lower.substr(at + letter.size());

        if (before.empty() && after.empty())
            continue;

        // "4k7" means 4.7k -- the decimal point standing in for the suffix, which
        // is how values are written on a real schematic.
        const std::string digits = after.empty() ? before : before + "." + after;

        if (!containsOnly(digits, "0123456789.-+"))
            continue;

        bool parsed = false;
        const double number = parseDouble(digits, parsed);
        if (!parsed)
            continue;

        if (ok) *ok = true;
        return number * suffix.scale;
    }

    // No suffix: a plain number, optionally in exponent form.
    if (!containsOnly(lower, "0123456789.-+e"))
        return 0.0;

    bool parsed = false;
    const double number = parseDouble(lower, parsed);
    if (!parsed)
        return 0.0;

    if (ok) *ok = true;
    return number;
}

}  // namespace livewdf::editor
