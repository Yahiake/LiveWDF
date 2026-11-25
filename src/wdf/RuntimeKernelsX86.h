#pragma once

#include "wdf/RuntimeKernels.h"

// Test the actual target slice so an ARM64 build never references x86 code.
#if defined(LIVEWDF_BUILD_AVX2) && \
    (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
#define LIVEWDF_HAS_AVX2_KERNELS 1

namespace wdf::detail {
extern const RuntimeKernels avx2Kernels;
}  // namespace wdf::detail

#endif
