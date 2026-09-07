#include "core/output_paths.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace listening {
namespace {

constexpr std::size_t maximumProjectIdBytes = 128;

[[nodiscard]] bool isAsciiAlphaNumeric(unsigned char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

[[nodiscard]] bool isStableProjectId(std::string_view value) noexcept {
    if (value.empty() || value.size() > maximumProjectIdBytes ||
        !isAsciiAlphaNumeric(static_cast<unsigned char>(value.front()))) {
        return false;
    }

    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return isAsciiAlphaNumeric(value) || value == '-' || value == '_' || value == '.' ||
               value == ':';
    });
}

}  // namespace

std::filesystem::path defaultOutputDirectory(
    const std::filesystem::path& projectJsonPath,
    const std::filesystem::path& currentWorkingDirectory,
    std::string_view projectId) {
    if (!projectJsonPath.empty()) {
        const std::filesystem::path filename = projectJsonPath.filename();
        if (filename.empty() || filename == "." || filename == "..") {
            throw OutputPathError("projectJsonPath must name a project file");
        }

        std::filesystem::path outputName = filename.stem();
        if (outputName.empty() || outputName == "." || outputName == "..") {
            throw OutputPathError("projectJsonPath must have a usable file stem");
        }
        outputName += "_audio";
        return projectJsonPath.parent_path() / outputName;
    }

    if (currentWorkingDirectory.empty()) {
        throw OutputPathError(
            "currentWorkingDirectory must not be empty for an unsaved project");
    }

    if (projectId.empty()) {
        projectId = defaultUnsavedProjectId;
    } else if (!isStableProjectId(projectId)) {
        throw OutputPathError(
            "projectId must be 1-128 ASCII letters, digits, '.', ':', '_' or '-', "
            "starting with a letter or digit");
    }

    return currentWorkingDirectory / std::filesystem::path(defaultResultsFolderName) /
           std::filesystem::path(projectId);
}

}  // namespace listening
