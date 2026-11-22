#pragma once

// Test-only ngspice oracle harness (verify.py methodology, in-process).
//
// Runs ngspice in batch mode on a deck and parses selected vectors back for
// comparison against LiveWDF's engine. Default ngspice path matches
// tools/tools/verify.py; override with the NGSPICE environment variable.
//
// Windows mechanics discovered empirically (2026-10-18, ngspice-47):
//   * `print v(x) > "C:\abs\path.txt"` inside a `.control` block silently
//     produces no file -- redirect targets must be UNQUOTED.
//   * To stay robust when the temp directory contains spaces, the deck is run
//     with the scratch directory as its working directory and uses relative
//     redirect names (`tag_k.txt`).
//   * The process is launched as `cmd /c ""<exe>" <args>"`: a bare quoted
//     command string handed to std::system() gets mangled by cmd.exe
//     ("The filename, directory name, or volume label syntax is incorrect.").

#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace lwdf_test {

namespace fs = std::filesystem;

inline fs::path ngspiceExe() {
#ifdef _MSC_VER
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "NGSPICE") == 0 && buf && *buf) {
        fs::path p(buf);
        std::free(buf);
        return p;
    }
    std::free(buf);
#else
    if (const char* env = std::getenv("NGSPICE"); env && *env)
        return fs::path(env);
#endif
#ifdef _WIN32
    return fs::path(R"(C:\Code\test\Spice64\bin\ngspice_con.exe)");
#else
    std::string path;
    char buf[4096];
    if (FILE* p = popen("command -v ngspice", "r")) {
        if (fgets(buf, sizeof buf, p)) path = buf;
        pclose(p);
    }
    return fs::path(path.substr(0, path.find('\n')));
#endif
}

/// True when the configured ngspice executable exists. Tests use this to turn
/// an absent external simulator into an explicit skip instead of making the
/// ordinary suite fail on a machine that has no ngspice installation.
inline bool ngspiceAvailable() {
    std::error_code ec;
    return fs::is_regular_file(ngspiceExe(), ec);
}

/// Guard for ngspice-dependent test cases. Returns false after emitting a
/// warning when the external simulator is not installed.
inline bool requireNgspice(const char* what) {
    if (ngspiceAvailable()) return true;
    WARN("ngspice not found; skipping " << what);
    return false;
}

/// Unique scratch dir under the system temp folder (ngspice runs here).
inline fs::path scratchDir() {
    static const fs::path d = [] {
        fs::path p = fs::temp_directory_path() / "livewdf_tests";
        fs::create_directories(p);
        return p;
    }();
    return d;
}

/// Name used *inside the deck* for a redirect target (relative, no quotes).
inline std::string redirectName(const std::string& tag, size_t k, bool op = false) {
    return tag + (op ? "_op_" : "_") + std::to_string(k) + ".txt";
}

/// Absolute path of that target, for reading the result back.
inline fs::path vecFile(const std::string& tag, size_t k) { return scratchDir() / redirectName(tag, k); }
inline fs::path vecFileOp(const std::string& tag, size_t k) {
    return scratchDir() / redirectName(tag, k, true);
}

struct NgspiceRun {
    bool ok = false;
    std::string console;  // captured ngspice log (for debugging failures)
};

/// Run a deck verbatim in batch mode. `tag` names the scratch .cir/.log files.
inline NgspiceRun runDeck(const std::string& deckBody, const std::string& tag) {
    const fs::path dir = scratchDir();
    const fs::path deckPath = dir / (tag + ".cir");
    const fs::path logPath = dir / (tag + ".log");
    {
        std::ofstream f(deckPath);
        f << deckBody;
    }

    // `cmd /c ""<exe>" -b -o "<log>" "<deck>""` with relative deck/log names,
    // executed from the scratch directory.
    std::string cmd = "\"" + ngspiceExe().string() + "\" -b -o \"" + (tag + ".log") + "\" \"" +
                      (tag + ".cir") + "\"";
#ifdef _WIN32
    cmd = "cmd /c \"" + cmd + "\"";
#endif

    NgspiceRun r;
    std::error_code ec;
    const fs::path saved = fs::current_path(ec);
    fs::current_path(dir, ec);
    r.ok = (std::system(cmd.c_str()) == 0);
    fs::current_path(saved, ec);

    std::ifstream log(logPath);
    std::stringstream ss;
    ss << log.rdbuf();
    r.console = ss.str();
    if (!r.ok)
        WARN("ngspice failed (" << cmd << "):\n" << r.console);
    return r;
}

/// Parse `print v(x) > file` output: one line per assignment, e.g.
/// `v(out) = 1.000000e+00`. Returns values in file order.
inline std::vector<double> parseOpFile(const fs::path& p) {
    std::ifstream f(p);
    REQUIRE(f.good());
    std::vector<double> out;
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        out.push_back(std::stod(line.substr(eq + 1)));
    }
    REQUIRE_FALSE(out.empty());
    return out;
}

/// Parsed `linearize`d transient table: `Index  time  v(x)...` rows.
struct TranTable {
    std::vector<double> time;
    std::vector<std::vector<double>> cols;  // cols[node][row]
    size_t nNodes() const { return cols.size(); }
    size_t nRows() const { return time.size(); }
};

inline TranTable parseTranFile(const fs::path& p, size_t nNodesHint = 0) {
    std::ifstream f(p);
    REQUIRE(f.good());
    TranTable t;
    std::string line;
    const size_t hint = nNodesHint;  // 0: count the columns ourselves
    bool indexCol = false;           // a leading index cell (seen the header)
    while (std::getline(f, line)) {
        if (line.empty() || line.rfind("---", 0) == 0)
            continue;
        std::istringstream ls(line);
        std::string first;
        if (!(ls >> first))
            continue;

        // Column header of `print`/`linearize` output: `Index  time  v(x)...`.
        // Count the value cells (everything after `time`) when the caller did
        // not pin the column count, then keep going: wrdata has no header at
        // all, so its first data row supplies the count instead.
        if (first == "Index") {
            if (hint == 0 && t.cols.empty()) {
                std::string tok;
                size_t cells = 0;
                while (ls >> tok)
                    ++cells;  // `time` plus one cell per requested vector
                if (cells > 0)
                    t.cols.resize(cells - 1);
            }
            indexCol = true;
            continue;
        }

        // Banner/comment lines (`* title`, `Transient Analysis ...`) never parse
        // as a number. In a table, the row index came first, so the time is the
        // next cell; in wrdata output the time is the first cell.
        // Collect every numeric cell of the row.
        std::vector<double> cells;
        {
            double v = 0.0;
            std::istringstream all(line);
            while (all >> v)
                cells.push_back(v);
        }
        if (cells.empty())
            continue;

        double time = 0.0;
        std::vector<double> row;
        if (indexCol) {
            // `Index time v1 v2 ...`: cell[0] is the row index, cell[1] is the
            // grid time, the rest are the vector values.
            if (cells.size() < 2)
                continue;
            time = cells[1];
            row.assign(cells.begin() + 2, cells.end());
        } else {
            // wrdata: `time v1 time v2 ...` — the independent variable is
            // interleaved with every vector value. Keep every cell as a column
            // so the caller can index `cols[2*j]` (time, repeated) /
            // `cols[2*j+1]` (value); tt.time is the grid (cell[0]).
            time = cells[0];
            row = cells;
        }

        // Size the columns from the first row that has no header above it.
        if (hint != 0)
            t.cols.resize(hint);
        if (t.cols.empty())
            t.cols.resize(row.size());
        // A row that does not fill every column means the table was truncated or
        // re-wrapped mid-row; say so instead of silently misaligning columns.
        REQUIRE(row.size() == t.cols.size());
        t.time.push_back(time);
        for (size_t n = 0; n < t.cols.size(); ++n)
            t.cols[n].push_back(row[n]);
    }
    REQUIRE_FALSE(t.time.empty());
    return t;
}

/// Build a `.control` block that runs `analysisLines`, writes each named vector
/// expression to a relative scratch file, and (optionally) writes the DC
/// operating point to `<tag>_op_<k>.txt` afterwards.
inline std::string controlBlock(const std::vector<std::string>& analysisLines,
                                const std::vector<std::string>& vecExprs, const std::string& tag,
                                bool doOpAfter = false) {
    std::ostringstream d;
    // `print` defaults to 7 significant digits, which is not enough to check
    // KCL on milliampere-scale terminal currents (their rounded sum can exceed
    // 1e-8 A). Ask for full double precision instead of relaxing the check.
    d << ".control\nset filetype=ascii\nset numdgt=15\n";
    for (const auto& a : analysisLines)
        d << a << "\n";
    for (size_t k = 0; k < vecExprs.size(); ++k)
        d << "print " << vecExprs[k] << " > " << redirectName(tag, k) << "\n";
    if (doOpAfter) {
        d << "op\n";
        for (size_t k = 0; k < vecExprs.size(); ++k)
            d << "print " << vecExprs[k] << " > " << redirectName(tag, k, true) << "\n";
    }
    d << ".endc\n";
    return d.str();
}

/// Stable text form for CSV cells. Full round-trip precision for doubles.
inline std::string csvNumber(double v) {
    std::ostringstream s;
    s << std::setprecision(17) << v;
    return s.str();
}

inline std::string csvText(std::string s) {
    const bool quote = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!quote) return s;
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/// Write a machine-readable comparison table beside the ngspice decks. The
/// file is written even when individual comparisons fail, so a failing run
/// still leaves the complete measured matrix behind. Returns the file path.
inline fs::path writeCsv(const std::string& tag, const std::vector<std::string>& header,
                         const std::vector<std::vector<std::string>>& rows) {
    const fs::path p = scratchDir() / (tag + ".csv");
    std::ofstream f(p);
    REQUIRE(f.good());
    for (size_t c = 0; c < header.size(); ++c) {
        if (c) f << ',';
        f << csvText(header[c]);
    }
    f << '\n';
    for (const auto& row : rows) {
        REQUIRE(row.size() == header.size());
        for (size_t c = 0; c < row.size(); ++c) {
            if (c) f << ',';
            f << csvText(row[c]);
        }
        f << '\n';
    }
    WARN("oracle report: " << p.string());
    return p;
}

/// Absolute + relative comparison helper. `floor` is the absolute scale below
/// which a relative error would be meaningless (near-zero current/voltage).
inline double relativeError(double reference, double actual, double floor) {
    return std::abs(actual - reference) / std::max(std::abs(reference), floor);
}

}  // namespace lwdf_test
