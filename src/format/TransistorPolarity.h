#pragma once

// One answer to "which polarity is this transistor?", shared by the document
// converter, the inspector and the artwork selector.
//
// The document stores polarity in two places on purpose:
//
//   * a real BJT/JFET model card carries it intrinsically (a 2N3906 is a PNP and
//     a 2N5460 is a P-channel JFET);
//   * a MOSFET has presets rather than a card registry, and a custom JFET or
//     MOSFET has no card at all, so those parts also carry an explicit
//     `polarity` property ("N-channel" or "P-channel").
//
// For a named card the card wins, because compiling it as the other polarity
// would be a lie. For `Custom` (or an unknown name with editable values) the
// explicit property supplies the answer. Old documents with neither are given
// the same defaults the converter has always used.

#include "devices/Bjt.h"
#include "devices/Jfet.h"
#include "devices/MosfetPresets.h"
#include "format/Document.h"

#include <string>
#include <string_view>

namespace wdf {

enum class TransistorPolarity {
    Unknown,
    NType,
    PType,
};

inline constexpr const char* kPolarityProperty = "polarity";
inline constexpr const char* kNChannel = "N-channel";
inline constexpr const char* kPChannel = "P-channel";

inline std::string componentProperty(const SchematicComponent& part,
                                     const char* key,
                                     const char* fallback = "") {
    const auto it = part.properties.find(key);
    return it != part.properties.end() ? it->second : std::string(fallback);
}

inline TransistorPolarity polarityFromProperty(const SchematicComponent& part) {
    const std::string value = componentProperty(part, kPolarityProperty);
    if (value == kPChannel)
        return TransistorPolarity::PType;
    if (value == kNChannel)
        return TransistorPolarity::NType;
    return TransistorPolarity::Unknown;
}

inline TransistorPolarity transistorPolarity(const SchematicComponent& part) {
    if (part.type == "bjt") {
        const std::string model = componentProperty(part, "model", "2N3904");
        if (const auto* entry = findBjtByName(model.c_str()))
            return entry->model.pnp ? TransistorPolarity::PType
                                    : TransistorPolarity::NType;
        return TransistorPolarity::Unknown;
    }

    if (part.type == "jfet") {
        const std::string model = componentProperty(part, "model", "2N5952");
        if (const auto* entry = findJfetByName(model.c_str()))
            return entry->model.p.pChannel ? TransistorPolarity::PType
                                           : TransistorPolarity::NType;
        const TransistorPolarity explicitPolarity = polarityFromProperty(part);
        return explicitPolarity == TransistorPolarity::Unknown
                   ? TransistorPolarity::NType
                   : explicitPolarity;
    }

    if (part.type == "mosfet") {
        // A missing preset is a custom part, not the 2N7000 default: the
        // converter treats a missing/unknown preset as Custom. Read the card
        // only when the property is actually present and names one, so an
        // explicit polarity on a hand-edited custom part still selects P
        // artwork and a P-channel device.
        const auto preset = part.properties.find("preset");
        if (preset != part.properties.end()) {
            if (const auto* entry = findMosfetPreset(preset->second))
                return entry->pChannel ? TransistorPolarity::PType
                                       : TransistorPolarity::NType;
        }
        const TransistorPolarity explicitPolarity = polarityFromProperty(part);
        return explicitPolarity == TransistorPolarity::Unknown
                   ? TransistorPolarity::NType
                   : explicitPolarity;
    }

    return TransistorPolarity::Unknown;
}

inline const char* transistorPolarityName(TransistorPolarity polarity) noexcept {
    switch (polarity) {
    case TransistorPolarity::NType: return kNChannel;
    case TransistorPolarity::PType: return kPChannel;
    case TransistorPolarity::Unknown: break;
    }
    return "";
}

}  // namespace wdf
