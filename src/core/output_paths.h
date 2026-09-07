#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace listening {

inline constexpr std::string_view defaultResultsFolderName = "ListeningStudioResults";
inline constexpr std::string_view defaultUnsavedProjectId = "untitled-project";

class OutputPathError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Resolves the default audio output directory without inspecting or changing
// the filesystem.
//
// Saved project:
//   <project-json-parent>/<project-json-stem>_audio
//   currentWorkingDirectory and projectId are intentionally ignored.
//
// Unsaved project:
//   <currentWorkingDirectory>/ListeningStudioResults/<project-id>
//   An empty projectId degrades to "untitled-project". An empty working
//   directory or a non-empty projectId that is not a stable project id throws
//   OutputPathError.
[[nodiscard]] std::filesystem::path defaultOutputDirectory(
    const std::filesystem::path& projectJsonPath,
    const std::filesystem::path& currentWorkingDirectory,
    std::string_view projectId);

}  // namespace listening
