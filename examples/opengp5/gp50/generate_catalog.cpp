#include "format/ControlGroups.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
struct Model {
    std::string label, schematic;
    std::vector<std::pair<std::string, int>> controls;
};

Model readModel(const char* label, const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream json;
    json << file.rdbuf();
    Model model{label, json.str(), {}};
    wdf::LiveWDFDocument doc;
    std::string error;
    if (!file || !wdf::LiveWDFDocument::fromJson(model.schematic, doc, error))
        throw std::runtime_error(path + ": " + error);
    for (const auto& group : wdf::potControlGroups(doc)) {
        const auto part = std::find_if(doc.components.begin(), doc.components.end(),
            [&](const auto& member) { return member.id == group.members.front().id; });
        const auto& values = part->values;
        auto position = values.find("pos");
        double value = position == values.end() ? 0.5 : position->second;
        auto caption = group.name;
        if (caption == "WAH WAH") caption = "Treadle";
        model.controls.emplace_back(std::move(caption), static_cast<int>(std::lround(value * 1000)));
    }
    return model;
}

void writeCatalog(const std::filesystem::path& folder, const std::vector<Model>& models) {
    std::ofstream config(folder / "catalog_config.h");
    std::ofstream schematics(folder / "catalog_schematics.inc");
    std::ofstream params(folder / "params.h");
    std::string choices;
    size_t controlCount = 0;
    size_t defaultModel = 0, wahModel = 0;
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& model = models[i];
        if (!choices.empty()) choices += '|';
        choices += model.label;
        if (model.label == "DOD 250") defaultModel = i;
        if (model.label == "Cry Baby") wahModel = i;
        controlCount = std::max(controlCount, model.controls.size());
        schematics << '{' << std::quoted(model.label) << ", R\"GP50SCH(" << model.schematic << ")GP50SCH\"},\n";
    }
    config << "#pragma once\n#define LIVEWDF_GP50_CATALOG 1\n"
           << "#define CATALOG_MODELS " << models.size() << "\n"
           << "#define CATALOG_DEFAULT " << defaultModel << "\n"
           << "#define CATALOG_WAH " << wahModel << "\n"
           << "#define CATALOG_CONTROLS " << controlCount << "\n";
    params << "#pragma once\n#include \"catalog_config.h\"\n"
           << "#define GP5_PARAMS(P, C) \\\n"
           << "    C(MODEL, \"Model\", \"Circuit\", " << models.size() << ", " << std::quoted(choices)
           << ", " << defaultModel << ", GP5_P_DSP) \\\n"
           << "    P(CONTROL, \"Control\", \"Circuit\", 0, " << controlCount - 1
           << ", 0, GP5_U_CUSTOM, 0) \\\n"
              "    P(VALUE, \"Value\", \"Circuit\", 0, 1000, 500, GP5_U_CUSTOM, 0) \\\n"
              "    P(LEVEL, \"Level\", \"Circuit\", 0, 100, 100, GP5_U_PERCENT, GP5_P_DSP) \\\n"
              "    C(ON, \"Effect\", \"Circuit\", 2, \"Bypass|On\", 1, GP5_P_DSP) \\\n"
              "    C(CAB, \"Cab\", \"Output\", 2, \"Off|On\", 0, GP5_P_DSP) \\\n"
              "    P(DELAY, \"Delay\", \"Output\", 0, 100, 0, GP5_U_PERCENT, GP5_P_DSP) \\\n"
              "    P(REVERB, \"Reverb\", \"Output\", 0, 100, 0, GP5_U_PERCENT, GP5_P_DSP)";
    for (size_t i = 0; i < controlCount; ++i)
        params << " \\\n    P(POT_" << i << ", \"Pot " << i + 1
               << "\", \"Circuit\", -1, 1000, -1, GP5_U_NONE, GP5_P_DSP | GP5_P_HIDDEN)";
    params << '\n';
    config << "#ifdef GP5_CTL_SIDE\n"
              "static const unsigned catalog_control_count[CATALOG_MODELS] = {";
    for (const auto& model : models) config << model.controls.size() << ',';
    config << "};\n"
              "static const struct { const char *name; int position; } "
              "catalog_controls[CATALOG_MODELS][CATALOG_CONTROLS] = {\n";
    for (const auto& model : models) {
        config << "    {";
        for (const auto& [label, position] : model.controls)
            config << '{' << std::quoted(label) << ',' << position << "},";
        config << "},\n";
    }
    config << "};\n#endif\n";
    if (!config || !schematics || !params)
        throw std::runtime_error("could not write GP-50 catalog in " + folder.string());
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 4 || argc % 2 != 0)
            throw std::runtime_error("expected an output folder followed by model name and schematic pairs");
        std::vector<Model> models;
        for (int i = 2; i < argc; i += 2)
            models.push_back(readModel(argv[i], argv[i + 1]));
        writeCatalog(argv[1], models);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
