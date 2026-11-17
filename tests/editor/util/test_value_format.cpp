// Tests for the engineering value formatter and parser.
//
// Why this exists: every bug this file can catch is *silent*. `formatValue`
// writes "2.2M" for 2.2 megohms; if `parseValue` reads that back as 2.2
// *milli*ohms, nothing crashes and nothing logs -- a resistor just becomes a
// short the next time anyone opens its editor and presses return. That is the
// exact bug Celine's `parseValue` comment says it paid for, so the round trip is
// pinned by assertion rather than trusted to the comment.
//
// The rules under test are all ported from Celine
// (`SymbolPainter::formatValue`/`parseValue`/`formatRatio`/`formatPotValue`/
// `parsePotValue`), with one adaptation: our pots store the taper as a *name*
// ("Linear"/"Log"/"RevLog", matching `LiveWDFFile.h`), not Celine's enum, so the
// pot helpers speak names and their codes are B/A/C.

#include <catch2.hpp>

#include "format/ValueFormat.h"

#include <cmath>
#include <string>

using namespace livewdf::editor;

namespace {

bool close(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps * std::max(1.0, std::abs(b));
}

/// Parse, requiring a number to have been found.
double mustParse(const std::string& text) {
    bool ok = false;
    const double v = parseValue(text, &ok);
    INFO("parsing \"" << text << "\"");
    REQUIRE(ok);
    return v;
}

}  // namespace

TEST_CASE("formatValue writes engineering notation", "[value]") {
    CHECK(formatValue(0.0, "") == "0");
    CHECK(formatValue(1.5, "") == "1.5");
    CHECK(formatValue(4700.0, "") == "4.7k");
    CHECK(formatValue(1000.0, "") == "1k");
    CHECK(formatValue(2200000.0, "") == "2.2M");
    CHECK(formatValue(1.0e9, "") == "1G");
    CHECK(formatValue(1.0e12, "") == "1T");
    CHECK(formatValue(1e-6, "") == "1u");
    CHECK(formatValue(100e-9, "") == "100n");
    CHECK(formatValue(1e-12, "") == "1p");

    // No trailing zeros, and at most two decimals when under 10 ("4.7k", not
    // "4.70k"), one when 10 or over.
    CHECK(formatValue(4700.0, "") == "4.7k");
    CHECK(formatValue(15000.0, "") == "15k");
    CHECK(formatValue(2200.0, "") == "2.2k");

    // The unit is a plain suffix with no space.
    CHECK(formatValue(4700.0, "ohm") == "4.7kohm");
    CHECK(formatValue(9.0, "V") == "9V");

    // Negatives keep their sign.
    CHECK(formatValue(-4700.0, "") == "-4.7k");
}

TEST_CASE("formatValue round-trips through parseValue", "[value]") {
    // The property that matters most: what the formatter writes, the parser must
    // read back as the same number. This is the "2.2M became a short" bug.
    const double values[] = {
        1.5, 4700.0, 1000.0, 2200000.0, 1.0e9, 1.0e12,
        1e-6, 100e-9, 1e-12, 0.022, 250000.0, 15.0,
    };

    for (const double v : values) {
        const std::string text = formatValue(v, "");
        INFO("round trip of " << v << " via \"" << text << "\"");
        REQUIRE(close(mustParse(text), v));
    }
}

TEST_CASE("Upper-case M is mega and lower-case m is milli", "[value]") {
    // The whole reason `parseValue` spells the M out before folding case.
    CHECK(close(mustParse("2.2M"), 2.2e6));
    CHECK(close(mustParse("2.2m"), 2.2e-3));

    // And "meg" means mega whatever case it arrives in.
    CHECK(close(mustParse("2meg"), 2.0e6));
    CHECK(close(mustParse("2MEG"), 2.0e6));
    CHECK(close(mustParse("2Meg"), 2.0e6));

    // The prefix form "M2" is *not* "2 megohms". The rule is `before + "." +
    // after` around the suffix, which exists for "4k7"; fed "M2" it gives ".2"
    // megohms. This is Celine's behaviour, ported as-is rather than quietly
    // "fixed" differently from the reference -- and it is harmless in practice,
    // because the form nobody writes by hand is the one that degenerates: a user
    // types "2M", which is the case above.
    CHECK(close(mustParse("M2"), 0.2e6));
}

TEST_CASE("parseValue accepts the ways a value is written", "[value]") {
    CHECK(close(mustParse("4k7"), 4700.0));       // suffix as decimal point
    CHECK(close(mustParse("10k ohm"), 10000.0));  // unit word
    CHECK(close(mustParse("100nF"), 100e-9));     // unit letter
    CHECK(close(mustParse("9V"), 9.0));
    CHECK(close(mustParse("40ms"), 40e-3));       // the "s" that has to be last
    CHECK(close(mustParse("1e3"), 1000.0));       // exponent form
    CHECK(close(mustParse("10"), 10.0));          // plain
    CHECK(close(mustParse(" 4.7k "), 4700.0));    // surrounding space
    CHECK(close(mustParse("10 ohms"), 10.0));     // "ohms" before the bare "s"
}

TEST_CASE("parseValue reports failure rather than inventing a number", "[value]") {
    bool ok = true;
    parseValue("nonsense", &ok);
    CHECK_FALSE(ok);

    parseValue("", &ok);
    CHECK_FALSE(ok);

    // A trailing character the suffix scan did not consume fails rather than
    // truncating to the digits it liked.
    parseValue("12x", &ok);
    CHECK_FALSE(ok);
}

TEST_CASE("formatRatio is dimensionless and plain", "[value]") {
    CHECK(formatRatio(1.0, 10.0) == "1:10");
    CHECK(formatRatio(10.0, 1.0) == "10:1");
    // No engineering prefixes: "100m:1" is nonsense for a turns ratio.
    CHECK(formatRatio(0.1, 1.0) == "0.1:1");
    CHECK(formatRatio(8.0, 1.0) == "8:1");
}

TEST_CASE("formatPotValue marks a pot the way the part is sold", "[value][pot]") {
    // B is linear, A is audio (log), C is reverse audio -- the trade's markings.
    CHECK(formatPotValue(250000.0, "Linear") == "B250K");
    CHECK(formatPotValue(250000.0, "Log") == "A250K");
    CHECK(formatPotValue(250000.0, "RevLog") == "C250K");
    CHECK(formatPotValue(4700.0, "Log") == "A4.7K");

    // Uppercase K, as a part is marked.
    CHECK(formatPotValue(100000.0, "Linear") == "B100K");

    // Unset: a bare "0", not "B0" (which reads like a part number).
    CHECK(formatPotValue(0.0, "Linear") == "0");
    CHECK(formatPotValue(-1.0, "Log") == "0");
}

TEST_CASE("parsePotValue reads the code back and the value with it", "[value][pot]") {
    double value = 0.0;
    std::string taper;
    bool given = false;

    REQUIRE(parsePotValue("A250K", value, taper, given));
    CHECK(close(value, 250000.0));
    CHECK(given);
    CHECK(taper == "Log");

    REQUIRE(parsePotValue("B100K", value, taper, given));
    CHECK(close(value, 100000.0));
    CHECK(given);
    CHECK(taper == "Linear");

    REQUIRE(parsePotValue("C500K", value, taper, given));
    CHECK(close(value, 500000.0));
    CHECK(given);
    CHECK(taper == "RevLog");

    // No code: a plain value still parses, and says no taper was given.
    REQUIRE(parsePotValue("250K", value, taper, given));
    CHECK(close(value, 250000.0));
    CHECK_FALSE(given);

    // "M2" peels no taper code: M names no marking we have, so the whole string
    // reaches `parseValue` untouched. (What that then evaluates to is the
    // prefix-form wart above; what matters here is that nothing was mistaken for
    // a taper.)
    REQUIRE(parsePotValue("M2", value, taper, given));
    CHECK_FALSE(given);

    // A lone code is not a value and is refused.
    CHECK_FALSE(parsePotValue("A", value, taper, given));
}

TEST_CASE("Pot value text round-trips", "[value][pot]") {
    double value = 0.0;
    std::string taper;
    bool given = false;

    const std::pair<double, std::string> cases[] = {
        {250000.0, "Linear"},
        {250000.0, "Log"},
        {250000.0, "RevLog"},
        {4700.0, "Log"},
        {10000.0, "Linear"},
    };

    for (const auto& [v, t] : cases) {
        const std::string text = formatPotValue(v, t);
        INFO("pot round trip of " << v << " " << t << " via \"" << text << "\"");
        REQUIRE(parsePotValue(text, value, taper, given));
        CHECK(close(value, v));
        REQUIRE(given);
        CHECK(taper == t);
    }
}
