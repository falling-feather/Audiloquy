#pragma once

#include "core/project.h"

#include <cstddef>
#include <string>
#include <vector>

namespace listening::editor {

// A bounded project snapshot history for structural edits. The UI records a
// snapshot after a completed operation (add, duplicate, delete or reorder);
// ordinary field editing is folded into the current baseline after a debounce.
class ProjectHistory final {
public:
    explicit ProjectHistory(std::size_t maximumEntries = 32);

    void reset(const Project& project, std::string selectedSegmentId);
    // Fold ordinary field editing into the current baseline and drop redo.
    // This keeps structural undo bounded while ensuring typing after undo
    // cannot later be overwritten by a stale redo snapshot.
    void adoptCurrent(const Project& project, std::string selectedSegmentId);
    // Invalidate redo immediately without copying the live project. The UI
    // uses this on each keystroke and adopts the final value after a debounce.
    void invalidateRedo() noexcept;
    void recordTransition(const Project& before,
                          std::string beforeSelectedSegmentId,
                          const Project& after,
                          std::string afterSelectedSegmentId);

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] bool undo(const Project& liveProject,
                            std::string liveSelectedSegmentId,
                            Project* project,
                            std::string* selectedSegmentId = nullptr);
    [[nodiscard]] bool redo(const Project& liveProject,
                            std::string liveSelectedSegmentId,
                            Project* project,
                            std::string* selectedSegmentId = nullptr);

private:
    std::size_t maximumEntries_;
    struct Snapshot {
        Project project;
        std::string selectedSegmentId;
    };
    std::vector<Snapshot> snapshots_;
    std::size_t cursor_{};

    [[nodiscard]] Project mergeLiveFields(
        const Project& snapshot,
        const Project& liveProject) const;
};

}  // namespace listening::editor
