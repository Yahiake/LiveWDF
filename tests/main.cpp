// Catch2 v2 single-header runner for LiveWDF tests.
#define CATCH_CONFIG_RUNNER
#include <catch2.hpp>

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
