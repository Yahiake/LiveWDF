#include "MessageConsole.h"

#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <algorithm>
#include <cmath>

namespace livewdf::editor {

namespace {
constexpr int kHeaderH = 46;
constexpr int kRowH = 21;
constexpr int kRowPad = 4;

juce::String plural(int count, const char* one, const char* many) {
    return juce::String(count) + " " + (count == 1 ? one : many);
}
}  // namespace

MessageConsole::MessageConsole() {
    setOpaque(true);
    setInterceptsMouseClicks(false, true);

    viewport_.setViewedComponent(&rowList_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(8);
    viewport_.setWantsKeyboardFocus(false);
    addAndMakeVisible(viewport_);
}

void MessageConsole::setContent(juce::String headline, Severity headlineSeverity,
                                std::vector<Message> messages) {
    headline_ = std::move(headline);
    headlineSeverity_ = headlineSeverity;

    rows_.clear();
    rows_.reserve(messages.size());
    for (auto& message : messages) {
        Row row;
        row.text = std::move(message.text);
        row.severity = message.severity;
        row.partId = std::move(message.partId);
        rows_.push_back(std::move(row));
    }

    layoutRows();
    repaint();
}

void MessageConsole::clear() {
    headline_.clear();
    headlineSeverity_ = Severity::Info;
    rows_.clear();
    layoutRows();
    repaint();
}

int MessageConsole::problemCount() const noexcept {
    // The headline is a summary, not another problem. Counting it would make
    // "Build failed" plus its one error row read as two problems.
    int count = 0;
    for (const auto& row : rows_)
        if (row.severity == Severity::Error || row.severity == Severity::Warning)
            ++count;
    return count;
}

juce::String MessageConsole::rowText(int index) const {
    if (index < 0 || index >= static_cast<int>(rows_.size()))
        return {};
    return rows_[static_cast<size_t>(index)].text;
}

juce::String MessageConsole::rowPartId(int index) const {
    if (index < 0 || index >= static_cast<int>(rows_.size()))
        return {};
    return rows_[static_cast<size_t>(index)].partId;
}

MessageConsole::Severity MessageConsole::rowSeverity(int index) const {
    if (index < 0 || index >= static_cast<int>(rows_.size()))
        return Severity::Info;
    return rows_[static_cast<size_t>(index)].severity;
}

void MessageConsole::clickRow(int index) {
    if (index < 0 || index >= static_cast<int>(rows_.size()))
        return;

    const auto& row = rows_[static_cast<size_t>(index)];
    if (!row.partId.isEmpty() && onMessageClicked)
        onMessageClicked(row.partId);
}

juce::Colour MessageConsole::colourFor(Severity severity) {
    switch (severity) {
    case Severity::Error:
        return glow::getColourHotRed();
    case Severity::Warning:
        return glow::getColourAmber();
    case Severity::Notice:
        return glow::getColourSignal();
    case Severity::Info:
    default:
        return glow::getColourTextSecondary();
    }
}

void MessageConsole::layoutRows() {
    const int viewportH = std::max(1, viewport_.getHeight());
    const int naturalH = kRowPad * 2 + static_cast<int>(rows_.size()) * kRowH;
    int width = std::max(1, viewport_.getWidth());

    if (naturalH > viewportH)
        width = std::max(1, width - viewport_.getScrollBarThickness());

    rowList_.setSize(width, std::max(naturalH, viewportH));
    rowList_.repaint();
}

void MessageConsole::resized() {
    viewport_.setBounds(getLocalBounds().withTrimmedTop(kHeaderH));
    layoutRows();
}

void MessageConsole::paint(juce::Graphics& g) {
    g.fillAll(glow::getColourPanel());

    // Header band. The title is the smallest type in the window on purpose:
    // the console is an instrument readout, not a document.
    g.setColour(glow::getColourBorder());
    g.drawHorizontalLine(kHeaderH - 1, 0.0f, static_cast<float>(getWidth()));

    g.setColour(glow::getColourTextSecondary());
    g.setFont(Fonts::semiBold(10.0f));
    g.drawText("MESSAGES", juce::Rectangle<int>(10, 4, 160, 16),
               juce::Justification::centredLeft);

    const int problems = problemCount();
    const juce::Colour countColour =
        headlineSeverity_ == Severity::Error || std::any_of(rows_.begin(), rows_.end(),
                                                            [](const Row& r) {
                                                                return r.severity == Severity::Error;
                                                            })
            ? glow::getColourHotRed()
            : problems > 0 ? glow::getColourAmber() : glow::getColourTextSecondary();

    g.setColour(countColour);
    g.setFont(Fonts::mono(10.0f));
    const juce::String countText =
        problems > 0 ? plural(problems, "problem", "problems")
                     : plural(static_cast<int>(rows_.size()), "message", "messages");
    g.drawText(countText, juce::Rectangle<int>(getWidth() - 170, 4, 160, 16),
               juce::Justification::centredRight);

    if (!headline_.isEmpty()) {
        g.setColour(headlineSeverity_ == Severity::Error     ? glow::getColourHotRed()
                    : headlineSeverity_ == Severity::Warning ? glow::getColourAmber()
                    : headlineSeverity_ == Severity::Notice  ? glow::getColourSignal()
                                                              : glow::getColourTextPrimary());
        g.setFont(Fonts::mono(11.5f));
        g.drawText(headline_, juce::Rectangle<int>(10, 20, getWidth() - 20, 18),
                   juce::Justification::centredLeft, true);
    }

}

int MessageConsole::RowList::rowAt(int y) const {
    if (y < kRowPad)
        return -1;

    const int index = (y - kRowPad) / kRowH;
    if (index < 0 || index >= static_cast<int>(owner_.rows_.size()))
        return -1;

    const int rowTop = kRowPad + index * kRowH;
    return y < rowTop + kRowH ? index : -1;
}

void MessageConsole::RowList::paint(juce::Graphics& g) {
    g.fillAll(glow::getColourPanel());

    if (owner_.rows_.empty()) {
        g.setColour(glow::getColourTextSecondary().withAlpha(0.65f));
        g.setFont(Fonts::mono(11.0f));
        g.drawText("Nothing to report.", juce::Rectangle<int>(14, 6, getWidth() - 28, kRowH),
                   juce::Justification::centredLeft);
        return;
    }

    const juce::Font font = Fonts::mono(11.5f);
    g.setFont(font);

    for (int i = 0; i < static_cast<int>(owner_.rows_.size()); ++i) {
        const auto& row = owner_.rows_[static_cast<size_t>(i)];
        const juce::Rectangle<int> area(0, kRowPad + i * kRowH, getWidth(), kRowH);

        if (i == hovered_)
            g.setColour(glow::getColourElevated());
        else if (i % 2 == 1)
            g.setColour(glow::getColourBackground().withAlpha(0.24f));
        else
            g.setColour(juce::Colours::transparentBlack);

        if (i == hovered_ || i % 2 == 1)
            g.fillRect(area);

        // A severity bar, not a bullet: the eye can scan a column of them.
        const auto severityColour = MessageConsole::colourFor(row.severity);
        g.setColour(severityColour.withAlpha(row.severity == Severity::Info ? 0.4f : 0.9f));
        g.fillRect(area.getX() + 6, area.getY() + 5, 3, area.getHeight() - 10);

        g.setColour(row.severity == Severity::Info ? glow::getColourTextPrimary()
                                                   : severityColour);
        const int rightTrim = row.partId.isEmpty() ? 8 : 84;
        g.drawText(row.text, area.withTrimmedLeft(14).withTrimmedRight(rightTrim),
                   juce::Justification::centredLeft, true);

        // A row that points at a part says so with a tiny target glyph. The
        // click is the affordance; this is the hint that there is one.
        if (!row.partId.isEmpty()) {
            g.setColour(glow::getColourAccent().withAlpha(0.85f));
            g.setFont(Fonts::mono(9.0f));
            g.drawText(row.partId, area.withTrimmedRight(8),
                       juce::Justification::centredRight);
            g.setFont(font);
        }
    }
}

void MessageConsole::RowList::mouseDown(const juce::MouseEvent& e) {
    owner_.clickRow(rowAt(e.y));
}

void MessageConsole::RowList::mouseMove(const juce::MouseEvent& e) {
    const int row = rowAt(e.y);
    setMouseCursor(row >= 0 && !owner_.rows_[static_cast<size_t>(row)].partId.isEmpty()
                       ? juce::MouseCursor::PointingHandCursor
                       : juce::MouseCursor::NormalCursor);

    if (row != hovered_) {
        hovered_ = row;
        repaint();
    }
}

void MessageConsole::RowList::mouseExit(const juce::MouseEvent&) {
    setMouseCursor(juce::MouseCursor::NormalCursor);
    if (hovered_ != -1) {
        hovered_ = -1;
        repaint();
    }
}

}  // namespace livewdf::editor
