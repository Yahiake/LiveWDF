// Tests for the editor's `Schematic` model.
//
// Why this exists: the model is what every canvas gesture actually edits --
// `addWire`, `resizeWireEnd`, `mergeCollinearWires`, `extractNets` -- so a bug
// here is a bug in every gesture at once, and it is invisible in a screenshot.
// Checking it headlessly means connectivity and pin geometry are pinned by
// assertions rather than by dragging a wire around and squinting.
//
// The rule the engine work follows applies here too: the model joins the checker
// (this file) before the canvas is allowed to depend on it.

#include <catch2.hpp>

#include "SchematicModel.h"
#include "format/PartLibrary.h"
#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <cmath>

using namespace livewdf::editor;

namespace {

/// The net index of pin `pinIndex` of the part with `id`, or -1.
int netOfPin(const Schematic& s, const std::string& id, int pinIndex) {
    const auto nets = s.extractNets();
    const int index = s.indexOfPart(id);
    if (index < 0)
        return -1;
    if (pinIndex < 0 || pinIndex >= kMaxPins)
        return -1;
    return nets.netOfPart[static_cast<size_t>(index)][static_cast<size_t>(pinIndex)];
}

std::string netName(const Schematic& s, int netIndex) {
    const auto nets = s.extractNets();
    if (netIndex < 0 || netIndex >= static_cast<int>(nets.netNames.size()))
        return "";
    return nets.netNames[static_cast<size_t>(netIndex)];
}

/// True if two pins are on the same net.
bool pinsConnected(const Schematic& s, const std::string& aId, int aPin,
                   const std::string& bId, int bPin) {
    const int a = netOfPin(s, aId, aPin);
    const int b = netOfPin(s, bId, bPin);
    return a >= 0 && a == b;
}

}  // namespace

// ============================================================================
// Pin geometry
// ============================================================================

TEST_CASE("A two-pin part's pins sit symmetrically about the part's centre",
          "[schematic][geometry]") {
    Schematic s;
    s.addPart("res", 10, 10);
    const auto& part = s.getParts().front();

    const auto a = Schematic::pinPosition(part, 0);
    const auto b = Schematic::pinPosition(part, 1);

    // The contract: two pins, one each side of the centre, at the same distance
    // from it. Stated this way rather than as "a is left of b" because the
    // orientation is Céline's to decide -- its two-pin parts are drawn
    // vertically -- and pinning an axis here makes the test fail for the right
    // reason (a re-oriented symbol) rather than catching the thing that actually
    // matters (an asymmetric one).
    const int ax = a.x, ay = a.y, bx = b.x, by = b.y;

    const auto da = std::hypot(ax - 10.0, ay - 10.0);
    const auto db = std::hypot(bx - 10.0, by - 10.0);

    CHECK(da > 0.0);
    CHECK(db > 0.0);
    CHECK(da == db);
    CHECK((ax - 10) == -(bx - 10));
    CHECK((ay - 10) == -(by - 10));
}

TEST_CASE("Rotating a part rotates its pins the same way", "[schematic][geometry]") {
    Schematic s;
    s.addPart("res", 10, 10);
    auto& part = s.getParts().front();

    const auto before = Schematic::pinPosition(part, 0);

    part.visual.rotation = 90;
    const auto after = Schematic::pinPosition(part, 0);

    // A quarter turn clockwise: a point to the left of centre goes above it.
    CHECK(after.x == 10 - (before.y - 10));
    CHECK(after.y == 10 + (before.x - 10));
}

TEST_CASE("Mirroring swaps a pin across the part's vertical axis",
          "[schematic][geometry]") {
    Schematic s;
    s.addPart("res", 10, 10);
    auto& part = s.getParts().front();

    const auto before = Schematic::pinPosition(part, 0);
    part.visual.flipped = true;
    const auto after = Schematic::pinPosition(part, 0);

    CHECK(after.x == 10 - (before.x - 10));
    CHECK(after.y == before.y);
}

TEST_CASE("partBounds covers every pin", "[schematic][geometry]") {
    Schematic s;
    s.addPart("triode", 20, 20);
    const auto& part = s.getParts().front();
    const auto bounds = Schematic::partBounds(part);

    for (int pin = 0; pin < 3; ++pin) {
        const auto p = Schematic::pinPosition(part, pin);
        CHECK(bounds.contains(static_cast<float>(p.x), static_cast<float>(p.y)));
    }
}

TEST_CASE("A part's box is its symbol, not a box drawn around it",
          "[schematic][geometry]") {
    // **The rule that makes the sheet feel right, and the one it used to
    // break.** A part is picked where its symbol is -- not anywhere near it.
    //
    // The old box was a fixed six-by-eight grid squares of body plus another
    // square of slack, so a resistor claimed a quarter of the space around it:
    // clicking near a part grabbed the part, the wires that reached it could
    // not be picked at all, and a box drawn carefully round a symbol caught
    // nothing.
    //
    // The rule is Céline's `elementBounds`: the artwork's own box, plus the
    // pins, with a floor of 1.2 squares for the symbols that are genuinely
    // thin.
    Schematic s;
    s.addPart("res", 20, 20);
    const auto& part = s.getParts().front();
    const auto bounds = Schematic::partBounds(part);

    // The resistor's artwork is 1.52 squares wide and 4.12 tall.
    CHECK(bounds.getWidth() < 2.0f);
    CHECK(bounds.getHeight() < 4.5f);

    // Two squares to the side of the body is not the resistor.
    CHECK_FALSE(bounds.contains(22.0f, 20.0f));
    CHECK_FALSE(bounds.contains(20.0f, 20.0f + 3.0f));

    // And the body itself is.
    CHECK(bounds.contains(20.0f, 20.0f));
}

TEST_CASE("A click is tested against the drawn symbol, not its bounding box",
          "[schematic][geometry]") {
    // The bounding box is still useful for framing and enclosing, but it is the
    // wrong shape for a pointer. A capacitor is the clearest case: its box spans
    // the gap between the plates, while the drawing there is empty.
    Schematic s;
    s.addPart("cap", 20, 20);
    const auto& cap = s.getParts().front();

    CHECK(Schematic::partBounds(cap).contains(20.0f, 20.0f));
    CHECK_FALSE(s.hitTestPart(cap, 20.0f, 20.0f, 0.05f));

    // The plate itself is ink, and therefore a hit.
    CHECK(s.hitTestPart(cap, 20.0f, 19.65f, 0.05f));

    // Pin dots live outside the SVG, but they are drawn and catch the pointer.
    CHECK(s.hitTestPart(cap, 20.0f, 22.0f, 0.05f));

    // A closed symbol is picked through its body, not just on its outline, so a
    // resistor does not become a hollow rectangle.
    s.addPart("res", 30, 20);
    CHECK(s.hitTestPart(s.getParts().back(), 30.0f, 20.0f, 0.05f));

    // A valve's box has empty corners outside its envelope. The old box test
    // grabbed the valve there; the drawn shape does not.
    s.addPart("triode", 40, 20);
    const auto& triode = s.getParts().back();
    CHECK(Schematic::partBounds(triode).contains(37.0f, 17.0f));
    CHECK_FALSE(s.hitTestPart(triode, 37.0f, 17.0f, 0.05f));

    // The envelope itself is a region, so its interior is still a hit.
    CHECK(s.hitTestPart(triode, 40.0f, 20.0f, 0.0f));
}

TEST_CASE("A click follows the symbol's mirror and rotation",
          "[schematic][geometry]") {
    Schematic s;

    // A resistor's body is upright at rest and lying down after a quarter turn.
    s.addPart("res", 30, 20);
    auto& res = s.getParts().back();
    CHECK_FALSE(s.hitTestPart(res, 30.9f, 20.0f, 0.05f));
    res.visual.rotation = 90;
    CHECK(s.hitTestPart(res, 30.9f, 20.0f, 0.05f));

    // The triode's grid lead runs left; mirrored, it runs right. The point is
    // outside the envelope either way, so only the transformed lead can hit it.
    s.addPart("triode", 40, 20);
    auto& triode = s.getParts().back();
    CHECK_FALSE(s.hitTestPart(triode, 42.2f, 20.0f, 0.05f));
    triode.visual.flipped = true;
    CHECK(s.hitTestPart(triode, 42.2f, 20.0f, 0.05f));
}

TEST_CASE("A crossing box only catches a part where it is drawn",
          "[schematic][geometry]") {
    Schematic s;
    s.addPart("cap", 20, 20);
    const auto& cap = s.getParts().front();

    // A box in the empty gap between the plates is inside the old bounds but
    // touches no ink.
    CHECK_FALSE(s.partTouchesBox(cap, {19.9f, 19.9f, 0.2f, 0.2f}));

    // Crossing a plate is a hit.
    CHECK(s.partTouchesBox(cap, {19.0f, 19.5f, 2.0f, 0.2f}));

    // And so is a box drawn entirely round it.
    CHECK(s.partTouchesBox(cap, {18.0f, 17.5f, 4.0f, 5.0f}));

    // The valve's empty box corner is not its symbol.
    s.addPart("triode", 40, 20);
    const auto& triode = s.getParts().back();
    CHECK_FALSE(s.partTouchesBox(triode, {36.9f, 16.9f, 0.2f, 0.2f}));
}

TEST_CASE("A part's box turns with the part", "[schematic][geometry]") {
    // The box is transformed the way the drawing transforms the symbol --
    // mirror, then rotation -- so a turned part is still picked where it is
    // drawn. A box that did not turn would be a box in the wrong place.
    Schematic s;
    s.addPart("res", 20, 20);
    auto part = s.getParts().front();

    const auto upright = Schematic::partBounds(part);

    part.visual.rotation = 90;
    const auto turned = Schematic::partBounds(part);

    // The same box, on its side.
    CHECK(std::abs(turned.getWidth() - upright.getHeight()) < 0.01f);
    CHECK(std::abs(turned.getHeight() - upright.getWidth()) < 0.01f);
    CHECK(std::abs(turned.getCentreX() - upright.getCentreX()) < 0.01f);
    CHECK(std::abs(turned.getCentreY() - upright.getCentreY()) < 0.01f);
}

TEST_CASE("A wire is picked by its line, and the closest one wins", "[schematic][geometry]") {
    // **Céline's rule.** The distance from the pointer to the wire's *line*, so
    // a wire is caught by how close the pointer is to it -- and the *closest*
    // wire wins.
    //
    // Ours walked the wires and took the first it found, which was the last one
    // drawn rather than the nearest one: a pointer near one end of a long wire
    // could catch a short wire crossing it.
    Schematic s;
    s.addPart("res", 10, 10);

    const auto near = s.addWire(12, 8, 28, 8).front();
    const auto far = s.addWire(12, 12, 28, 12).front();

    // On the first wire's line...
    CHECK(s.wireNear(20.0f, 8.0f, 0.3f) == near);
    CHECK(s.wireNear(20.0f, 8.2f, 0.3f) == near);

    // ...and on the second one's.
    CHECK(s.wireNear(20.0f, 12.0f, 0.3f) == far);

    // **The closest wins**, whichever was drawn last: a pointer one square
    // above the lower wire is nearer the upper one.
    CHECK(s.wireNear(20.0f, 9.0f, 1.5f) == near);

    // And nothing is caught from far away.
    CHECK(s.wireNear(20.0f, 30.0f, 0.3f) == -1);
}

TEST_CASE("A diagonal drag is an L of two axis-aligned wires", "[schematic][geometry]") {
    // Every wire on the sheet is axis-aligned -- a diagonal is split -- so the
    // wire picker only ever has to think about two directions.
    Schematic s;
    s.addPart("res", 10, 10);
    s.addPart("res", 30, 20);

    const auto ids = s.addWire(12, 12, 28, 20);
    REQUIRE(ids.size() == 2);

    for (const auto id : ids) {
        const auto* wire = s.findWire(id);
        REQUIRE(wire != nullptr);
        CHECK((wire->isHorizontal() || wire->isVertical()));
    }
}

// ============================================================================
// Connectivity
// ============================================================================

TEST_CASE("Two touching pins are connected with no wire at all",
          "[schematic][nets]") {
    Schematic s;
    const std::string firstId = s.addPart("res", 10, 10);

    // A second resistor placed so its pin 0 lands exactly on the first's pin 1.
    // Coincident pins are a connection -- this is the property that makes butting
    // two parts together work.
    const auto* first = s.findPart(firstId);
    REQUIRE(first != nullptr);
    const auto rightPin = Schematic::pinPosition(*first, 1);

    auto second = makeComponent("res", s.getParts(), 0.0f, 0.0f);
    second.visual.x = 0.0f;
    second.visual.y = 0.0f;
    const std::string secondId = s.addPart(std::move(second));

    // Shift the second part so *its* pin 0 is on the first's pin 1. The delta is
    // read from the two pins rather than assumed to be along x: the pin frame is
    // Céline's and its two-pin parts are drawn vertically, so which axis the gap
    // is on is the model's business, not this test's.
    auto* placed = s.findPart(secondId);
    REQUIRE(placed != nullptr);
    const auto placedZero = Schematic::pinPosition(*placed, 0);
    placed->visual.x += static_cast<float>(rightPin.x - placedZero.x);
    placed->visual.y += static_cast<float>(rightPin.y - placedZero.y);

    CHECK(pinsConnected(s, firstId, 1, secondId, 0));
}

TEST_CASE("A wire joins the two pins it spans", "[schematic][nets]") {
    Schematic s;
    const std::string aId = s.addPart("res", 10, 10);
    const std::string bId = s.addPart("res", 20, 10);

    const auto a = Schematic::pinPosition(*s.findPart(aId), 1);
    const auto b = Schematic::pinPosition(*s.findPart(bId), 0);

    s.addWire(a.x, a.y, b.x, b.y);

    CHECK(pinsConnected(s, aId, 1, bId, 0));
    CHECK_FALSE(pinsConnected(s, aId, 0, bId, 1));
}

TEST_CASE("A wire ending partway along another joins them (T-junction)",
          "[schematic][nets]") {
    Schematic s;

    // A horizontal wire from (0,0) to (10,0), and a vertical one landing on its
    // middle at (5,0). The extractor must see the T.
    s.addWire(0, 0, 10, 0);
    s.addWire(5, 0, 5, 5);

    const auto nets = s.extractNets();

    // Everything is one net: the two wires share the point (5,0).
    REQUIRE(nets.netOfWire.size() == 2);
    CHECK(nets.netOfWire[0] == nets.netOfWire[1]);
}

TEST_CASE("Two crossing wires with no shared endpoint are not connected",
          "[schematic][nets]") {
    Schematic s;

    // A plus sign: (0,5)-(10,5) and (5,0)-(5,10) meet at (5,5), but neither has
    // an endpoint there, so they must NOT join. A crossing is not a connection.
    s.addWire(0, 5, 10, 5);
    s.addWire(5, 0, 5, 10);

    const auto nets = s.extractNets();

    REQUIRE(nets.netOfWire.size() == 2);
    CHECK(nets.netOfWire[0] != nets.netOfWire[1]);
}

TEST_CASE("Ground names merge by name, not by wire", "[schematic][nets]") {
    Schematic s;
    const std::string srcId = s.addPart("src", 4, 10);
    const std::string capId = s.addPart("cap", 20, 10);

    // Grounding each pin with a gnd symbol connects them without a wire,
    // because both are node "0". (gnd pin offset is (0, -2))
    const auto srcNeg = Schematic::pinPosition(*s.findPart(srcId), 1);
    const auto capB = Schematic::pinPosition(*s.findPart(capId), 1);
    s.addPart("gnd", srcNeg.x, srcNeg.y + 2);
    s.addPart("gnd", capB.x, capB.y + 2);

    const auto nets = s.extractNets();
    const int srcIndex = s.indexOfPart(srcId);
    const int capIndex = s.indexOfPart(capId);

    const int srcNegNet = nets.netOfPart[static_cast<size_t>(srcIndex)][1];
    const int capBNet = nets.netOfPart[static_cast<size_t>(capIndex)][1];

    // They may be different *net indices* (extraction is geometric), but they
    // must carry the same *name*, which is what the converter maps by.
    CHECK(nets.netNames[static_cast<size_t>(srcNegNet)] == "0");
    CHECK(nets.netNames[static_cast<size_t>(capBNet)] == "0");
}

TEST_CASE("A pin on its own is reported as a dangling net", "[schematic][nets]") {
    Schematic s;
    s.addPart("res", 10, 10);

    const auto nets = s.extractNets();

    // Both pins of a lone resistor touch exactly one thing each.
    CHECK(nets.danglingNets.size() == 2);
}

// ============================================================================
// Wires as things
// ============================================================================

TEST_CASE("A diagonal wire becomes an L of two segments", "[schematic][wires]") {
    Schematic s;
    const auto ids = s.addWire(0, 0, 4, 4);

    REQUIRE(ids.size() == 2);
    const auto* first = s.findWire(ids[0]);
    const auto* second = s.findWire(ids[1]);

    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    // Across, then down: the corner is at (4,0).
    CHECK(first->isHorizontal());
    CHECK(second->isVertical());
    CHECK(second->ax == 4);
    CHECK(second->ay == 0);

    // They share the corner, so the extractor sees one net through both.
    const auto nets = s.extractNets();
    CHECK(nets.netOfWire[0] == nets.netOfWire[1]);
}

TEST_CASE("A zero-length wire is not added", "[schematic][wires]") {
    Schematic s;
    CHECK(s.addWire(3, 3, 3, 3).empty());
    CHECK(s.getWires().empty());
}

TEST_CASE("Resizing a wire end keeps it axis-aligned", "[schematic][wires]") {
    Schematic s;
    const auto id = s.addWire(0, 0, 10, 0).front();

    // Drag the 'b' end somewhere off the line; only the along-axis coordinate
    // may move, so it stays horizontal.
    CHECK(s.resizeWireEnd(id, 1, 7, 5));

    const auto* wire = s.findWire(id);
    REQUIRE(wire != nullptr);
    CHECK(wire->isHorizontal());
    CHECK(wire->bx == 7);
    CHECK(wire->by == 0);
    CHECK(wire->ax == 0);
}

TEST_CASE("Resizing a wire end onto its anchor is refused", "[schematic][wires]") {
    Schematic s;
    const auto id = s.addWire(0, 0, 10, 0).front();

    // Landing exactly on the other end would make a wire that is invisible and
    // unclickable, so it is refused rather than allowed and then special-cased.
    CHECK_FALSE(s.resizeWireEnd(id, 1, 0, 0));
}

TEST_CASE("Two collinear wires drawn end to end merge into one",
          "[schematic][merge]") {
    Schematic s;
    s.addWire(0, 0, 5, 0);
    s.addWire(5, 0, 10, 0);

    REQUIRE(s.getWires().size() == 2);
    const int removed = s.mergeCollinearWires();

    CHECK(removed == 1);
    REQUIRE(s.getWires().size() == 1);
    CHECK(s.getWires().front().ax == 0);
    CHECK(s.getWires().front().bx == 10);
}

TEST_CASE("A merge keeps the preferred wire's id", "[schematic][merge]") {
    Schematic s;
    const auto first = s.addWire(0, 0, 5, 0).front();
    const auto second = s.addWire(5, 0, 10, 0).front();

    s.mergeCollinearWires(second);

    // The wire being dragged must still be selected when the drag ends, so its
    // id has to survive the merge.
    CHECK(s.findWire(second) != nullptr);
    CHECK(s.findWire(first) == nullptr);
}

TEST_CASE("A T merges, and the stem stays connected through the seam",
          "[schematic][merge]") {
    Schematic s;

    // Two collinear horizontals meeting at (5,0), and a vertical stem ending
    // there. The stem's own endpoint registers (5,0), so the seam survives the
    // merge and the stem is still joined to the merged bar. A T is safe to
    // merge; the drawing is one line and the net is one net either way.
    s.addWire(0, 0, 5, 0);
    s.addWire(5, 0, 10, 0);
    s.addWire(5, 0, 5, -5);

    s.mergeCollinearWires();

    // The two horizontals became one; the stem is untouched.
    CHECK(s.getWires().size() == 2);

    const auto nets = s.extractNets();
    REQUIRE(nets.netOfWire.size() == 2);
    CHECK(nets.netOfWire[0] == nets.netOfWire[1]);
}

TEST_CASE("A merge that would strand a wire at a four-way node is refused",
          "[schematic][merge]") {
    Schematic s;

    // Two horizontals and two verticals all meeting at (5,0). Merging the two
    // horizontals is fine (the verticals still end at the seam), but merging the
    // two verticals afterwards would leave only a long horizontal and a long
    // vertical that merely *cross* at (5,0) -- and a crossing is not a
    // connection. The sheet looks identical and the circuit has come apart in the
    // middle, so the second merge must be refused.
    s.addWire(0, 0, 5, 0);
    s.addWire(5, 0, 10, 0);
    s.addWire(5, -5, 5, 0);
    s.addWire(5, 0, 5, 5);

    s.mergeCollinearWires();

    const auto nets = s.extractNets();

    // Whatever merged, every wire must still be on one net: the node must not
    // have split.
    for (const int net : nets.netOfWire)
        CHECK(net == nets.netOfWire.front());
}

// ============================================================================
// Junctions
// ============================================================================

TEST_CASE("A T-junction earns a dot", "[schematic][junctions]") {
    Schematic s;
    s.addWire(0, 0, 10, 0);
    s.addWire(5, 0, 5, 5);

    const auto junctions = s.findJunctions();
    REQUIRE(junctions.size() == 1);
    CHECK(junctions.front().first == 5);
    CHECK(junctions.front().second == 0);
}

TEST_CASE("A plain crossing earns no dot", "[schematic][junctions]") {
    Schematic s;
    s.addWire(0, 5, 10, 5);
    s.addWire(5, 0, 5, 10);

    CHECK(s.findJunctions().empty());
}

TEST_CASE("Three wire ends at one point earn a dot", "[schematic][junctions]") {
    Schematic s;
    s.addWire(0, 0, 5, 0);
    s.addWire(5, 0, 10, 0);
    s.addWire(5, 0, 5, 5);

    const auto junctions = s.findJunctions();
    CHECK(std::find(junctions.begin(), junctions.end(), std::make_pair(5, 0))
          != junctions.end());
}

// ============================================================================
// Persistence
// ============================================================================

TEST_CASE("A sheet round-trips through a document", "[schematic][document]") {
    Schematic s;
    const std::string srcId = s.addPart("src", 4, 10);
    const std::string resId = s.addPart("res", 12, 10);
    const std::string capId = s.addPart("cap", 20, 10);

    const auto srcPin = Schematic::pinPosition(*s.findPart(srcId), 0);
    const auto srcNeg = Schematic::pinPosition(*s.findPart(srcId), 1);
    const auto resPin0 = Schematic::pinPosition(*s.findPart(resId), 0);
    const auto resPin1 = Schematic::pinPosition(*s.findPart(resId), 1);
    const auto capPin0 = Schematic::pinPosition(*s.findPart(capId), 0);
    const auto capPin1 = Schematic::pinPosition(*s.findPart(capId), 1);

    s.addPart("in", srcPin.x - 2, srcPin.y);
    s.addPart("gnd", srcNeg.x, srcNeg.y + 2);
    s.addPart("gnd", capPin1.x, capPin1.y + 2);

    s.addWire(srcPin.x, srcPin.y, resPin0.x, resPin0.y);
    s.addWire(resPin1.x, resPin1.y, capPin0.x, capPin0.y);

    const auto doc = s.toDocument();

    Schematic reloaded;
    reloaded.fromDocument(doc);

    // Same number of parts, and their pins carry the same nodes -- which is the
    // property the converter reads.
    REQUIRE(reloaded.getParts().size() == s.getParts().size());
    CHECK(reloaded.findPart(srcId)->pins.at("pos") == "in");
    CHECK(reloaded.findPart(resId)->pins.at("b") == reloaded.findPart(capId)->pins.at("a"));
    CHECK(reloaded.findPart(capId)->pins.at("b") == "0");

    // And the wire survived as a wire.
    CHECK_FALSE(reloaded.getWires().empty());
}

TEST_CASE("toDocument names each wire's net the way extractNets does",
          "[schematic][document]") {
    Schematic s;
    const std::string aId = s.addPart("res", 10, 10);
    const std::string bId = s.addPart("res", 20, 10);

    const auto a = Schematic::pinPosition(*s.findPart(aId), 1);
    const auto b = Schematic::pinPosition(*s.findPart(bId), 0);
    s.addWire(a.x, a.y, b.x, b.y);

    const auto doc = s.toDocument();

    // The one net carries both wire segments; each segment is its own polyline,
    // so the diagonal L is written as two independent two-point wires.
    REQUIRE(doc.wires.size() == 2);
    CHECK_FALSE(doc.wires.front().net.empty());
    CHECK(doc.wires.front().net == doc.wires[1].net);
}

TEST_CASE("A wire that passes through a pin connects to it, and the file says so",
          "[schematic][document][file]") {
    Schematic s;
    const std::string resId = s.addPart("res", 12, 10);
    const std::string capId = s.addPart("cap", 20, 14);

    const auto resPin1 = Schematic::pinPosition(*s.findPart(resId), 1);
    const auto capPin1 = Schematic::pinPosition(*s.findPart(capId), 1);
    s.addPart("gnd", capPin1.x, capPin1.y + 2);

    // A wire from the resistor's far end, straight across the capacitor's
    // upper terminal -- the pin it passes over.
    s.addWire(resPin1.x, resPin1.y, 30, resPin1.y);

    // The capacitor's upper terminal is on the wire, so it is on the wire's net.
    const auto doc = s.toDocument();
    const auto nets = s.extractNets();

    const int resNet = nets.netOfPart[0][1];
    const int capNet = nets.netOfPart[1][0];
    CHECK(resNet == capNet);

    const std::string json = doc.toJson();
    wdf::LiveWDFDocument parsed;
    std::string error;
    REQUIRE(wdf::LiveWDFDocument::fromJson(json, parsed, error));

    Schematic reloaded;
    reloaded.fromDocument(parsed);

    CHECK(reloaded.findPart(resId)->pins.at("b")
          == reloaded.findPart(capId)->pins.at("a"));
    CHECK(reloaded.findPart(capId)->pins.at("b") == "0");
}

TEST_CASE("A sheet survives the file format: model -> JSON -> model",
          "[schematic][document][file]") {
    Schematic s;
    const std::string vsrcId = s.addPart("src", 4, 10);
    const std::string resId = s.addPart("res", 12, 10);
    const std::string capId = s.addPart("cap", 20, 14);

    // A value and a label, so the two string maps are exercised and not just the
    // pins.
    s.findPart(resId)->values["value"] = 4700.0;
    s.findPart(resId)->properties["label"] = "Rin";

    // Rotate one, so `visual` is not all defaults.
    s.findPart(capId)->visual.rotation = 1;

    const auto srcPin = Schematic::pinPosition(*s.findPart(vsrcId), 0);
    const auto srcNeg = Schematic::pinPosition(*s.findPart(vsrcId), 1);
    const auto resPin = Schematic::pinPosition(*s.findPart(resId), 0);
    const auto resPin1 = Schematic::pinPosition(*s.findPart(resId), 1);
    const auto capPin = Schematic::pinPosition(*s.findPart(capId), 0);
    const auto capNeg = Schematic::pinPosition(*s.findPart(capId), 1);

    s.addPart("in", srcPin.x - 2, srcPin.y);
    s.addPart("gnd", srcNeg.x, srcNeg.y + 2);
    s.addPart("gnd", capNeg.x, capNeg.y + 2);

    s.addWire(srcPin.x, srcPin.y, resPin.x, resPin.y);
    s.addWire(resPin1.x, resPin1.y, capPin.x, capPin.y);

    // Through the format, not just through the doc: the same text the Save
    // button would write, parsed by the same reader Open uses.
    const std::string json = s.toDocument().toJson();
    REQUIRE_FALSE(json.empty());

    wdf::LiveWDFDocument parsed;
    std::string error;
    REQUIRE(wdf::LiveWDFDocument::fromJson(json, parsed, error));

    Schematic reloaded;
    reloaded.fromDocument(parsed);

    // Same parts, and the pins still name the same nodes -- which is the property
    // the engine's converter actually reads.
    REQUIRE(reloaded.getParts().size() == s.getParts().size());
    CHECK(reloaded.findPart(resId)->pins.at("a") == "in");
    CHECK(reloaded.findPart(resId)->pins.at("b") == reloaded.findPart(capId)->pins.at("a"));
    CHECK(reloaded.findPart(capId)->pins.at("b") == "0");

    // The value, the property and the rotation came back too.
    CHECK(reloaded.findPart(resId)->values.at("value") == 4700.0);
    CHECK(reloaded.findPart(resId)->properties.at("label") == "Rin");
    CHECK(reloaded.findPart(capId)->visual.rotation == 1);

    CHECK(netOfPin(reloaded, resId, 1) == netOfPin(s, resId, 1));
    CHECK(netOfPin(reloaded, capId, 0) == netOfPin(s, capId, 0));
    CHECK(netOfPin(reloaded, vsrcId, 0) == netOfPin(s, vsrcId, 0));

    // Every pin still names a net: nothing was dropped or stranded by the trip.
    for (const auto& part : reloaded.getParts())
        for (const auto& [pin, net] : part.pins)
            CHECK_FALSE(net.empty());
}


TEST_CASE("Stale 0 on passive pin does not turn ungrounded net into ground",
          "[schematic][ground]") {
    Schematic s;
    const std::string r1 = s.addPart("res", 10, 10);
    const std::string pot1 = s.addPart("pot", 20, 10);

    // Stale "0" on pot high terminal, even though no GND component is on this net
    s.findPart(pot1)->pins["high"] = "0";

    const auto rPin = Schematic::pinPosition(*s.findPart(r1), 1);
    const auto potPin = Schematic::pinPosition(*s.findPart(pot1), 0);
    s.addWire(rPin.x, rPin.y, potPin.x, potPin.y);

    const auto nets = s.extractNets();
    const auto doc = s.toDocument();

    const int potHighNet = nets.netOfPart[1][0];
    REQUIRE(potHighNet >= 0);
    // Net must NOT be ground "0"
    CHECK(nets.netNames[static_cast<size_t>(potHighNet)] != "0");
    CHECK(doc.components[1].pins.at("high") != "0");
}

TEST_CASE("Duplicate generated net names across disconnected nets are deduplicated",
          "[schematic][dedup]") {
    Schematic s;
    const std::string r1 = s.addPart("res", 10, 10);
    const std::string r2 = s.addPart("res", 50, 50);

    // Both parts carry "n1" from an older file or paste
    s.findPart(r1)->pins["a"] = "n1";
    s.findPart(r2)->pins["a"] = "n1";

    const auto doc = s.toDocument();
    CHECK(doc.components[0].pins.at("a") != doc.components[1].pins.at("a"));
}
