#include "FastLiveCache.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <limits>

namespace livewdf {
namespace {

constexpr int64_t maxFileBytes = 16 * 1024;
constexpr int maxEntries = 8;
constexpr int maxMasks = 5;

juce::var readCache(const juce::File& file) {
    if (file.getSize() == 0 || file.getSize() > maxFileBytes)
        return {};
    juce::FileInputStream stream(file);
    if (!stream.openedOk())
        return {};
    juce::MemoryBlock bytes;
    stream.readIntoMemoryBlock(bytes, maxFileBytes + 1);
    if (bytes.getSize() > maxFileBytes)
        return {};
    return juce::JSON::parse(juce::String::fromUTF8(
        static_cast<const char*>(bytes.getData()), static_cast<int>(bytes.getSize())));
}

std::optional<std::vector<unsigned>> readMasks(const juce::File& file, const juce::Identifier& key) {
    const auto cache = readCache(file);
    const auto entry = cache.getProperty(key, {});
    const auto* masks = entry.getArray();
    if (!masks || masks->size() > maxMasks)
        return {};
    std::vector<unsigned> result;
    for (const auto& value : *masks) {
        if (!value.isInt() && !value.isInt64())
            return {};
        const auto integer = static_cast<juce::int64>(value);
        if (integer < 0 || integer > std::numeric_limits<unsigned>::max())
            return {};
        const auto mask = static_cast<unsigned>(integer);
        if (std::find(result.begin(), result.end(), mask) != result.end())
            return {};
        result.push_back(mask);
    }
    return result;
}

bool writeMasks(const juce::File& file, const juce::Identifier& key,
                std::span<const unsigned> masks) {
    auto cache = readCache(file);
    if (!cache.isObject())
        cache = new juce::DynamicObject();
    auto& object = *cache.getDynamicObject();
    object.removeProperty(key);
    while (object.getProperties().size() >= maxEntries)
        object.removeProperty(object.getProperties().getName(0));

    juce::Array<juce::var> entry;
    for (const unsigned mask : masks)
        entry.add(static_cast<juce::int64>(mask));
    object.setProperty(key, juce::var(entry));
    const auto text = juce::JSON::toString(cache, true);
    return text.getNumBytesAsUTF8() <= maxFileBytes && file.replaceWithText(text);
}

} // namespace

void buildCachedAffineSetup(wdf::Engine::MixedAffineSetup& left,
                            wdf::Engine::MixedAffineSetup* right,
                            const std::string& document,
                            const juce::File& modelFile,
                            bool gridCurrent) {
    const juce::SHA256 documentHash(document.data(), document.size());
    juce::MemoryOutputStream fingerprint;
    // Bump the version when the calibration stimulus or mask semantics change.
    fingerprint.writeString("LiveWDF affine calibration v1");
    const auto digest = documentHash.getRawData();
    fingerprint.write(digest.getData(), digest.getSize());
    fingerprint.writeDouble(left.sampleRate());
    fingerprint.writeInt64(static_cast<juce::int64>(left.inputSource()));
    fingerprint.writeBool(gridCurrent);
    const auto transfer = left.transferMatrix();
    fingerprint.write(transfer.data(), transfer.size_bytes());
    const juce::Identifier key(juce::SHA256(fingerprint.getData(), fingerprint.getDataSize()).toHexString());

    const auto appCache = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("LiveWDF").getChildFile("FastLive").getChildFile(documentHash.toHexString() + ".json");
    const auto sidecar = modelFile == juce::File{} ? juce::File{}
        : modelFile.getSiblingFile(modelFile.getFileName() + ".fast-live-cache");
    auto masks = sidecar == juce::File{} ? std::nullopt : readMasks(sidecar, key);
    if (!masks)
        masks = readMasks(appCache, key);
    if (masks)
        left.build(std::span<const unsigned>(*masks));
    if (!masks || left.slopeMasks() != *masks) {
        left.build();
        masks = left.slopeMasks();
        if (sidecar == juce::File{} || !writeMasks(sidecar, key, *masks)) {
            if (appCache.getParentDirectory().createDirectory().wasOk())
                writeMasks(appCache, key, *masks);
        }
    }
    if (right)
        right->build(std::span<const unsigned>(*masks));
}

} // namespace livewdf
