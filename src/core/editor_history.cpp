#include "core/editor_history.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace listening::editor {

ProjectHistory::ProjectHistory(std::size_t maximumEntries)
    : maximumEntries_(std::max<std::size_t>(2, maximumEntries)) {}

void ProjectHistory::reset(const Project& project, std::string selectedSegmentId) {
    snapshots_.clear();
    snapshots_.push_back(Snapshot{project, std::move(selectedSegmentId)});
    cursor_ = 0;
}

void ProjectHistory::adoptCurrent(const Project& project, std::string selectedSegmentId) {
    if (snapshots_.empty()) {
        reset(project, std::move(selectedSegmentId));
        return;
    }
    snapshots_[cursor_] = Snapshot{project, std::move(selectedSegmentId)};
    if (cursor_ + 1 < snapshots_.size()) {
        snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1),
                         snapshots_.end());
    }
}

void ProjectHistory::invalidateRedo() noexcept {
    if (!snapshots_.empty() && cursor_ + 1 < snapshots_.size()) {
        snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1),
                         snapshots_.end());
    }
}

void ProjectHistory::recordTransition(const Project& before,
                                      std::string beforeSelectedSegmentId,
                                      const Project& after,
                                      std::string afterSelectedSegmentId) {
    if (snapshots_.empty()) {
        reset(before, beforeSelectedSegmentId);
    } else if (snapshots_[cursor_].project != before ||
               snapshots_[cursor_].selectedSegmentId != beforeSelectedSegmentId) {
        // The editor deliberately does not record every QTextEdit keystroke.
        // Fold those unrecorded edits into the current baseline before adding
        // the structural transition, preserving text when the operation is
        // later undone.
        snapshots_[cursor_] = Snapshot{before, std::move(beforeSelectedSegmentId)};
    }
    if (snapshots_[cursor_].project == after &&
        snapshots_[cursor_].selectedSegmentId == afterSelectedSegmentId) {
        return;
    }
    if (cursor_ + 1 < snapshots_.size()) {
        snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1),
                         snapshots_.end());
    }
    snapshots_.push_back(Snapshot{after, std::move(afterSelectedSegmentId)});
    cursor_ = snapshots_.size() - 1;
    if (snapshots_.size() > maximumEntries_) {
        const std::size_t removeCount = snapshots_.size() - maximumEntries_;
        snapshots_.erase(snapshots_.begin(),
                         snapshots_.begin() + static_cast<std::ptrdiff_t>(removeCount));
        cursor_ -= std::min(cursor_, removeCount);
    }
}

bool ProjectHistory::canUndo() const noexcept {
    return !snapshots_.empty() && cursor_ > 0;
}

bool ProjectHistory::canRedo() const noexcept {
    return !snapshots_.empty() && cursor_ + 1 < snapshots_.size();
}

Project ProjectHistory::mergeLiveFields(const Project& snapshot,
                                        const Project& liveProject) const {
    Project merged = snapshot;
    // Structural history owns the segment vector and output program reference,
    // while ordinary field edits belong to the live project. This lets
    // "add B, then edit A, then undo add" retain A's latest text.
    merged.title = liveProject.title;
    merged.accent = liveProject.accent;
    merged.targetWpm = liveProject.targetWpm;
    merged.voiceSettings = liveProject.voiceSettings;
    // A structural undo may expose a snapshot from before the latest full
    // program assembly. Reusing that program would be unsafe when live text
    // or ordering has changed, so force a fresh program assembly.
    merged.renderedProgramFile.clear();
    const bool voiceSettingsChanged = snapshot.accent != liveProject.accent ||
                                      snapshot.targetWpm != liveProject.targetWpm ||
                                      snapshot.voiceSettings != liveProject.voiceSettings;

    std::unordered_map<std::string, const Segment*> liveById;
    liveById.reserve(liveProject.segments.size());
    for (const Segment& segment : liveProject.segments) {
        liveById.emplace(segment.id, &segment);
    }
    for (Segment& segment : merged.segments) {
        const auto found = liveById.find(segment.id);
        if (found != liveById.end()) {
            segment = *found->second;
        } else if (voiceSettingsChanged) {
            // This segment is being restored from history. Its old rendered
            // audio was produced under settings that no longer match the live
            // project and must not be presented as playable.
            segment.renderedAudioFile.clear();
        }
    }
    return merged;
}

bool ProjectHistory::undo(const Project& liveProject,
                          std::string liveSelectedSegmentId,
                          Project* project,
                          std::string* selectedSegmentId) {
    if (project == nullptr || !canUndo()) {
        return false;
    }
    --cursor_;
    *project = mergeLiveFields(snapshots_[cursor_].project, liveProject);
    if (selectedSegmentId != nullptr) {
        *selectedSegmentId = snapshots_[cursor_].selectedSegmentId;
        if (project->findSegment(*selectedSegmentId) == nullptr) {
            *selectedSegmentId = std::move(liveSelectedSegmentId);
        }
    }
    return true;
}

bool ProjectHistory::redo(const Project& liveProject,
                          std::string liveSelectedSegmentId,
                          Project* project,
                          std::string* selectedSegmentId) {
    if (project == nullptr || !canRedo()) {
        return false;
    }
    ++cursor_;
    *project = mergeLiveFields(snapshots_[cursor_].project, liveProject);
    if (selectedSegmentId != nullptr) {
        *selectedSegmentId = snapshots_[cursor_].selectedSegmentId;
        if (project->findSegment(*selectedSegmentId) == nullptr) {
            *selectedSegmentId = std::move(liveSelectedSegmentId);
        }
    }
    return true;
}

}  // namespace listening::editor
