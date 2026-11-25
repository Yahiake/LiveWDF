#include "generated_c99.h"

static LiveWdfCircuit circuit;

void test_c99_reset(void) { livewdf_reset(&circuit); }
float test_c99_step(float input) { return livewdf_step(&circuit, input); }
