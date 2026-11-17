#pragma once

// The editor's message console.
//
// The node table answers "what is the circuit doing"; this answers "why did it
// not build, and which part do I look at". They are deliberately separate: a
// diagnostic list that replaced the operating point would take away the check
// the editor leans on most.
//
// Rows are short and one line each. A row that names a part is clickable, and
// clicking it selects that part on the sheet without stealing focus from the
// drawing in any other way.

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace livewdf::editor {

class MessageConsole : public juce::Component {
public:
    enum class Severity { Info, Notice, Warning, Error };

    struct Message {
        juce::String text;
        Severity severity = Severity::Info;

        /// Empty when the message is not about one part. A non-empty id makes
        /// the row clickable.
        juce::String partId;
    };

    MessageConsole();

    /// Replace the headline and the rows. The headline is always shown; the
    /// rows scroll under it.
    void setContent(juce::String headline, Severity headlineSeverity,
                    std::vector<Message> messages);

    void clear();

    /// The number of rows, including informational notes but not the headline.
    int messageCount() const noexcept { return static_cast<int>(rows_.size()); }

    /// Errors and warnings among the rows. The headline is a summary, so it is
    /// not counted as another problem.
    int problemCount() const noexcept;

    bool hasProblems() const noexcept { return problemCount() > 0; }

    /// Read-only access for tests, and for the host when it wants to mirror the
    /// console somewhere else.
    juce::String rowText(int index) const;
    juce::String rowPartId(int index) const;
    Severity rowSeverity(int index) const;

    /// What clicking a row does. Public so a test can exercise the same path a
    /// pointer takes instead of synthesising a mouse event.
    void clickRow(int index);

    /// A row that names a part was clicked.
    std::function<void(const juce::String&)> onMessageClicked;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Row {
        juce::String text;
        Severity severity = Severity::Info;
        juce::String partId;
    };

    class RowList : public juce::Component {
    public:
        explicit RowList(MessageConsole& owner) : owner_(owner) {}

        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;

    private:
        int rowAt(int y) const;
        MessageConsole& owner_;
        int hovered_ = -1;
    };

    void layoutRows();
    static juce::Colour colourFor(Severity severity);

    juce::String headline_;
    Severity headlineSeverity_ = Severity::Info;
    std::vector<Row> rows_;

    juce::Viewport viewport_;
    RowList rowList_{*this};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MessageConsole)
};

}  // namespace livewdf::editor
