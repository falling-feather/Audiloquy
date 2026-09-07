#pragma once

#include "core/project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace listening::recovery {

struct Candidate {
    std::filesystem::path directory;
    std::filesystem::path draftPath;
    std::filesystem::path originalProjectPath;
};

class Store final {
public:
    explicit Store(std::filesystem::path root);
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // Starts ownership of one session directory. Existing records belonging
    // to a live process are ignored by candidates().
    void start();

    [[nodiscard]] std::vector<Candidate> candidates() const;
    [[nodiscard]] Project load(const Candidate& candidate) const;
    void discard(const Candidate& candidate);

    void write(const Project& project, const std::filesystem::path& originalProjectPath);
    void clear();
    void shutdown() noexcept;

    [[nodiscard]] const std::filesystem::path& sessionDirectory() const noexcept {
        return sessionDirectory_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path sessionDirectory_;
    std::filesystem::path boundOriginalProjectPath_;
    bool metadataReady_{};
    bool started_{};
};

}  // namespace listening::recovery
