#pragma once

#include <cstddef>
#include <span>

namespace wdf {

struct Circuit;

// Fresh Preview circuits only. List every ideal voltage source that can vary
// during playback. The return value counts the removed pentodes.
std::size_t collapseMatchedPentodes(Circuit& circuit, std::span<const std::size_t> varyingVoltageSources = {});

} // namespace wdf
