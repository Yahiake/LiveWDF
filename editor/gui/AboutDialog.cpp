#include "AboutDialog.h"
#include "Fonts.h"
#include "glow/Glow.h"
#include "livewdf.h"

namespace livewdf::editor {

AboutDialog::AboutDialog(Mode mode) : mode_(mode) {
    setAlwaysOnTop(true);
    setWantsKeyboardFocus(true);

    closeBtn_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    closeBtn_.setColour(juce::TextButton::textColourOffId, glow::getColourNormal());
    closeBtn_.onClick = [this] {
        if (auto* parent = getParentComponent())
            parent->removeChildComponent(this);
    };
    addAndMakeVisible(closeBtn_);

    githubBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1E2530));
    githubBtn_.setColour(juce::TextButton::textColourOffId, glow::getColourAccent());
    githubBtn_.onClick = [] {
        juce::URL("https://github.com/Yahiake").launchInDefaultBrowser();
    };
    addAndMakeVisible(githubBtn_);

    docBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1E2530));
    docBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCBD5E1));
    docBtn_.onClick = [] {
        juce::URL("https://github.com/Yahiake").launchInDefaultBrowser();
    };
    addAndMakeVisible(docBtn_);

    licenseBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1E2530));
    licenseBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCBD5E1));
    licenseBtn_.onClick = [] {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            "LiveWDF License",
            "LiveWDF is developed independently by Yahia Kemari.\n"
            "Released under the GNU Affero General Public License v3 (AGPLv3).\n\n"
            "Third-Party Notices:\n"
            "- JUCE Framework (GPLv3 / Commercial)\n"
            "- Inter & IBM Plex Mono fonts (SIL Open Font License 1.1)");
    };
    addAndMakeVisible(licenseBtn_);

    if (mode_ == Mode::Keys)
    {
        // The keyboard card has nothing to click through: it is for reading,
        // and its close button is the one control it needs.
        githubBtn_.setVisible(false);
        docBtn_.setVisible(false);
        licenseBtn_.setVisible(false);
    }
}

void AboutDialog::show(juce::Component* parent, Mode mode) {
    if (parent == nullptr) return;
    auto* dialog = new AboutDialog(mode);
    dialog->setBounds(parent->getLocalBounds());
    parent->addAndMakeVisible(dialog);
    dialog->grabKeyboardFocus();
}

bool AboutDialog::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        if (auto* parent = getParentComponent())
            parent->removeChildComponent(this);
        return true;
    }
    return false;
}

void AboutDialog::mouseDown(const juce::MouseEvent& e) {
    if (!cardBounds_.contains(e.getPosition())) {
        if (auto* parent = getParentComponent())
            parent->removeChildComponent(this);
    }
}

void AboutDialog::paint(juce::Graphics& g) {
    // 1. Frosted dim scrim backdrop
    g.fillAll(juce::Colour(0xDD0B0D10));

    // 2. Main About Card (dark graphite chassis)
    auto card = cardBounds_.toFloat();

    // Drop shadow
    g.setColour(juce::Colour(0x90000000));
    g.fillRoundedRectangle(card.expanded(4.0f).translated(0.0f, 6.0f), 10.0f);

    // Card background
    g.setColour(juce::Colour(0xFF14171D));
    g.fillRoundedRectangle(card, 8.0f);

    // Bezel border
    g.setColour(juce::Colour(0xFF273140));
    g.drawRoundedRectangle(card, 8.0f, 1.2f);

    if (mode_ == Mode::Keys)
    {
        // ---- Keyboard card ---------------------------------------------------
        // Two columns, not sentences: the answer to "what does Ctrl+D do"
        // should be one glance. The keys are set in mono -- they are *text* the
        // sheet answers to, and mono is how the sheet itself prints text.
        const int pad = 28;
        int curY = cardBounds_.getY() + pad;

        g.setFont(Fonts::bold(20.0f));
        g.setColour(juce::Colour(0xFFFFFFFF));
        g.drawText("Keyboard", cardBounds_.getX() + pad, curY,
                   cardBounds_.getWidth() - 2 * pad - 40, 26, juce::Justification::left);
        curY += 28;

        g.setFont(Fonts::semiBold(13.5f));
        g.setColour(glow::getColourAccent());
        g.drawText("Every shortcut the sheet answers to.", cardBounds_.getX() + pad, curY,
                   cardBounds_.getWidth() - 2 * pad, 20, juce::Justification::left);
        curY += 26;

        g.setColour(juce::Colour(0xFF232A36));
        g.drawHorizontalLine(curY, static_cast<float>(cardBounds_.getX() + pad),
                             static_cast<float>(cardBounds_.getRight() - pad));
        curY += 16;

        struct Row { const char* key; const char* what; };
        const Row rows[] = {
            {"S / W / X", "select · wire · delete tool"},
            {"F", "fit the sheet to the drawing"},
            {"Esc", "back to select, clear selection"},
            {"part letter", "arm that part from the palette"},
            {"digits + Enter", "set the selected part's value"},
            {"Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y", "undo · redo"},
            {"Ctrl+D / Ctrl+R / Ctrl+F", "duplicate · rotate (Space) · flip"},
            {"Ctrl+C / Ctrl+X / Ctrl+V", "copy · cut · paste"},
            {"middle-click", "clone what is under the pointer"},
            {"right-click while placing", "cancel, back to select"},
            {"right-drag", "pan the sheet"},
            {"wheel", "zoom about the pointer"},
        };

        const int keyW = 224;
        const int whatX = cardBounds_.getX() + pad + keyW + 10;
        const int whatW = cardBounds_.getRight() - pad - whatX;

        for (const auto& row : rows)
        {
            g.setFont(Fonts::monoSemiBold(12.0f));
            g.setColour(juce::Colour(0xFFE6EDF5));
            g.drawText(row.key, cardBounds_.getX() + pad, curY, keyW, 16,
                       juce::Justification::left);

            g.setFont(Fonts::medium(11.5f));
            g.setColour(juce::Colour(0xFF8B9BB4));
            g.drawText(row.what, whatX, curY, whatW, 16, juce::Justification::left);

            curY += 22;
        }

        // Footer: the two parts of the value gesture that are easy to lose.
        curY += 12;
        g.setColour(juce::Colour(0xFF232A36));
        g.drawHorizontalLine(curY, static_cast<float>(cardBounds_.getX() + pad),
                             static_cast<float>(cardBounds_.getRight() - pad));
        curY += 12;

        g.setFont(Fonts::medium(11.5f));
        g.setColour(juce::Colour(0xFF8B9BB4));
        g.drawText("a number commits on Enter -- Esc or a click drops it",
                   cardBounds_.getX() + pad, curY, cardBounds_.getWidth() - 2 * pad, 16,
                   juce::Justification::left);
        return;
    }

    // 3. Header title and subtitle
    const int pad = 28;
    int curY = cardBounds_.getY() + pad;

    g.setFont(Fonts::bold(20.0f));
    g.setColour(juce::Colour(0xFFFFFFFF));
    const juce::String title = (mode_ == Mode::Editor) ? "LiveWDF Editor" : "LiveWDF";
    g.drawText(title, cardBounds_.getX() + pad, curY, cardBounds_.getWidth() - 2 * pad - 40, 26, juce::Justification::left);
    curY += 28;

    g.setFont(Fonts::semiBold(13.5f));
    g.setColour(glow::getColourAccent());
    const juce::String subtitle = (mode_ == Mode::Editor)
        ? "Design. Explore. Understand analog circuits."
        : "Analog circuits. Real-time audio.";
    g.drawText(subtitle, cardBounds_.getX() + pad, curY, cardBounds_.getWidth() - 2 * pad, 20, juce::Justification::left);
    curY += 26;

    // Divider line
    g.setColour(juce::Colour(0xFF232A36));
    g.drawHorizontalLine(curY, static_cast<float>(cardBounds_.getX() + pad),
                               static_cast<float>(cardBounds_.getRight() - pad));
    curY += 14;

    // 4. Body Description Text
    g.setFont(Fonts::medium(12.5f));
    g.setColour(juce::Colour(0xFFCBD5E1));

    juce::String bodyText;
    if (mode_ == Mode::Editor) {
        bodyText =
            "LiveWDF is an analog circuit design and simulation environment built around Wave Digital Filter techniques.\n\n"
            "Build circuits, explore component models, and investigate the behavior of analog networks through an interactive schematic workspace.\n\n"
            "From passive components to nonlinear semiconductor and vacuum-tube models, LiveWDF brings circuit construction and simulation together in one environment.";
    } else {
        bodyText =
            "LiveWDF brings circuit-based analog simulation into the audio domain.\n\n"
            "Powered by the LiveWDF Wave Digital Filter engine, the plugin allows analog circuit behavior to be explored and processed directly within your audio workflow.\n\n"
            "Built with a focus on nonlinear circuit modeling, numerical accuracy, and real-time performance.";
    }

    juce::Rectangle<int> textRect(cardBounds_.getX() + pad, curY, cardBounds_.getWidth() - 2 * pad, 125);
    g.drawFittedText(bodyText, textRect, juce::Justification::topLeft, 10);
    curY += 130;

    // 5. Author Card (Yahia Kemari)
    auto authorRect = juce::Rectangle<float>(static_cast<float>(cardBounds_.getX() + pad), static_cast<float>(curY),
                                             static_cast<float>(cardBounds_.getWidth() - 2 * pad), 34.0f);
    g.setColour(juce::Colour(0xFF0F1217));
    g.fillRoundedRectangle(authorRect, 6.0f);
    g.setColour(juce::Colour(0xFF222B38));
    g.drawRoundedRectangle(authorRect, 6.0f, 1.0f);

    g.setFont(Fonts::semiBold(13.0f));
    g.setColour(juce::Colour(0xFFFFFFFF));
    g.drawText("Developed independently by Yahia Kemari.", authorRect.toNearestInt(), juce::Justification::centred);
    curY += 44;

    // Divider line
    g.setColour(juce::Colour(0xFF232A36));
    g.drawHorizontalLine(curY, static_cast<float>(cardBounds_.getX() + pad),
                               static_cast<float>(cardBounds_.getRight() - pad));
    curY += 12;

    // 6. Version and copyright
    g.setFont(Fonts::monoSemiBold(12.0f));
    g.setColour(juce::Colour(0xFF8B9BB4));
    g.drawText(juce::String("Version: ") + livewdf::version(),
               cardBounds_.getX() + pad, curY, 200, 20,
               juce::Justification::left);

    g.setFont(Fonts::medium(11.5f));
    g.drawText("(C) 2026 Yahia Kemari  |  GNU AGPLv3",
               cardBounds_.getX() + pad, curY + 20, cardBounds_.getWidth() - 2 * pad, 20,
               juce::Justification::left);
}

void AboutDialog::resized() {
    const int cardW = juce::jmin(560, getWidth() - 40);
    const int cardH = 430;
    cardBounds_ = juce::Rectangle<int>((getWidth() - cardW) / 2,
                                       (getHeight() - cardH) / 2,
                                       cardW, cardH);

    // Close button (top right of card)
    closeBtn_.setBounds(cardBounds_.getRight() - 36, cardBounds_.getY() + 12, 24, 24);

    // Bottom action link buttons
    const int pad = 28;
    const int btnY = cardBounds_.getBottom() - 44;
    const int btnW = 100;
    githubBtn_.setBounds(cardBounds_.getRight() - pad - btnW, btnY, btnW, 28);
    docBtn_.setBounds(cardBounds_.getRight() - pad - 2 * btnW - 8, btnY, btnW + 10, 28);
    licenseBtn_.setBounds(cardBounds_.getRight() - pad - 3 * btnW - 16, btnY, btnW - 15, 28);
}

}  // namespace livewdf::editor
