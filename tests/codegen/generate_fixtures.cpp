#include "compiler/EmitRecurrence.h"
#include "recurrence_fixture.h"
#include "wdf/Engine.h"

#include <fstream>

int main(int argc, char** argv) {
    if (argc != 6) return 1;
    for (int count = 0; count < 4; ++count) {
        std::ofstream header(argv[count + 1]);
        const wdf::Engine engine(recurrenceFixture(count), 48000.);
        livewdf::compiler::emitRecurrence(header, engine, 0, "out", count == 2);
        if (!header) return 1;
    }
    std::ofstream header(argv[5]);
    const wdf::Engine engine(recurrenceFixture(0), 44100.);
    livewdf::compiler::emitRecurrence(header, engine, 0, "out", false, livewdf::compiler::RecurrenceTarget::C99Float);
    return header ? 0 : 1;
}
