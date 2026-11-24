#include "PreviewPlayback.h"
#include "NonlinearPreviewJson.h"
#include "livewdf/PreviewFingerprint.h"
#include <juce_cryptography/juce_cryptography.h>

namespace livewdf {
namespace {

bool number(const juce::var& v) {
    return (v.isDouble() || v.isInt() || v.isInt64()) && std::isfinite(static_cast<double>(v));
}
bool atMost(const juce::var& v, const char* key, double threshold) {
    const auto value = v.getProperty(key, {});
    return number(value) && double(value) <= threshold;
}

std::optional<wdf::preview::ModalPreview> readModal(const juce::var& root) {
    const auto qualification = root["qualification"];
    if (qualification["decision"].toString() != "accepted") return {};
    const auto m = qualification["metrics"];
    if (!atMost(m["impulse_level_disagreement"], "relative_rms_db", -45)
        || !atMost(m["impulse_fit"], "relative_rms_db", -60)) return {};
    for (auto level : {"35mV", "360mV", "1200mV"}) {
        auto v = m["multilevel_probes"][level];
        if (!atMost(v, "relative_rms_db", -55) || !atMost(v, "relative_peak_db", -35)) return {};
    }
    using namespace wdf::preview;
    const auto* rows = root["modes"].getArray();
    if (!number(root["direct"]) || !rows || rows->size() > int(ModalPreview::capacity)) return {};
    std::vector<Mode> modes;
    for (const auto& row : *rows) {
        const auto* a = row.getArray();
        if (!a || a->size() != 4 || !std::all_of(a->begin(), a->end(), number)) return {};
        modes.push_back({double((*a)[0]), double((*a)[1]), double((*a)[2]), double((*a)[3])});
    }
    ModalPreview result;
    if (!result.prepare(double(root["direct"]), modes)) return {};
    return result;
}

std::unique_ptr<PreviewPlayback> decode(const juce::var& root, const juce::String& digest, double rate) {
    // This identifies the reference device equations against which these fits qualified.
    constexpr auto reference = LIVEWDF_PREVIEW_ENGINE_SHA256;
    if (root["engine_sha256"].toString() != reference || root["document_sha256"].toString() != digest
        || !number(root["sample_rate"]) || double(root["sample_rate"]) != rate
        || !number(root["input_peak_v"]) || double(root["input_peak_v"]) <= 0
        || !number(root["output_bias_v"])) return {};
    auto play = std::make_unique<PreviewPlayback>();
    play->peak = double(root["input_peak_v"]);
    play->bias = double(root["output_bias_v"]);
    const auto schema = root["schema"].toString();
    if (schema == "livewdf.modal-preview.v1") {
        if (play->peak > 1.2) return {};
        auto modal = readModal(root);
        if (!modal) return {};
        play->channels = {*modal, *modal};
    } else if (schema == "livewdf.nonlinear-preview.v1") {
        const auto qualification = root["qualification"];
        if (!root["experimental"].isBool() || !bool(root["experimental"])
            || qualification["decision"].toString() != "experimental"
            || !qualification["manual_audition"].isBool() || !bool(qualification["manual_audition"])
            || play->peak > 1.2) return {};
        auto model = readNonlinearPreview(root);
        if (!model) return {};
        play->channels = {*model, *model};
    } else return {};
    return play;
}
}

std::unique_ptr<PreviewPlayback> loadPreviewPlayback(const std::string& document,
                                                      const juce::File& circuit, double sampleRate) {
    if (sampleRate != std::round(sampleRate) || !(sampleRate >= 8000 && sampleRate <= 384000)) return {};
    const auto rate = juce::String(int(sampleRate));
    const auto digest = juce::SHA256(document.data(), document.size()).toHexString();
    const auto cacheDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                              .getChildFile("LiveWDF").getChildFile("Preview");
    const auto cached = cacheDir.getChildFile(digest + "-" + rate + ".json");
    const auto local = circuit == juce::File{} ? juce::File{}
        : circuit.getSiblingFile(circuit.getFileName() + "." + rate + ".preview.json");
    for (const auto& file : {local, cached}) {
        constexpr int maxBytes = 256 * 1024;
        if (!file.existsAsFile() || file.getSize() > maxBytes) continue;
        juce::FileInputStream stream(file);
        juce::MemoryBlock bytes;
        if (!stream.openedOk() || !stream.readIntoMemoryBlock(bytes, maxBytes + 1)
                || bytes.getSize() > maxBytes) continue;
        auto play = decode(juce::JSON::parse(juce::String::fromUTF8(
            static_cast<const char*>(bytes.getData()), int(bytes.getSize()))), digest, sampleRate);
        if (play) {
            // Host sessions restore the document without its original file path.
            if (file == local && cacheDir.createDirectory().wasOk())
                cached.replaceWithData(bytes.getData(), bytes.getSize());
            return play;
        }
    }
    return {};
}

}
