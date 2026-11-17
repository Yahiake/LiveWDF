// Measures what a symbol actually *draws*, and holds it to where the model says
// it should be (U3.5).
//
// **The bug this exists for.** Every symbol on the sheet was drawn ten times too
// small, and then -- once that was fixed -- as a solid block. Neither showed up
// in the geometry: the artwork's bounds were right, the shape count was right,
// the parity test against Céline's measured values stayed green. What was wrong
// was the *placement* -- the transform that turns the artwork's grid units into
// pixels -- and no test was looking at the pixels.
//
// So this test looks at the pixels. It draws a symbol into an image and
// measures the ink: where it is, and how big it is. The artwork's own geometry
// is known (`SymbolArtwork::bounds`, in grid units), so the ink's size and
// position are both predictable, and both are checkable.
//
// **What it holds.** The ink's width and height match the artwork's bounds
// scaled by the zoom; the ink is centred where the part is. That catches a
// scale factor that is wrong (ten times out, as it was), a stroke that is
// scaled by the transform (which fills a symbol in as a solid block), and a
// placement that is off-centre.
//
// **What it cannot hold.** Whether the symbol is the *right* symbol -- that is
// the parity test's job, and it compares against Céline's measured geometry.
// Neither test can say the symbol is handsome; the rendered PNG is for that,
// and the ASCII dump in `tools/render` is what let the ten-times-out bug be
// seen at all.

#include "celine_reference_geometry.h"

#include "EditorLookAndFeel.h"
#include "SchematicModel.h"
#include "SymbolArtwork.h"

#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace livewdf::editor;

namespace {

/// The ground the symbol is drawn on. It is Céline's window ground, so "is
/// there ink here" is a question about the drawing rather than about the
/// palette.
const juce::Colour kGround{0xff17151a};

/// The colour a symbol is drawn in: Céline's part colour.
const juce::Colour kInk{0xffd9d9d9};

/// The zoom the symbols are measured at: big enough that a pixel of error is
/// small, small enough that the whole symbol fits in a modest image.
constexpr float kGridSize = 26.0f;
constexpr int kImageSize = 320;
constexpr float kStroke = 3.12f;   // gridSize * 0.12, the sheet's own rule

/// Where the ink landed, in pixels. `empty` means nothing was drawn at all --
/// which is a failure in itself, and one this test has to be able to say.
struct InkBox {
    /// The ink's extent. `minX`/`minY` start *outside* the image, not at zero --
    /// a box that starts at zero says "there is ink in the corner" whether or not
    /// there is, which is a bug this file made once.
    int minX = 1 << 30, minY = 1 << 30, maxX = -1, maxY = -1;
    /// How many pixels carry ink at all, inside the whole image.
    int inkedPixels = 0;
    bool empty() const noexcept { return maxX < 0; }
    int width() const noexcept { return empty() ? 0 : maxX - minX + 1; }
    int height() const noexcept { return empty() ? 0 : maxY - minY + 1; }
    float centreX() const noexcept { return (minX + maxX) * 0.5f; }
    float centreY() const noexcept { return (minY + maxY) * 0.5f; }
};

/// Draws `type`'s artwork into a fresh image and measures the ink.
InkBox measureInk(const std::string& type) {
    juce::Image image(juce::Image::ARGB, kImageSize, kImageSize, true);
    {
        juce::Graphics g(image);
        g.fillAll(kGround);
        drawSymbolArtwork(g, type, {kImageSize * 0.5f, kImageSize * 0.5f},
                          kGridSize, kStroke);
    }

    InkBox box;
    for (int y = 0; y < kImageSize; ++y)
        for (int x = 0; x < kImageSize; ++x)
        {
            const auto p = image.getPixelAt(x, y);
            const int diff = std::abs(static_cast<int>(p.getRed())   - kGround.getRed())
                           + std::abs(static_cast<int>(p.getGreen()) - kGround.getGreen())
                           + std::abs(static_cast<int>(p.getBlue())  - kGround.getBlue());
            if (diff > 40)
            {
                box.minX = std::min(box.minX, x);
                box.minY = std::min(box.minY, y);
                box.maxX = std::max(box.maxX, x);
                box.maxY = std::max(box.maxY, y);
                ++box.inkedPixels;
            }
        }
    return box;
}

/// Where the pot's `H` or `L` should be, in pixels. `pinIndex` is the palette
/// order (0 = high, 2 = low). This mirrors the drawing transform: mirror, then
/// rotate, with the letter between the pin and the body.
juce::Point<float> potLetterCentre(const wdf::SchematicComponent& part, int pinIndex,
                                   const SheetView& view) {
    const auto offsets = Schematic::pinOffsets(part.type);
    const auto pin = offsets[static_cast<size_t>(pinIndex)];

    float x = -0.85f;
    float y = static_cast<float>(pin.y) + (pin.y < 0 ? 0.35f : -0.35f);

    if (part.visual.flipped)
        x = -x;

    for (int turn = 0; turn < ((part.visual.rotation / 90) & 3); ++turn) {
        const float rx = -y;
        y = x;
        x = rx;
    }

    return view.toPixel(part.visual.x + x, part.visual.y + y);
}

/// Ink in a small box round `centre`. The pot's body, leads and pin dots are
/// outside these boxes, so this is the terminal letter and nothing else.
int inkRound(const juce::Image& image, juce::Point<float> centre, int radius = 11) {
    int inked = 0;
    const int x0 = std::max(0, static_cast<int>(std::lround(centre.x)) - radius);
    const int y0 = std::max(0, static_cast<int>(std::lround(centre.y)) - radius);
    const int x1 = std::min(image.getWidth() - 1, x0 + radius * 2);
    const int y1 = std::min(image.getHeight() - 1, y0 + radius * 2);

    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const auto p = image.getPixelAt(x, y);
            const int diff = std::abs(static_cast<int>(p.getRed())   - kGround.getRed())
                           + std::abs(static_cast<int>(p.getGreen()) - kGround.getGreen())
                           + std::abs(static_cast<int>(p.getBlue())  - kGround.getBlue());
            if (diff > 40)
                ++inked;
        }
    return inked;
}

juce::Image renderPotSymbol(const wdf::SchematicComponent& pot, const SheetView& view) {
    juce::Image image(juce::Image::ARGB, kImageSize, kImageSize, true);
    juce::Graphics g(image);
    g.fillAll(kGround);
    drawComponentSymbolOnly(g, pot, view);
    return image;
}

}  // namespace

TEST_CASE("A symbol is drawn at the size its geometry says it is") {
    // The artwork's bounds are in grid units; the sheet draws at `kGridSize`
    // pixels per grid square; so the ink should be that many times bigger.
    //
    // A tolerance of a few pixels covers the stroke's own width at each edge
    // and the rounding of a path's end points. It does not cover being ten
    // times out, or being filled in as a block -- both of which are hundreds of
    // pixels of difference, which is what this test was written for.
    for (const auto& ref : refgeom::table()) {
        INFO("artwork: " << ref.artworkKey);

        const auto* art = artworkForType(ref.artworkKey);
        REQUIRE(art != nullptr);

        const auto ink = measureInk(ref.artworkKey);
        REQUIRE_FALSE(ink.empty());

        const float expectedW = art->bounds.getWidth()  * kGridSize;
        const float expectedH = art->bounds.getHeight() * kGridSize;

        INFO("ink box " << ink.minX << ".." << ink.maxX
             << " x " << ink.minY << ".." << ink.maxY
             << " expected " << expectedW << " x " << expectedH);

        CHECK(std::abs(ink.width()  - expectedW) <= 6.0f);
        CHECK(std::abs(ink.height() - expectedH) <= 6.0f);
    }
}

TEST_CASE("A symbol is drawn where the part is") {
    // The ink must land on the artwork's own bounds: inside them, and not
    // outside them by more than a line's width.
    //
    // **Why the tolerance and not an exact match.** The bounds are the shapes'
    // boxes grown by half a stroke, which covers the stroke's width but not its
    // *caps*: a horizontal line with a round cap puts ink half a stroke further
    // along than the path reaches. An inductor is three horizontal arcs, so its
    // ink really does stick out past its own bounds by a little. The allowance
    // is one line width, which covers that and nothing else.
    //
    // An exact centre would be the wrong test anyway: the bounds' centre is the
    // *shapes'* centre, and a symbol whose artwork is not centred on its own
    // origin (which is a thing files are allowed to be) would fail it.
    const float slack = kStroke + 2.0f;

    for (const auto& ref : refgeom::table()) {
        INFO("artwork: " << ref.artworkKey);

        const auto* art = artworkForType(ref.artworkKey);
        REQUIRE(art != nullptr);

        const auto ink = measureInk(ref.artworkKey);
        REQUIRE_FALSE(ink.empty());

        const float expectedLeft   = kImageSize * 0.5f + art->bounds.getX() * kGridSize;
        const float expectedTop    = kImageSize * 0.5f + art->bounds.getY() * kGridSize;
        const float expectedRight  = expectedLeft + art->bounds.getWidth()  * kGridSize;
        const float expectedBottom = expectedTop  + art->bounds.getHeight() * kGridSize;

        INFO("ink box " << ink.minX << ".." << ink.maxX
             << " x " << ink.minY << ".." << ink.maxY
             << " expected " << expectedLeft << ".." << expectedRight
             << " x " << expectedTop << ".." << expectedBottom);

        // Inside: the ink covers the artwork it drew.
        CHECK(static_cast<float>(ink.minX) <= expectedLeft + slack);
        CHECK(static_cast<float>(ink.maxX) >= expectedRight - slack);
        CHECK(static_cast<float>(ink.minY) <= expectedTop + slack);
        CHECK(static_cast<float>(ink.maxY) >= expectedBottom - slack);

        // And not a long way outside it: a symbol drawn in the wrong place
        // would satisfy the checks above only if it were also bigger, and the
        // size test would have caught that. This is the belt to that braces.
        CHECK(static_cast<float>(ink.minX) >= expectedLeft - slack);
        CHECK(static_cast<float>(ink.maxX) <= expectedRight + slack);
        CHECK(static_cast<float>(ink.minY) >= expectedTop - slack);
        CHECK(static_cast<float>(ink.maxY) <= expectedBottom + slack);
    }
}

TEST_CASE("A symbol's ink is a shape, not a filled block") {
    // The stroke width is stated in *pixels* and must not be scaled by the
    // transform. JUCE 9 scales a stroke that is drawn through a scaled graphics
    // transform, which filled every symbol in as a solid rectangle: the ink's
    // area was a large fraction of its bounding box instead of a thin outline.
    //
    // The fraction is measured against the *outline* the artwork describes: a
    // symbol's shapes are strokes, so its ink is a fraction of the area its
    // bounds enclose. A filled block is nearly all of it.
    for (const auto& ref : refgeom::table()) {
        INFO("artwork: " << ref.artworkKey);

        const auto* art = artworkForType(ref.artworkKey);
        REQUIRE(art != nullptr);

        const auto ink = measureInk(ref.artworkKey);
        REQUIRE_FALSE(ink.empty());

        const float boxArea = static_cast<float>(ink.width()) * static_cast<float>(ink.height());
        REQUIRE(boxArea > 0.0f);

        const float coverage = static_cast<float>(ink.inkedPixels) / boxArea;

        INFO("coverage " << coverage << " of a " << ink.width() << "x" << ink.height()
             << " box, " << ink.inkedPixels << " inked pixels");

        // A stroked symbol covers well under half its own bounding box. The
        // bound is generous because a small symbol's few strokes can be a large
        // fraction of a small box -- but a filled block is nearly all of it.
        CHECK(coverage < 0.75f);
    }
}

TEST_CASE("Every part can be drawn as a placed component, caption and all") {
    // **This is the "click on it" path.** The canvas draws a part with its body,
    // its pins and its caption, and the caption is where a part's *identity*
    // lives -- for a model-bearing part it is the model name, not a number. So
    // a part that cannot be drawn with its caption cannot be looked at, which is
    // the same as a part that is not there.
    //
    // Every palette type is drawn here, because the failure is type-specific:
    // the parts whose default value is zero draw their value in the "unset"
    // colour, and the parts with a model draw the model name instead of a
    // number, and both are paths the artwork-only test never touches.
    for (const auto& spec : partPalette()) {
        INFO("type: " << spec.type);

        const auto comp = makeComponent(spec.type, {}, 0.0f, 0.0f);
        REQUIRE_FALSE(comp.type.empty());

        juce::Image image(juce::Image::ARGB, 400, 400, true);
        {
            juce::Graphics g(image);
            g.fillAll(kGround);

            SheetView view;
            view.gridSize = 26.0f;
            view.origin = {200.0f, 200.0f};

            // The whole part, exactly as the canvas draws it.
            drawComponent(g, comp, view);
        }

        // Something must have been drawn: a part that draws nothing is a part
        // the user cannot see, whatever the reason.
        int inked = 0;
        for (int y = 0; y < 400; ++y)
            for (int x = 0; x < 400; ++x)
            {
                const auto p = image.getPixelAt(x, y);
                const int diff = std::abs(static_cast<int>(p.getRed())   - kGround.getRed())
                               + std::abs(static_cast<int>(p.getGreen()) - kGround.getGreen())
                               + std::abs(static_cast<int>(p.getBlue())  - kGround.getBlue());
                if (diff > 40)
                    ++inked;
            }

        INFO("inked pixels: " << inked);
        CHECK(inked > 0);
    }
}

TEST_CASE("A pot's hidden terminal letters follow the drawn pins") {
    // A pot can be end-for-end without looking any different, because the body
    // is symmetric. The H/L letters are the drawing's own statement of which
    // end is which. They must move with the pin transform, not stay in file
    // order, or the drawing would disagree with the pins it labels.
    SheetView view;
    view.gridSize = kGridSize;
    view.origin = {kImageSize * 0.5f, kImageSize * 0.5f};

    // A new pot is placed high-up/low-down. At each letter's pin position the
    // drawing must have ink; then the same pot is explicitly turned end-for-end,
    // and the same screen positions must carry the other letter.
    auto pot = makeComponent("pot", {}, 0.0f, 0.0f);
    const auto topPoint = potLetterCentre(pot, 0, view);    // high is on top
    const auto bottomPoint = potLetterCentre(pot, 2, view); // low is below
    const auto defaultImage = renderPotSymbol(pot, view);
    const auto defaultTop = inkRound(defaultImage, topPoint);
    const auto defaultBottom = inkRound(defaultImage, bottomPoint);

    INFO("default ink: top " << defaultTop << ", bottom " << defaultBottom);
    CHECK(defaultTop > 0);
    CHECK(defaultBottom > 0);

    pot.visual.rotation = 180;
    pot.visual.flipped = true;
    const auto turnedImage = renderPotSymbol(pot, view);
    const auto turnedTop = inkRound(turnedImage, topPoint);
    const auto turnedBottom = inkRound(turnedImage, bottomPoint);

    INFO("turned ink: top " << turnedTop << ", bottom " << turnedBottom);
    CHECK(turnedTop > 0);
    CHECK(turnedBottom > 0);

    // The same screen position carries `H` on one placement and `L` on the
    // other. If the letters were fixed to file order rather than the pins, the
    // two images would have the same ink there.
    CHECK(defaultTop != turnedTop);
    CHECK(defaultBottom != turnedBottom);
}

TEST_CASE("A palette swatch shows the symbol, fitted to its row") {
    // The palette draws each part's symbol in a square swatch, fitted to the
    // symbol's own reach. It goes through the same artwork path as the sheet, so
    // it had the same ten-times-out fault -- and a swatch ten times too small is
    // a smudge, which is what a palette row that cannot be read actually looks
    // like.
    //
    // What is checked: that the swatch has ink in it at all, that the ink
    // *fills* the swatch (a symbol fitted to the swatch reaches most of the way
    // across it), and that it does not spill outside it.
    constexpr int kSwatch = 64;

    for (const auto& ref : refgeom::table()) {
        INFO("artwork: " << ref.artworkKey);

        juce::Image image(juce::Image::ARGB, kSwatch, kSwatch, true);
        {
            juce::Graphics g(image);
            g.fillAll(kGround);
            drawPreviewSymbol(g, ref.artworkKey,
                              {0.0f, 0.0f, static_cast<float>(kSwatch),
                                          static_cast<float>(kSwatch)},
                              kInk);
        }

        int inked = 0;
        int minX = kSwatch, minY = kSwatch, maxX = -1, maxY = -1;
        for (int y = 0; y < kSwatch; ++y)
            for (int x = 0; x < kSwatch; ++x)
            {
                const auto p = image.getPixelAt(x, y);
                const int diff = std::abs(static_cast<int>(p.getRed())   - kGround.getRed())
                               + std::abs(static_cast<int>(p.getGreen()) - kGround.getGreen())
                               + std::abs(static_cast<int>(p.getBlue())  - kGround.getBlue());
                if (diff > 40)
                {
                    ++inked;
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                }
            }

        INFO("swatch ink " << inked << " pixels, box " << minX << ".." << maxX
             << " x " << minY << ".." << maxY);

        REQUIRE(inked > 0);

        // It is drawn at a sensible *size*. The preview fits the symbol's reach
        // into the swatch, so a symbol's ink should span a good part of it --
        // a tenth of it is a smudge, which is what a swatch ten times too small
        // looks like, and what this test was written for.
        //
        // The measure is the ink's larger dimension rather than its area: a
        // symbol is mostly outline, and how much of its box it fills depends on
        // how many shapes it has. The ground is one thin glyph in a small
        // extent, and the transformer is a dense box; their areas differ by an
        // order of magnitude and their sizes do not.
        const int longSide = std::max(maxX - minX, maxY - minY) + 1;
        CHECK(static_cast<float>(longSide) / static_cast<float>(kSwatch) > 0.2f);

        // And it stays inside it. The preview clips to the swatch, so this is
        // about the *shape* being cut off, which is what a symbol too large for
        // its row looks like.
        CHECK(minX >= 0);
        CHECK(minY >= 0);
        CHECK(maxX < kSwatch);
        CHECK(maxY < kSwatch);
    }
}
