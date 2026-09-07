#pragma once

#include "core/project.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace listening {

class ProjectFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Produces UTF-8 JSON. Quotes, backslashes and control characters are escaped;
// valid non-ASCII UTF-8 is preserved as readable Unicode.
[[nodiscard]] std::string toJson(
    const Project& project,
    bool pretty = true,
    ValidationPurpose purpose = ValidationPurpose::Strict);

// Parses JSON strings and both BMP and surrogate-pair \u escapes, then validates
// the complete project before returning it.
[[nodiscard]] Project fromJson(
    std::string_view json,
    ValidationPurpose purpose = ValidationPurpose::Strict);

void saveProject(
    const Project& project,
    const std::filesystem::path& path,
    ValidationPurpose purpose = ValidationPurpose::Strict);
// Writes a complete temporary file beside path, flushes it, and replaces path
// with a platform atomic move. The original remains intact if serialization or
// the move fails.
void saveProjectAtomic(
    const Project& project,
    const std::filesystem::path& path,
    ValidationPurpose purpose = ValidationPurpose::Strict);
[[nodiscard]] Project loadProject(
    const std::filesystem::path& path,
    ValidationPurpose purpose = ValidationPurpose::Strict);

}  // namespace listening
