#pragma once

#include "NonlinearPreview.h"
#include <juce_core/juce_core.h>
#include <limits>
#include <optional>

// Non-realtime. The caller owns document, revision, rate, control, and range checks.
inline std::optional<wdf::preview::NonlinearPreview> readNonlinearPreview(const juce::var& root) {
    const auto* object = root.getDynamicObject();
    if (object == nullptr || object->getProperty("schema").toString() != "livewdf.nonlinear-preview.v1")
        return std::nullopt;
    auto number = [](const juce::var& value, float& result) {
        if (!(value.isDouble() || value.isInt() || value.isInt64())) return false;
        const double d = static_cast<double>(value);
        if (!std::isfinite(d) || std::abs(d) > std::numeric_limits<float>::max()) return false;
        result = static_cast<float>(d);
        return true;
    };
    auto array = [&](const char* name, auto& dest) {
        const auto* entries = object->getProperty(name).getArray();
        if (entries == nullptr || entries->size() != static_cast<int>(dest.size())) return false;
        for (int i = 0; i < entries->size(); ++i)
            if (!number(entries->getReference(i), dest[static_cast<std::size_t>(i)])) return false;
        return true;
    };
    wdf::preview::NonlinearPreview::Coefficients c;
    if (!(array("alpha0", c.alpha0) && array("alpha1", c.alpha1) && array("alpha2", c.alpha2)
          && array("input_scale", c.input_scale) && array("first_weights", c.first_weights)
          && array("first_bias", c.first_bias) && array("second_weights", c.second_weights)
          && array("second_bias", c.second_bias) && array("output_weights", c.output_weights)
          && number(object->getProperty("output_scale"), c.output_scale))) return std::nullopt;
    wdf::preview::NonlinearPreview result;
    if (!result.prepare(c)) return std::nullopt;
    return result;
}
