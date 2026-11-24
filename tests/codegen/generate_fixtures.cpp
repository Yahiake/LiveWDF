#include "compiler/EmitRecurrence.h"
#include "recurrence_fixture.h"
#include "wdf/Engine.h"

#include <fstream>

int main(int argc, char** argv) {
    if (argc != 4) return 1;
    for (int count = 0; count < 3; ++count) {
        std::ofstream header(argv[count + 1]);
        const wdf::Engine engine(recurrenceFixture(count), 48000.);
        livewdf::compiler::emitRecurrence(header, engine, 0, "out", count == 2);
        if (!header) return 1;
    }
}
