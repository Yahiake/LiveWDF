#pragma once
#include <cstdlib>
#include <cstring>

// Experiment switch for the push-pull tries: LIVEWDF_PP=accept,slew,gridlaw,platelag,predict
inline bool livewdfPpTry(const char* name) {
    const char* flags = std::getenv("LIVEWDF_PP");
    return flags && std::strstr(flags, name);
}
