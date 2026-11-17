// Catch2 v2 single-header runner for the editor session tests.
//
// A copy of `tests/main.cpp`, for the same reason the editor keeps its own copy
// of anything it shares: this target must stay buildable without the core test
// target, and a two-line runner is cheaper to copy than to couple.
#define CATCH_CONFIG_RUNNER
#include <catch2.hpp>

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
