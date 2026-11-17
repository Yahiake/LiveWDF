// The fit: the frame button has to frame the *drawing*, wires included.
//
// **The failure this exists for.** "The fit button is broken, it only fits the
// components and not the wires." A sheet whose wire runs out past the last
// component is framed as if the wire were not there, so the wire is off the
// edge of the window and the user has to zoom by hand to find it.
//
// The frame is the one thing that is supposed to say "here is your whole
// circuit" -- and a circuit is its wires as much as its parts.

#include <catch2.hpp>

#include "SchematicModel.h"

#include <string>

using namespace livewdf::editor;

namespace {

/// A sheet with a resistor at the origin and a wire running a long way east.
Schematic sheetWithAWireReachingOut(int reach) {
    Schematic s;
    const std::string res = s.addPart("res", 0, 0);
    s.findPart(res)->pins["a"] = "in";
    s.findPart(res)->pins["b"] = "out";

    // The resistor's far pin, and a wire from it reaching `reach` squares.
    const auto pin = Schematic::pinPosition(*s.findPart(res), 1);
    s.addWire(pin.x, pin.y, pin.x + reach, pin.y);

    return s;
}

}  // namespace

TEST_CASE("The content bounds include the wires, not only the parts",
          "[schematic][geometry]") {
    // A wire that reaches twenty squares past the last component has to be
    // inside the frame.
    const auto s = sheetWithAWireReachingOut(20);
    const auto bounds = s.contentBounds();

    INFO("the bounds are " << bounds.toString());
    CHECK(bounds.getRight() >= 20.0f);
    CHECK(bounds.getWidth() > 20.0f);
}

TEST_CASE("The content bounds of a sheet with no parts are the wires'",
          "[schematic][geometry]") {
    // A sheet that is nothing but a wire -- routed between two points before
    // either part exists -- still has a frame.
    Schematic s;
    s.addWire(0, 0, 30, 0);

    const auto bounds = s.contentBounds();

    INFO("the bounds are " << bounds.toString());
    CHECK(bounds.getWidth() >= 30.0f);
}

TEST_CASE("The content bounds of an empty sheet are empty", "[schematic][geometry]") {
    Schematic s;
    CHECK(s.contentBounds().isEmpty());
}
