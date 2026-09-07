#pragma once

#include "core/project.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace listening::storage {

struct MissingResource {
    std::string kind;
    std::string reference;
    std::filesystem::path resolvedPath;
};

struct LoadResult {
    Project project;
    std::vector<MissingResource> missingResources;
};

struct PackageResult {
    std::filesystem::path directory;
    std::filesystem::path projectPath;
    std::size_t copiedResourceCount{};
};

// Resource references in schema 2 may be absolute (legacy projects) or
// relative to the project JSON. This helper is the single resolution rule for
// opening and packaging projects.
[[nodiscard]] std::filesystem::path resolveResource(
    const std::filesystem::path& projectPath,
    std::string_view reference);

[[nodiscard]] LoadResult load(
    const std::filesystem::path& projectPath,
    ValidationPurpose purpose = ValidationPurpose::Draft);

void save(
    const Project& project,
    const std::filesystem::path& projectPath,
    ValidationPurpose purpose = ValidationPurpose::Draft);

// Rewrites relative/absolute audio references against a new project file
// location. Existing relative references are first resolved against
// sourceProjectPath, so Save As does not silently point at the wrong folder.
[[nodiscard]] Project prepareForSave(
    const Project& project,
    const std::filesystem::path& sourceProjectPath,
    const std::filesystem::path& targetProjectPath);

// Creates a new child directory under destinationRoot, copies every referenced
// PCM WAV into audio/, writes project.json with relative references, and never
// overwrites an existing child directory. Any invalid or missing reference
// fails before the caller's source project is changed.
[[nodiscard]] PackageResult package(
    const Project& project,
    const std::filesystem::path& sourceProjectPath,
    const std::filesystem::path& destinationRoot);

}  // namespace listening::storage
