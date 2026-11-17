// ==============================================================================
// Renders one schematic to a PNG so it can be looked at.
//
// This is the missing half of verification. `ctest` proves invariants hold and
// says nothing about whether a symbol reads as a symbol; this dumps a picture,
// and a picture is the only thing that answers "does it look right".
//
// The same program builds both sides. LIVEWDF_RENDER_CELINE picks which: with it
// the sheet is drawn through Celine's own SchematicCanvas, without it through
// ours. The two are meant to be compared by eye, which is why the sheet, the zoom
// and the output size are identical either way -- that is the whole point of the
// harness being one file rather than two.
//
// Usage:  <tool> <output.png> [--strip]
//         <tool> --dump [--strip]
//         <tool> --ascii <type> [--diag]
//   default  draws a small fixed sheet (a divider and a few parts)
//   --strip  draws the parts in a grid, for symbol-by-symbol comparison
//   --dump   prints the symbol geometry as text instead of drawing it. This is
//            the machine-checkable half: the PNG is for a human to look at, the
//            dump is for comparing Céline's geometry against ours numerically,
//            because nobody can diff two pictures by reading them.
//   --ascii  prints ONE symbol as a grid of characters, so its shape can be read
//            without an image viewer. This is what found the ten-times-too-small
//            bug the PNG had been hiding: both sides can be printed and the two
//            blocks of characters compared line for line.
//   --diag   prints the loaded artwork's shapes to stderr, for when a symbol
//            comes out wrong and the question is what was loaded.
// ==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdio>
#include <cstdlib>
#include <functional>

#if LIVEWDF_RENDER_CELINE
    #include "Schematic.h"
    #include "SchematicCanvas.h"
    #include "SchematicSymbols.h"
    #include "SymbolArtwork.h"
    namespace ref = SchematicModel;
#else
    #include "SchematicEditor.h"
    #include "SchematicModel.h"
    #include "SymbolArtwork.h"
#endif

namespace
{
    constexpr int kWidth = 560;
    constexpr int kHeight = 400;

    juce::File outputFile;
    bool stripMode = false;
    bool dumpMode = false;

    /// `--file <path>`: render a `.livewdf` the user drew, rather than one of
    /// the canned sheets.
    bool fileMode = false;
    juce::String filePath;
    bool asciiMode = false;
    bool diagnostics = false;
    juce::String asciiType;
    /// The empty-sheet colour the ASCII map is measured against.
    const juce::Colour ground{0xff17151a};

    //==========================================================================
#if LIVEWDF_RENDER_CELINE

    /** The reference sheet: a voltage divider with a capacitor across the lower
        leg. Wires, junctions, a ground and a part carrying a value -- everything
        a symbol has to draw. */
    void buildSheet(ref::Schematic& sheet)
    {
        using T = ref::ElementType;

        const int rTop = sheet.addElement(T::Resistor, 8, 6);
        const int rBot = sheet.addElement(T::Resistor, 8, 12);
        sheet.addElement(T::Ground, 8, 17);
        const int cap = sheet.addElement(T::Capacitor, 14, 12);
        sheet.addElement(T::VoltageSource, 2, 6);

        if (auto* e = sheet.findElement(rTop)) e->value = 100000.0;
        if (auto* e = sheet.findElement(rBot)) e->value = 47000.0;
        if (auto* e = sheet.findElement(cap))  e->value = 1.0e-7;

        sheet.addWire({2, 6}, {8, 6});
        sheet.addWire({8, 8}, {8, 10});
        sheet.addWire({8, 14}, {8, 17});
        sheet.addWire({8, 6}, {14, 6});
        sheet.addWire({14, 6}, {14, 10});
        sheet.addWire({14, 14}, {14, 17});
        sheet.addWire({8, 17}, {14, 17});
        sheet.addWire({10, 12}, {12, 12});
    }

    /** One of every type we have artwork for, in a grid. */
    void buildStrip(ref::Schematic& sheet)
    {
        using T = ref::ElementType;
        const T types[] = {
            T::Ground, T::Input, T::Output, T::Resistor, T::Capacitor, T::Inductor,
            T::Potentiometer, T::VoltageSource, T::Diode, T::Triode, T::Pentode,
            T::Transformer,
        };

        constexpr int perRow = 4;
        int i = 0;
        for (const auto type : types)
        {
            sheet.addElement(type, 3 + (i % perRow) * 5, 4 + (i / perRow) * 5);
            ++i;
        }
    }

    /** One element of a named type, at the sheet's centre. For `--ascii`. */
    void buildOne(ref::Schematic& sheet, const juce::String& name)
    {
        const char* names[] = {
            "Ground", "Input", "Output", "Resistor", "Capacitor", "Inductor",
            "Potentiometer", "Switch", "VoltageSource", "Diode", "Transistor",
            "Triode", "OpAmp", "Transformer", "CenterTapTransformer", "Jfet",
            "VacuumDiode", "Pentode", "Text", "Rectangle", "Spdt", "Node", "Scope",
        };
        for (int i = 0; i < static_cast<int>(std::size(names)); ++i)
            if (name == names[i])
            {
                sheet.addElement(static_cast<ref::ElementType>(i), 0, 0);
                return;
            }
        std::fprintf(stderr, "ascii: unknown Céline type '%s'\n", name.toRawUTF8());
    }

#else // ---- our editor -----------------------------------------------------

    // Our model is `livewdf::editor::Schematic`: parts carry a type string and a
    // `values` map, wires are two grid points. `addPart` builds the part from the
    // palette table so its pins match the symbol, which is the whole reason to go
    // through it rather than constructing a `SchematicComponent` by hand.
    void buildSheet(livewdf::editor::Schematic& sheet)
    {
        const auto rTop = sheet.addPart("res", 8, 6);
        const auto rBot = sheet.addPart("res", 8, 12);
        const auto cap = sheet.addPart("cap", 14, 12);
        sheet.addPart("src", 2, 6);

        // No ground element here: our ground is the net name `"0"`, not a
        // placeable part, so a sheet that wants one draws a ground *symbol* --
        // which is `artworkForType("ground")`, exercised by `--strip` instead.
        // Céline, which does have a Ground element, places one at (8,17); the
        // divergence is named rather than papered over with a part we do not
        // have.

        if (auto* e = sheet.findPart(rTop)) e->values["value"] = 100000.0;
        if (auto* e = sheet.findPart(rBot)) e->values["value"] = 47000.0;
        if (auto* e = sheet.findPart(cap))  e->values["value"] = 1.0e-7;

        sheet.addWire(2, 6, 8, 6);
        sheet.addWire(8, 8, 8, 10);
        sheet.addWire(8, 14, 8, 17);
        sheet.addWire(8, 6, 14, 6);
        sheet.addWire(14, 6, 14, 10);
        sheet.addWire(14, 14, 14, 17);
        sheet.addWire(8, 17, 14, 17);
        sheet.addWire(10, 12, 12, 12);
    }

    void buildStrip(livewdf::editor::Schematic& sheet)
    {
        // Palette *parts*, so this is `partPalette`'s vocabulary -- which is also
        // the vocabulary the ground lives in now ("gnd"): it is a part we place,
        // not only a symbol we draw. An earlier version listed "ground" here
        // and silently placed nothing.
        const char* types[] = {
            "res", "cap", "ind", "pot",
            "varres", "diode", "triode", "pentode",
            "transformer", "src", "isrc", "ctrlres",
        };

        constexpr int perRow = 4;
        int i = 0;
        for (const char* type : types)
        {
            sheet.addPart(type, 3 + (i % perRow) * 5, 4 + (i / perRow) * 5);
            ++i;
        }

        // The ground, placed like any other part. It used to be added as a bare
        // component under the artwork key "ground", because it was a symbol we
        // drew and not a part we offered -- but it is a part now ("gnd", with a
        // pin on the ground net), and it is placed as one so the sheet shows
        // what the sheet shows.
        sheet.addPart("gnd", 3, 19);
    }

    /** One part of a named type, at the sheet's centre. For `--ascii`. `ground`
     *  is Celine's name for our `gnd`, so the two harnesses can be given the
     *  same word and their output compared directly. */
    void buildOne(livewdf::editor::Schematic& sheet, const juce::String& type)
    {
        const auto ours = (type == "ground") ? juce::String("gnd") : type;

        const auto id = sheet.addPart(ours.toStdString(), 0, 0);
        if (id.empty())
            std::fprintf(stderr, "ascii: unknown part type '%s'\n", type.toRawUTF8());
    }

#endif

    //==========================================================================
    // Symbol geometry as text.
    //
    // What determines how a symbol looks is: the paths its artwork loaded (in
    // grid units), which are filled and how heavy their strokes are, and where
    // its pins sit. All of that is numbers, so it can be printed and compared --
    // which is the only automated check available to something that cannot see
    // the PNG. The PNG is for the person; this is for the machine.

#if LIVEWDF_RENDER_CELINE
    void dumpSymbols()
    {
        using T = ref::ElementType;

        // **In ElementType's declaration order** (`Celine/source/Schematic/Element.h`),
        // because the enum's index is what this walks. Getting the order wrong
        // silently labels every row with the wrong type -- which is how it was
        // first written, and it made a transformer's geometry look like a JFET's.
        const char* names[] = {
            "Ground", "Input", "Output", "Resistor", "Capacitor", "Inductor",
            "Potentiometer", "Switch", "VoltageSource", "Diode", "Transistor",
            "Triode", "OpAmp", "Transformer", "CenterTapTransformer", "Jfet",
            "VacuumDiode", "Pentode", "Text", "Rectangle", "Spdt", "Node", "Scope",
        };

        for (int i = 0; i < static_cast<int>(std::size(names)); ++i)
        {
            const auto type = static_cast<T>(i);
            ref::Element e;
            e.type = type;

            const auto& info = ref::getElementInfo(type);
            std::printf("TYPE %s\n", names[i]);
            std::printf("  pins %d\n", info.pinCount);
            for (int p = 0; p < info.pinCount; ++p)
                std::printf("    pin %d at (%d,%d)\n", p, info.pins[p].x, info.pins[p].y);

            const auto* artwork = Celine::getSymbolArtwork(e);
            if (artwork == nullptr)
            {
                std::printf("  artwork none\n\n");
                continue;
            }

            std::printf("  asset %s\n", Celine::getSymbolAssetName(e).toRawUTF8());
            std::printf("  shapes %d\n", static_cast<int>(artwork->shapes.size()));
            for (const auto& s : artwork->shapes)
            {
                const auto b = s.path.getBounds();
                std::printf("    shape filled=%d strokeScale=%.3f bounds=(%.2f,%.2f %.2f x %.2f)\n",
                            s.filled ? 1 : 0, s.strokeScale,
                            b.getX(), b.getY(), b.getWidth(), b.getHeight());
            }
            const auto ab = artwork->bounds;
            std::printf("  artworkBounds (%.2f,%.2f %.2f x %.2f)\n\n",
                        ab.getX(), ab.getY(), ab.getWidth(), ab.getHeight());
        }
    }
#else
    void dumpSymbols()
    {
        const char* types[] = {
            "ground", "src", "isrc", "res", "cap", "ind",
            "pot", "varres", "diode", "triode", "pentode", "transformer",
        };

        for (const char* type : types)
        {
            std::printf("TYPE %s\n", type);

            const auto pins = livewdf::editor::Schematic::pinOffsets(type);
            std::printf("  pins %d\n", static_cast<int>(pins.size()));
            for (int p = 0; p < static_cast<int>(pins.size()); ++p)
                std::printf("    pin %d at (%d,%d)\n", p, pins[p].x, pins[p].y);

            if (const auto* artwork = livewdf::editor::artworkForType(type))
            {
                std::printf("  shapes %d\n", static_cast<int>(artwork->shapes.size()));
                for (const auto& s : artwork->shapes)
                {
                    const auto b = s.path.getBounds();
                    std::printf("    shape filled=%d strokeScale=%.3f bounds=(%.2f,%.2f %.2f x %.2f)\n",
                                s.filled ? 1 : 0, s.strokeScale,
                                b.getX(), b.getY(), b.getWidth(), b.getHeight());
                }
                const auto ab = artwork->bounds;
                std::printf("  artworkBounds (%.2f,%.2f %.2f x %.2f)\n\n",
                            ab.getX(), ab.getY(), ab.getWidth(), ab.getHeight());
            }
            else
            {
                std::printf("  artwork none\n\n");
            }
        }
    }
#endif

    //==========================================================================
    bool writePng(juce::Component& component)
    {
        component.setSize(kWidth, kHeight);
        const auto image = component.createComponentSnapshot(component.getLocalBounds(), false);

        outputFile.deleteFile();
        juce::FileOutputStream stream(outputFile);

        if (!stream.openedOk())
        {
            std::fprintf(stderr, "render: cannot open %s\n",
                         outputFile.getFullPathName().toRawUTF8());
            return false;
        }

        juce::PNGImageFormat png;
        if (!png.writeImageToStream(image, stream))
        {
            std::fprintf(stderr, "render: PNG encode failed\n");
            return false;
        }

        std::printf("render: wrote %s (%dx%d)\n",
                    outputFile.getFullPathName().toRawUTF8(), kWidth, kHeight);
        return true;
    }

    //==========================================================================
    // One symbol, as characters.
    //
    // **Why this exists.** The PNG is for the person; a machine cannot read it and
    // neither can I. This prints the same snapshot as a coarse grid of characters
    // -- '#' where the symbol drew ink, '.' where it left the ground -- so the
    // *shape* of a symbol can be read as text and compared between the two sides
    // without an image viewer in the loop. It is lossy on purpose: one character
    // is a block of pixels, which is exactly the level a symbol's silhouette
    // lives at.
    //
    // It cannot say whether a colour is right or a caption is placed well. It says
    // whether a resistor is a rectangle, whether a triode is a triangle with a
    // bar, whether anything drew at all. That is the class of bug that keeps
    // surviving a green test suite.

    void printAsciiArt(const std::function<void(juce::Graphics&)>& paint)
    {
        juce::Image image(juce::Image::ARGB, kWidth, kHeight, true);
        {
            juce::Graphics g(image);
            g.fillAll(juce::Colour(0xff17151a));  // Céline's ground, so ink reads
            // The ground colour is the fill colour, named rather than sampled:
            // sampling before the fill has landed reads back transparent, and
            // "does this cell differ from the empty ground" is then measured
            // against the wrong reference.
            paint(g);
        }

        constexpr int cols = 100;
        constexpr int rows = 50;
        const float cw = static_cast<float>(kWidth) / static_cast<float>(cols);
        const float ch = static_cast<float>(kHeight) / static_cast<float>(rows);

        // The ground colour, taken before the paint (see above).
        const auto groundColour = ground;

        std::printf("ASCII %dx%d over %dx%d px\n", cols, rows, kWidth, kHeight);
        for (int r = 0; r < rows; ++r)
        {
            juce::String line;
            for (int c = 0; c < cols; ++c)
            {
                const int x0 = static_cast<int>(c * cw);
                const int y0 = static_cast<int>(r * ch);
                const int x1 = juce::jmin(kWidth, x0 + static_cast<int>(cw));
                const int y1 = juce::jmin(kHeight, y0 + static_cast<int>(ch));

                int inked = 0;
                int sampled = 0;
                for (int y = y0; y < y1; y += 2)
                    for (int x = x0; x < x1; x += 2)
                    {
                        const auto p = image.getPixelAt(x, y);
                        const int diff = std::abs(static_cast<int>(p.getRed())   - groundColour.getRed())
                                       + std::abs(static_cast<int>(p.getGreen()) - groundColour.getGreen())
                                       + std::abs(static_cast<int>(p.getBlue())  - groundColour.getBlue());
                        if (diff > 40)
                            ++inked;
                        ++sampled;
                    }

                const float frac = sampled > 0 ? static_cast<float>(inked) / static_cast<float>(sampled) : 0.0f;
                line += frac > 0.45f ? '#' : frac > 0.12f ? '+' : frac > 0.02f ? '.' : ' ';
            }
            std::printf("%s\n", line.toRawUTF8());
        }

        // The ink's own bounding box, in pixels. The ASCII map says *that* there
        // is ink; this says *where*, which is what distinguishes "drawn in the
        // wrong place" from "drawn too small" from "not drawn at all".
        int minX = kWidth, minY = kHeight, maxX = -1, maxY = -1;
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
            {
                const auto p = image.getPixelAt(x, y);
                const int diff = std::abs(static_cast<int>(p.getRed())   - ground.getRed())
                               + std::abs(static_cast<int>(p.getGreen()) - ground.getGreen())
                               + std::abs(static_cast<int>(p.getBlue())  - ground.getBlue());
                if (diff > 40)
                {
                    minX = juce::jmin(minX, x); maxX = juce::jmax(maxX, x);
                    minY = juce::jmin(minY, y); maxY = juce::jmax(maxY, y);
                }
            }
        if (maxX < 0)
            std::printf("INK none\n");
        else
            std::printf("INK box x=%d..%d y=%d..%d (w=%d h=%d)\n",
                        minX, maxX, minY, maxY, maxX - minX + 1, maxY - minY + 1);
    }
} // namespace

//==============================================================================
class RenderApp : public juce::JUCEApplicationBase
{
public:
    const juce::String getApplicationName() override { return "livewdf_render"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String&) override {}
    void systemRequestedQuit() override { quit(); }
    void suspended() override {}
    void resumed() override {}
    void unhandledException(const std::exception*, const juce::String&, int) override {}

    void initialise(const juce::String& commandLine) override
    {
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        dumpMode = args.contains("--dump");
        stripMode = args.contains("--strip");

        // `--file <path>` renders a `.livewdf` file, so a circuit the user
        // actually drew can be looked at -- the framing, the wires, the
        // captions -- rather than only a canned sheet.
        const int fileAt = args.indexOf("--file");
        if (fileAt >= 0 && fileAt + 1 < args.size()) {
            fileMode = true;
            filePath = args[fileAt + 1];
        }

        // `--ascii <type>` prints one symbol as characters, so its shape can be
        // read without an image viewer. The type name follows the flag.
        const int asciiAt = args.indexOf("--ascii");
        if (asciiAt >= 0 && asciiAt + 1 < args.size())
        {
            asciiMode = true;
            asciiType = args[asciiAt + 1];
        }
        diagnostics = args.contains("--diag");

        if (dumpMode)
        {
            dumpSymbols();
            setApplicationReturnValue(0);
            quit();
            return;
        }

        if (asciiMode)
        {
#if LIVEWDF_RENDER_CELINE
            ref::Schematic sheet;
            buildOne(sheet, asciiType);
            const auto& elements = sheet.getElements();
            if (elements.empty()) { quit(); return; }
            ref::Element element = elements.front();

            const float gridSize = 26.0f;
            const juce::Point<float> origin(static_cast<float>(kWidth) / 2.0f,
                                            static_cast<float>(kHeight) / 2.0f);
            printAsciiArt([&](juce::Graphics& g) {
                Celine::SymbolPainter painter;
                painter.gridSize = gridSize;
                painter.origin = origin;
                // Symbol and pins, no caption: the drawing is what is being
                // compared, and Céline's element carries no label to caption.
                painter.drawSymbolOnly(g, element, juce::Colour(0xffd9d9d9));
            });
#else
            livewdf::editor::Schematic sheet;
            buildOne(sheet, asciiType);
            const auto doc = sheet.toDocument();
            if (doc.components.empty()) { quit(); return; }
            wdf::SchematicComponent part = doc.components.front();

            // The comparison is about the *symbol*, so the caption is cleared:
            // Céline's element has no label and no value, ours has both, and the
            // caption text would show up as a difference that is not about the
            // drawing. The caption's own appearance is checked separately.
            part.properties.erase("label");
            part.values.clear();

            if (diagnostics) {
                std::fprintf(stderr, "diag: part.type='%s' visual=(%.2f,%.2f) rot=%d flip=%d art=%p\n",
                             part.type.c_str(), part.visual.x, part.visual.y,
                             part.visual.rotation, part.visual.flipped ? 1 : 0,
                             static_cast<const void*>(livewdf::editor::artworkForType(part.type)));
                if (const auto* art = livewdf::editor::artworkForType(part.type)) {
                    std::fprintf(stderr, "diag: shapes=%d bounds=(%.2f,%.2f %.2fx%.2f)\n",
                                 static_cast<int>(art->shapes.size()),
                                 art->bounds.getX(), art->bounds.getY(),
                                 art->bounds.getWidth(), art->bounds.getHeight());
                    for (size_t s = 0; s < art->shapes.size(); ++s) {
                        const auto& sh = art->shapes[s];
                        const auto b = sh.path.getBounds();
                        std::fprintf(stderr, "diag:   shape %d filled=%d bounds=(%.2f,%.2f %.2fx%.2f)\n",
                                     static_cast<int>(s), sh.filled ? 1 : 0,
                                     b.getX(), b.getY(), b.getWidth(), b.getHeight());
                    }
                }
            }

            livewdf::editor::SheetView view;
            view.gridSize = 26.0f;
            view.origin = {static_cast<float>(kWidth) / 2.0f,
                           static_cast<float>(kHeight) / 2.0f};
            printAsciiArt([&](juce::Graphics& g) {
                livewdf::editor::drawComponentSymbolOnly(g, part, view);
            });
#endif
            setApplicationReturnValue(0);
            quit();
            return;
        }

        if (!args.isEmpty())
            outputFile = juce::File::getCurrentWorkingDirectory().getChildFile(args[0]);

        if (outputFile == juce::File{})
        {
            std::fprintf(stderr, "usage: <tool> <output.png> [--strip]\n");
            setApplicationReturnValue(2);
            quit();
            return;
        }

        int result = 0;

#if LIVEWDF_RENDER_CELINE
        {
            ref::Schematic sheet;
            if (stripMode) buildStrip(sheet); else buildSheet(sheet);

            Celine::SchematicCanvas canvas(sheet);
            canvas.setSize(kWidth, kHeight);
            canvas.zoomToFit();
            if (!writePng(canvas)) result = 1;
        }
#else
        {
            livewdf::editor::SchematicEditor canvas;

            if (fileMode) {
                // A circuit the user drew, loaded the way the editor loads it.
                wdf::LiveWDFDocument doc;
                std::string error;
                if (!wdf::LiveWDFDocument::loadFromFile(filePath.toStdString(), doc, error)) {
                    std::fprintf(stderr, "could not load '%s': %s\n",
                                 filePath.toRawUTF8(), error.c_str());
                    setApplicationReturnValue(3);
                    quit();
                    return;
                }

                canvas.setDocument(doc);
            } else {
                livewdf::editor::Schematic sheet;
                if (stripMode) buildStrip(sheet); else buildSheet(sheet);
                canvas.getDocument() = sheet.toDocument();
                canvas.reloadFromDocument();
            }

            canvas.setSize(kWidth, kHeight);
            canvas.frameAll();
            if (!writePng(canvas)) result = 1;
        }
#endif

        setApplicationReturnValue(result);
        quit();
    }

    void shutdown() override {}
};

START_JUCE_APPLICATION(RenderApp)
