#include "SymbolArtwork.h"

#include "format/LiveWDFFile.h"
#include "format/TransistorPolarity.h"

#include <BinaryData.h>

#include <array>
#include <cmath>
#include <map>
#include <memory>

namespace livewdf::editor {

namespace {

/// SVG units per grid square. Baked into the artwork, so it lives here and in
/// the header's convention note and nowhere else. (Celine's own constant.)
constexpr float unitsPerGrid = 10.0f;

/// The stroke width the artwork calls normal.
constexpr float baselineStroke = 1.2f;

/// Defined with the picking helpers below. Declared here because a shape records
/// whether it closes when it is extracted, which happens before those helpers in
/// the file.
bool isRegionPath(const juce::Path& path);

/// One slot per file. The *index* is a symbol's identity as far as the drawing is
/// concerned; the filename is needed once, when the file is parsed.
///
/// That split is the whole point, and Celine spells out why: the canvas asks for
/// artwork once per part per repaint, and answering by building a string and
/// hashing it into a map is three allocations per part per frame in the one
/// function that is supposed to allocate nothing. Order matches `assetNames`.
enum class Asset {
    Resistor,
    Capacitor,
    Inductor,
    Diode,
    Ground,
    Potentiometer,
    VariableResistor,
    VoltageSource,
    CurrentSource,
    GainBlock,
    BucketBrigade,
    Vca,
    Transformer,
    CenterTapTransformer,
    Triode,
    Pentode,
    Input,
    Output,
    ControlledResistor,
    Photoresistor,
    SpdtA,
    SpdtB,
    BjtNpn,
    BjtPnp,
    JfetN,
    JfetP,
    MosfetN,
    MosfetP,
    DcRail,
    ScopeProbe,
    OpAmp,
    IdealOpAmp,
    Ca3080,
    That2180,
    RmsDetector,
    Dbx200,
    count,
};

constexpr int assetCount = static_cast<int>(Asset::count);

const char* const assetNames[] = {
    "resistor",
    "capacitor",
    "inductor",
    "diode",
    "ground",
    "potentiometer",
    "variable-resistor",
    "voltage-source",
    "current-source",
    "gain-block",
    "bucket-brigade",
    "vca",
    "transformer",
    "transformer-ct",
    "triode",
    "pentode",
    "input",
    "output",
    "controlled-resistor",
    "photoresistor",
    "spdt-a",
    "spdt-b",
    "npn-bjt",
    "pnp-bjt",
    "n-jfet",
    "p-jfet",
    "n-mosfet",
    "p-mosfet",
    "rail",
    "scope",
    "opamp",
    "ideal-opamp",
    "ca3080",
    "that2180",
    "rms-detector",
    "dbx200",
};

static_assert(std::size(assetNames) == static_cast<size_t>(assetCount),
              "every Asset needs a filename, in the same order");

/// Maps our part `type` strings onto an asset. Returns `noAsset` for a type with
/// no file -- `scope`, our own addition, and any type whose symbol neither
/// Céline nor the transistor set draws.
///
/// `assetForType` answers with the **NPN/N-channel** file, which is what the
/// palette preview and an old part with no polarity information should show.
/// `artworkForPart` replaces it with the P variant from the card or the
/// explicit polarity property.
constexpr int noAsset = -1;

int assetForType(const std::string& type) {
    if (type == "res")         return static_cast<int>(Asset::Resistor);
    if (type == "cap")         return static_cast<int>(Asset::Capacitor);
    if (type == "ind")         return static_cast<int>(Asset::Inductor);
    if (type == "diode")       return static_cast<int>(Asset::Diode);
    if (type == "ground")      return static_cast<int>(Asset::Ground);
    if (type == "pot")         return static_cast<int>(Asset::Potentiometer);
    if (type == "varres")      return static_cast<int>(Asset::VariableResistor);
    if (type == "src")         return static_cast<int>(Asset::VoltageSource);
    if (type == "isrc")        return static_cast<int>(Asset::CurrentSource);
    if (type == "vcvs")        return static_cast<int>(Asset::GainBlock);
    if (type == "rmsdet" || type == "dbx209") return static_cast<int>(Asset::RmsDetector);
    if (type == "bbd")         return static_cast<int>(Asset::BucketBrigade);
    if (type == "vca")         return static_cast<int>(Asset::Vca);
    if (type == "dbx200")      return static_cast<int>(Asset::Dbx200);
    if (type == "that2180")     return static_cast<int>(Asset::That2180);
    if (type == "transformer") return static_cast<int>(Asset::Transformer);
    if (type == "transformer_ct") return static_cast<int>(Asset::CenterTapTransformer);
    if (type == "triode")      return static_cast<int>(Asset::Triode);
    if (type == "pentode")     return static_cast<int>(Asset::Pentode);
    if (type == "in")          return static_cast<int>(Asset::Input);
    if (type == "out")         return static_cast<int>(Asset::Output);
    if (type == "gnd")         return static_cast<int>(Asset::Ground);
    if (type == "ctrlres")     return static_cast<int>(Asset::ControlledResistor);
    if (type == "photores")    return static_cast<int>(Asset::Photoresistor);
    if (type == "switch_res")  return static_cast<int>(Asset::SpdtA);
    if (type == "bjt")         return static_cast<int>(Asset::BjtNpn);
    if (type == "jfet")        return static_cast<int>(Asset::JfetN);
    if (type == "mosfet")      return static_cast<int>(Asset::MosfetN);
    if (type == "rail")        return static_cast<int>(Asset::DcRail);
    if (type == "scope")       return static_cast<int>(Asset::ScopeProbe);
    if (type == "opamp")       return static_cast<int>(Asset::OpAmp);
    if (type == "idealopamp")  return static_cast<int>(Asset::IdealOpAmp);
    if (type == "ca3080")     return static_cast<int>(Asset::Ca3080);
    return noAsset;
}

/// The union of every shape's path, grown by half a stroke so the ink is inside
/// the box rather than straddling it.
///
/// Written out by hand rather than with `Rectangle::getUnion`, for the reason
/// Celine spells out: a horizontal line has zero height, JUCE counts a
/// zero-sized rectangle as empty, and `getUnion` answers with the *other*
/// operand whenever either side is empty -- so a symbol made of straight lines
/// would measure as one of them.
juce::Rectangle<float> measure(const SymbolArtwork& artwork) {
    bool first = true;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;

    for (const auto& shape : artwork.shapes) {
        if (shape.path.isEmpty())
            continue;

        // Half the *authored* weight, in grid units. The canvas strokes at a
        // floor that varies with zoom, so this is the shape's own idea of how fat
        // it is, not the view's.
        const auto half = shape.filled
                              ? 0.0f
                              : 0.5f * shape.strokeScale * baselineStroke / unitsPerGrid;

        const auto box = shape.path.getBounds().expanded(half);

        if (first) {
            minX = box.getX();
            minY = box.getY();
            maxX = box.getRight();
            maxY = box.getBottom();
            first = false;
            continue;
        }

        minX = juce::jmin(minX, box.getX());
        minY = juce::jmin(minY, box.getY());
        maxX = juce::jmax(maxX, box.getRight());
        maxY = juce::jmax(maxY, box.getBottom());
    }

    return first ? juce::Rectangle<float>()
                 : juce::Rectangle<float>(minX, minY, maxX - minX, maxY - minY);
}

/// Pulls the paths out of a parsed SVG, in grid units. Recursive because a
/// drawing program will nest things in groups even when the artwork is flat.
void collect(const juce::Drawable& drawable, SymbolArtwork& out,
             juce::AffineTransform accumulated) {
    // getDrawableTransform rather than getTransform: JUCE 9 made the latter
    // protected and virtual, and this one public. They return the same member.
    const auto here = drawable.getDrawableTransform().followedBy(accumulated);

    if (const auto* shape = dynamic_cast<const juce::DrawablePath*>(&drawable)) {
        SymbolArtwork::Shape s;
        s.path = shape->getPath();
        s.path.applyTransform(here.scaled(1.0f / unitsPerGrid, 1.0f / unitsPerGrid));

        const auto stroke = shape->getStrokeType().getStrokeThickness();

        // No stroke means the path is a filled one -- a diode's triangle, an
        // arrowhead, a wiper's head.
        s.filled = stroke <= 0.0f;
        s.strokeScale = s.filled ? 1.0f : stroke / baselineStroke;
        s.closed = isRegionPath(s.path);

        out.shapes.push_back(std::move(s));
    }

    // Children by index. JUCE 9 stopped deriving Drawable from Component, so a
    // composite no longer has Component::getChildren() and there is nothing to
    // downcast -- getChild() hands back a Drawable already.
    if (const auto* composite = dynamic_cast<const juce::DrawableComposite*>(&drawable))
        for (int i = 0; i < composite->getNumChildren(); ++i)
            collect(composite->getChild(i), out, here);
}

/// The viewBox origin, which the SVG parser normalises away.
///
/// JUCE 9's LunaSVG-based parser maps the viewBox onto a viewport starting at
/// (0,0), so for our `viewBox="-60 -60 120 120"` every path comes back carrying a
/// translation of (+60,+60) and the artwork lands in the first quadrant instead
/// of straddling the origin -- every symbol six grid squares down and right of
/// where its pins are drawn. Undone here rather than compensated for further
/// down, because the convention is that (0,0) is the part's centre and that has
/// to be true of the paths themselves.
///
/// Read from the file rather than assumed, even though the convention fixes it at
/// -60: a hardcoded 60 would silently mis-place any artwork that used a different
/// box -- exactly the failure the convention exists to prevent -- and do it
/// without a compiler or a test saying anything.
juce::Point<float> viewBoxOrigin(const void* data, int size) {
    const auto text = juce::String::createStringFromData(data, size);

    if (const auto xml = juce::XmlDocument::parse(text)) {
        auto numbers =
            juce::StringArray::fromTokens(xml->getStringAttribute("viewBox").trim(), " ,", "");
        numbers.removeEmptyStrings();

        if (numbers.size() >= 2)
            return {numbers[0].getFloatValue(), numbers[1].getFloatValue()};
    }

    return {};
}

/// Loads one asset by *filename*, or leaves it empty if there is not one.
///
/// Matched on the original filename rather than on the identifier JUCE derives
/// from it. That derivation is not the obvious one -- "variable-resistor.svg"
/// becomes `variableresistor_svg`, with the hyphen dropped rather than turned
/// into an underscore -- and guessing it wrong is silent: the part simply draws
/// as nothing but its pins. Asking for the filename means the rule never has to
/// be guessed, and "drop a file in with this name" is the whole contract.
SymbolArtwork load(const juce::String& name) {
    SymbolArtwork artwork;

    const auto wanted = name + ".svg";

    for (int i = 0; i < ArtworkData::namedResourceListSize; ++i) {
        const auto* resource = ArtworkData::namedResourceList[i];

        if (wanted != ArtworkData::getNamedResourceOriginalFilename(resource))
            continue;

        int size = 0;

        if (const auto* data = ArtworkData::getNamedResource(resource, size);
            data != nullptr && size > 0)
            if (const auto drawable = juce::Drawable::createFromImageData(data, static_cast<size_t>(size))) {
                // Seeded with the viewBox origin, so the parser's normalisation
                // is cancelled before anything is cached.
                const auto origin = viewBoxOrigin(data, size);
                collect(*drawable, artwork, juce::AffineTransform::translation(origin.x, origin.y));
            }

        break;
    }

    // Measured here, with the paths, so nothing can hold artwork whose bounds
    // were never taken.
    artwork.bounds = measure(artwork);

    return artwork;
}

/// The parsed artwork, one slot per asset.
///
/// A `DeletedAtShutdown` rather than a plain function-local static, for the reason
/// Celine gives: the paths inside are leak-counted JUCE objects, and a static
/// holding them outlives the detector that counts them -- a permanent false
/// "Leaked objects detected" at the end of every test run, which is worse than no
/// leak checking at all because it trains you to ignore the one that matters.
struct ArtworkCache final : public juce::DeletedAtShutdown {
    ~ArtworkCache() override { instance = nullptr; }

    std::array<std::unique_ptr<SymbolArtwork>, static_cast<size_t>(assetCount)> store;
    juce::CriticalSection lock;

    static ArtworkCache* instance;
};

ArtworkCache* ArtworkCache::instance = nullptr;

/// Parsed once each, on first use, and found again by index.
const SymbolArtwork& cached(int asset) {
    if (ArtworkCache::instance == nullptr)
        ArtworkCache::instance = new ArtworkCache();

    auto& cache = *ArtworkCache::instance;
    const juce::ScopedLock guard(cache.lock);

    jassert(juce::isPositiveAndBelow(asset, assetCount));
    auto& slot = cache.store[static_cast<size_t>(asset)];

    if (slot == nullptr)
        slot = std::make_unique<SymbolArtwork>(load(assetNames[asset]));

    return *slot;
}

// ============================================================================
// Shape-accurate picking
// ============================================================================

/// Curves are flattened this finely when a click or marquee is asked what it
/// landed on. The artwork is authored in grid units, so this is a hundredth of a
/// grid square: below a pixel at every zoom the canvas allows, while still
/// cheap enough to run over one symbol's handful of paths.
constexpr float kFlattenTolerance = 0.01f;

/// A path is a region when it was filled, or when every one of its sub-paths is
/// closed. A resistor's rectangle is closed and is picked throughout its body; a
/// capacitor plate is only a line and is picked along its ink.
///
/// `Path::contains` is documented as only meaningful for closed paths, so this
/// guard matters: asking it about a transformer's open winding would fill the
/// area under the curve and turn empty space into a hit target.
bool isRegionPath(const juce::Path& path) {
    int subPaths = 0;
    int closedSubPaths = 0;

    juce::PathFlatteningIterator it(path, {}, kFlattenTolerance);
    while (it.next()) {
        if (it.subPathIndex == 0)
            ++subPaths;
        if (it.closesSubPath)
            ++closedSubPaths;
    }

    return subPaths > 0 && closedSubPaths == subPaths;
}

float distanceSquaredToSegment(float px, float py, float x1, float y1, float x2, float y2) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float lengthSquared = dx * dx + dy * dy;

    float t = 0.0f;
    if (lengthSquared > 0.0f)
        t = juce::jlimit(0.0f, 1.0f, ((px - x1) * dx + (py - y1) * dy) / lengthSquared);

    const float nx = px - (x1 + t * dx);
    const float ny = py - (y1 + t * dy);
    return nx * nx + ny * ny;
}

bool shapeHitTest(const SymbolArtwork::Shape& shape, const juce::AffineTransform& transform,
                  float px, float py, float reach) {
    // Quick reject against the shape's transformed box before flattening it.
    // `partAtPixel` runs on mouse-move, so the common case -- the pointer is
    // nowhere near this path -- must not pay for a curve walk.
    const auto bounds = shape.path.getBoundsTransformed(transform)
                            .expanded(reach + kFlattenTolerance);
    if (!bounds.contains(px, py))
        return false;

    const bool region = shape.filled || shape.closed;

    if (region) {
        juce::Path placed;
        placed.addPath(shape.path, transform);
        if (placed.contains(px, py, kFlattenTolerance))
            return true;
    }

    const float reachSquared = reach * reach;

    juce::PathFlatteningIterator it(shape.path, transform, kFlattenTolerance);
    while (it.next())
        if (distanceSquaredToSegment(px, py, it.x1, it.y1, it.x2, it.y2) <= reachSquared)
            return true;

    return false;
}

bool shapeIntersectsBox(const SymbolArtwork::Shape& shape,
                        const juce::AffineTransform& transform, juce::Rectangle<float> box) {
    const auto bounds = shape.path.getBoundsTransformed(transform);

    // Nothing this shape can touch is in the box.
    if (!bounds.expanded(kFlattenTolerance).intersects(box))
        return false;

    // Wholly inside the marquee? Then every part of the shape is inside it too.
    if (box.contains(bounds))
        return true;

    const bool region = shape.filled || shape.closed;

    if (region) {
        juce::Path placed;
        placed.addPath(shape.path, transform);

        // A box drawn entirely inside a filled body still covers ink -- the
        // body's interior. Testing the corners as well as the centre also
        // catches a box that spans a concave part of the region.
        if (placed.contains(box.getCentre(), kFlattenTolerance)
            || placed.contains(box.getTopLeft(), kFlattenTolerance)
            || placed.contains(box.getTopRight(), kFlattenTolerance)
            || placed.contains(box.getBottomLeft(), kFlattenTolerance)
            || placed.contains(box.getBottomRight(), kFlattenTolerance))
            return true;
    }

    juce::PathFlatteningIterator it(shape.path, transform, kFlattenTolerance);
    while (it.next()) {
        if (box.contains(it.x1, it.y1) || box.contains(it.x2, it.y2))
            return true;

        if (box.intersects(juce::Line<float>(it.x1, it.y1, it.x2, it.y2)))
            return true;
    }

    return false;
}

}  // namespace

const SymbolArtwork* artworkForType(const std::string& type) {
    const int asset = assetForType(type);

    return asset == noAsset ? nullptr : &cached(asset);
}

const SymbolArtwork* artworkForPart(const wdf::SchematicComponent& part) {
    if (part.type == "switch_res")
        return &cached(wdf::switchResSelectsB(part) ? static_cast<int>(Asset::SpdtB)
                                                    : static_cast<int>(Asset::SpdtA));

    if (part.type == "bjt") {
        const bool pnp = wdf::transistorPolarity(part) ==
                         wdf::TransistorPolarity::PType;
        return &cached(static_cast<int>(pnp ? Asset::BjtPnp : Asset::BjtNpn));
    }
    if (part.type == "jfet") {
        const bool pChannel = wdf::transistorPolarity(part) ==
                              wdf::TransistorPolarity::PType;
        return &cached(static_cast<int>(pChannel ? Asset::JfetP : Asset::JfetN));
    }
    if (part.type == "mosfet") {
        const bool pChannel = wdf::transistorPolarity(part) ==
                              wdf::TransistorPolarity::PType;
        return &cached(static_cast<int>(pChannel ? Asset::MosfetP : Asset::MosfetN));
    }

    return artworkForType(part.type);
}

juce::AffineTransform artworkTransformFor(const wdf::SchematicComponent& part,
                                          juce::Point<float> centre, float scale) {
    const int quarterTurns = (part.visual.rotation / 90) & 3;

    // Mirror, then rotate, then move -- the order `pinPosition` and
    // `drawArtworkAt` both use. Stated once here so a hit test and the picture
    // cannot drift apart about where the symbol is.
    return juce::AffineTransform::scale(part.visual.flipped ? -scale : scale, scale)
        .rotated(static_cast<float>(quarterTurns) * juce::MathConstants<float>::halfPi)
        .translated(centre.x, centre.y);
}

bool artworkHitTest(const SymbolArtwork& artwork, const wdf::SchematicComponent& part,
                    float px, float py, float reach) {
    const auto transform = artworkTransformFor(
        part,
        {static_cast<float>(std::lround(part.visual.x)),
         static_cast<float>(std::lround(part.visual.y))},
        1.0f);

    for (const auto& shape : artwork.shapes)
        if (shapeHitTest(shape, transform, px, py, reach))
            return true;

    return false;
}

bool artworkIntersectsBox(const SymbolArtwork& artwork, const wdf::SchematicComponent& part,
                          juce::Rectangle<float> box) {
    if (box.isEmpty())
        return false;

    const auto transform = artworkTransformFor(
        part,
        {static_cast<float>(std::lround(part.visual.x)),
         static_cast<float>(std::lround(part.visual.y))},
        1.0f);

    for (const auto& shape : artwork.shapes)
        if (shapeIntersectsBox(shape, transform, box))
            return true;

    return false;
}

bool hasArtwork(const std::string& type) {
    return assetForType(type) != noAsset;
}

std::vector<std::string> artworkTypes() {
    std::vector<std::string> types;

    // Walked by asking each palette type whether it has artwork, rather than by
    // listing the asset names again -- so the list cannot drift from the map, and
    // a type added to one but not the other is a test failure, not a blank part.
    for (const auto* name : {"res", "cap", "ind", "diode", "ground", "gnd",
                             "pot", "varres", "src", "isrc", "vcvs", "bbd", "rmsdet", "dbx209", "dbx200",
                             "transformer", "triode", "pentode", "ctrlres", "photores",
                             "switch_res", "bjt", "jfet", "mosfet",
                             "in", "out", "opamp", "ca3080"})
        if (hasArtwork(name))
            types.emplace_back(name);

    return types;
}

float artworkScaleFor(const SymbolArtwork& artwork) {
    // 1:1, in grid units, with no scale step.
    //
    // This used to grow the file to cover a ±22/±26 body, because our pins were
    // five times Celine's own ±2 and the artwork was authored for Celine's frame.
    // Now that the pins are ±3/±3.5 (see `Schematic::pinOffsets`), the artwork's
    // own grid frame *is* the sheet's frame: 10 SVG units = 1 grid square means a
    // pin drawn at SVG (0,-35) lands on a pin at grid (0,-3.5), one to one. The
    // function stays as the single place a caller asks, so a part that ever needs
    // a different frame has somewhere to say so -- but every current file is 1:1.
    juce::ignoreUnused(artwork);
    return 1.0f;
}

}  // namespace livewdf::editor
