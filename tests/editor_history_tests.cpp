#include "core/editor_history.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using listening::Project;
using listening::Segment;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

Project projectWithText(std::string text) {
    Project project;
    project.id = "history-project";
    project.title = "History";
    project.segments.emplace_back("segment-1", listening::QuestionRange{4, 4},
                                  "Narrator", std::move(text), 1.0, 1);
    return project;
}

void structuralUndoPreservesUnrecordedText() {
    Project initial = projectWithText("");
    initial.segments.front().renderedAudioFile = "old-segment.wav";
    initial.renderedProgramFile = "old-program.wav";
    listening::editor::ProjectHistory history;
    history.reset(initial, "segment-1");

    Project before = initial;
    before.segments.front().text = "draft before add";
    Project after = before;
    after.segments.emplace_back("segment-2", listening::QuestionRange{8, 8},
                                "Woman", "new group", 2.0, 1);
    history.recordTransition(before, "segment-1", after, "segment-2");

    Project restored;
    std::string selected;
    expect(history.undo(after, "segment-2", &restored, &selected),
           "add operation should be undoable");
    expect(restored.segments == before.segments && selected == "segment-1",
           "undo must restore the exact pre-operation text and selection");
    expect(restored.renderedProgramFile.empty(),
           "structural undo must not resurrect a stale complete-program reference");
    expect(history.redo(before, "segment-1", &restored, &selected),
           "add operation should be redoable");
    expect(restored.segments == after.segments && selected == "segment-2",
           "redo must restore the exact post-operation snapshot");
    expect(restored.renderedProgramFile.empty(),
           "redo must keep the complete-program reference invalidated");
}

void restoredAudioIsInvalidatedWhenVoiceSettingsChange() {
    Project before = projectWithText("A");
    before.segments.front().renderedAudioFile = "a.wav";
    before.segments.emplace_back("segment-2", listening::QuestionRange{2, 2},
                                 "Narrator", "B", 0.0, 1, "b.wav");
    before.renderedProgramFile = "program.wav";
    Project after = before;
    after.segments.erase(after.segments.begin());
    after.renderedProgramFile.clear();

    listening::editor::ProjectHistory history;
    history.reset(before, "segment-1");
    history.recordTransition(before, "segment-1", after, "segment-2");

    Project live = after;
    live.accent = listening::Accent::British;
    live.targetWpm = 160.0;
    live.voiceSettings.maleVoiceTokenId = "changed-male";
    Project restored;
    std::string selected;
    expect(history.undo(live, "segment-2", &restored, &selected),
           "delete operation should be undoable after settings change");
    expect(restored.findSegment("segment-1") != nullptr &&
               restored.findSegment("segment-1")->renderedAudioFile.empty(),
           "a restored group must lose audio made under old voice settings");
    expect(restored.renderedProgramFile.empty(),
           "a restored project must not retain an old program audio reference");
}

void laterEditSurvivesStructuralUndo() {
    const Project initial = projectWithText("original A");
    Project afterAdd = initial;
    afterAdd.segments.emplace_back("segment-2", listening::QuestionRange{2, 2},
                                   "Narrator", "B", 0.0, 1);
    listening::editor::ProjectHistory history;
    history.reset(initial, "segment-1");
    history.recordTransition(initial, "segment-1", afterAdd, "segment-2");

    Project live = afterAdd;
    live.segments.front().text = "edited A after adding B";
    Project restored;
    std::string selected;
    expect(history.undo(live, "segment-2", &restored, &selected),
           "structural undo should work after a later field edit");
    expect(restored.segments.size() == 1 &&
               restored.segments.front().text == "edited A after adding B",
           "undoing the add must not discard edits to a surviving segment");
}

void typingAfterUndoDropsStaleRedo() {
    const Project initial = projectWithText("one");
    Project added = initial;
    added.segments.emplace_back("segment-2", listening::QuestionRange{2, 2},
                                "Man", "two", 0.0, 1);
    listening::editor::ProjectHistory history;
    history.reset(initial, "segment-1");
    history.recordTransition(initial, "segment-1", added, "segment-2");

    Project restored;
    std::string selection;
    expect(history.undo(added, "segment-2", &restored, &selection),
           "undo should move to the initial state");
    restored.segments.front().text = "typed after undo";
    history.adoptCurrent(restored, "segment-1");
    expect(!history.canRedo(), "typing after undo must invalidate stale redo");
    expect(history.undo(restored, "segment-1", &restored, &selection) == false,
           "there should be no older structural state after the baseline was adopted");
}

void boundedHistoryKeepsSnapshotsAligned() {
    listening::editor::ProjectHistory history(3);
    Project current = projectWithText("0");
    history.reset(current, "segment-1");
    for (int index = 1; index <= 8; ++index) {
        Project next = current;
        next.segments.front().text = std::to_string(index);
        next.segments.emplace_back("s-" + std::to_string(index),
                                   listening::QuestionRange{index + 10, index + 10},
                                   "Narrator", "", 0.0, 1);
        history.recordTransition(current, "segment-1", next, next.segments.back().id);
        current = std::move(next);
    }
    int undoCount = 0;
    Project restored;
    std::string selected;
    while (history.undo(current, "s-8", &restored, &selected)) {
        current = restored;
        ++undoCount;
        expect(!selected.empty(), "bounded snapshots must retain selection metadata");
    }
    expect(undoCount == 2, "history bound should retain exactly the requested entries");
}

}  // namespace

int main() {
    try {
        structuralUndoPreservesUnrecordedText();
        restoredAudioIsInvalidatedWhenVoiceSettingsChange();
        laterEditSurvivesStructuralUndo();
        typingAfterUndoDropsStaleRedo();
        boundedHistoryKeepsSnapshotsAligned();
        std::cout << "editor_history_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "editor_history_tests failure: " << error.what() << '\n';
        return 1;
    }
}
