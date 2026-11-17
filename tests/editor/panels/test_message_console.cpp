// The message drawer is the place where "why did it not build" is answered.
//
// These tests hold the two things the editor relies on: a row that names a part
// can be clicked to select it, and the problem count is a count of *rows*, not
// of a headline that summarises them.

#include <catch2.hpp>

#include "MessageConsole.h"

using livewdf::editor::MessageConsole;

TEST_CASE("A message console keeps its rows and counts only real problems") {
    MessageConsole console;

    std::vector<MessageConsole::Message> messages;
    messages.push_back({"R1.a is not connected to anything",
                        MessageConsole::Severity::Warning, "R1"});
    messages.push_back({"A note about the loader", MessageConsole::Severity::Info, {}});
    messages.push_back({"C2 is not connected to anything",
                        MessageConsole::Severity::Warning, "C2"});

    console.setContent("3 parts  4 nodes", MessageConsole::Severity::Info,
                       std::move(messages));

    CHECK(console.messageCount() == 3);
    CHECK(console.problemCount() == 2);
    CHECK(console.hasProblems());
    CHECK(console.rowText(0) == "R1.a is not connected to anything");
    CHECK(console.rowPartId(0) == "R1");
    CHECK(console.rowSeverity(0) == MessageConsole::Severity::Warning);
    CHECK(console.rowPartId(1).isEmpty());
}

TEST_CASE("Clicking a diagnostic row selects the part it names") {
    MessageConsole console;
    console.setContent("Build failed", MessageConsole::Severity::Error,
                       {{"root transfer failed", MessageConsole::Severity::Error, "C1"},
                        {"a note without a part", MessageConsole::Severity::Info, {}}});

    juce::String clicked;
    console.onMessageClicked = [&clicked](const juce::String& id) { clicked = id; };

    console.clickRow(0);
    CHECK(clicked == "C1");

    clicked.clear();
    console.clickRow(1);
    CHECK(clicked.isEmpty());

    console.clickRow(-1);
    console.clickRow(99);
    CHECK(clicked.isEmpty());
}

TEST_CASE("A headline is a summary, not an extra problem") {
    MessageConsole console;
    console.setContent("Build failed", MessageConsole::Severity::Error,
                       {{"root transfer failed", MessageConsole::Severity::Error, "C1"}});

    // One error row, not "headline + row".
    CHECK(console.problemCount() == 1);
    CHECK(console.hasProblems());

    console.clear();
    CHECK(console.messageCount() == 0);
    CHECK(console.problemCount() == 0);
    CHECK_FALSE(console.hasProblems());
}
