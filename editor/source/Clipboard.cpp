#include "Clipboard.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace livewdf::editor {

namespace {

/// The first line, so a paste can tell our text from anything else that
/// happens to be on the clipboard.
const char* kMagic = "LIVWDF-GROUP-1";

/// A tab-separated field, with the tab and the newline escaped, so a value
/// with a space in it survives and one with a tab in it does not break the
/// line.
std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (const char c : text) {
        switch (c) {
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\\': out += "\\\\"; break;
        default: out += c; break;
        }
    }

    return out;
}

std::string unescape(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }

        switch (text[++i]) {
        case 't': out += '\t'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case '\\': out += '\\'; break;
        default: out += text[i]; break;
        }
    }

    return out;
}

/// Split a line on tabs.
std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out;
    std::string current;

    for (const char c : line) {
        if (c == '\t') {
            out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }

    out.push_back(current);
    return out;
}

}  // namespace

void ClipboardGroup::bounds(float& minX, float& minY, float& maxX, float& maxY) const {
    minX = minY = std::numeric_limits<float>::max();
    maxX = maxY = std::numeric_limits<float>::lowest();

    const auto include = [&](float x, float y) {
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    };

    for (const auto& part : parts)
        include(part.x, part.y);

    for (const auto& wire : wires) {
        include(static_cast<float>(wire.ax), static_cast<float>(wire.ay));
        include(static_cast<float>(wire.bx), static_cast<float>(wire.by));
    }
}

std::string ClipboardGroup::toText() const {
    std::ostringstream out;
    out << kMagic << "\n";

    for (const auto& part : parts) {
        out << "P\t" << part.type << "\t" << part.x << "\t" << part.y << "\t"
            << part.rotation << "\t" << (part.flipped ? 1 : 0) << "\t";

        for (size_t i = 0; i < part.pins.size(); ++i) {
            if (i > 0)
                out << ",";
            out << part.pins[i];
        }
        out << "\n";

        for (const auto& [key, value] : part.values)
            out << "V\t" << escape(key) << "\t" << value << "\n";

        for (const auto& [key, value] : part.properties)
            out << "R\t" << escape(key) << "\t" << escape(value) << "\n";
    }

    for (const auto& wire : wires)
        out << "W\t" << wire.ax << "\t" << wire.ay << "\t"
            << wire.bx << "\t" << wire.by << "\n";

    return out.str();
}

ClipboardGroup ClipboardGroup::fromText(const std::string& text) {
    ClipboardGroup group;

    std::istringstream in(text);
    std::string line;

    if (!std::getline(in, line))
        return group;

    // Not ours: leave the clipboard alone.
    if (line != kMagic)
        return group;

    while (std::getline(in, line)) {
        if (line.empty())
            continue;

        const auto f = fields(line);

        if (f[0] == "P" && f.size() >= 7) {
            ClipboardPart part;
            part.type = f[1];
            part.x = static_cast<float>(std::atof(f[2].c_str()));
            part.y = static_cast<float>(std::atof(f[3].c_str()));
            part.rotation = std::atoi(f[4].c_str());
            part.flipped = f[5] == "1";

            std::stringstream pins(f[6]);
            std::string pin;
            while (std::getline(pins, pin, ','))
                if (!pin.empty())
                    part.pins.push_back(pin);

            group.parts.push_back(std::move(part));
        } else if (f[0] == "V" && f.size() >= 3) {
            if (!group.parts.empty())
                group.parts.back().values[unescape(f[1])] = std::atof(f[2].c_str());
        } else if (f[0] == "R" && f.size() >= 3) {
            if (!group.parts.empty())
                group.parts.back().properties[unescape(f[1])] = unescape(f[2]);
        } else if (f[0] == "W" && f.size() >= 5) {
            group.wires.push_back({std::atoi(f[1].c_str()), std::atoi(f[2].c_str()),
                                   std::atoi(f[3].c_str()), std::atoi(f[4].c_str())});
        }
    }

    return group;
}

}  // namespace livewdf::editor
