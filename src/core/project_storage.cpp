#include "core/project_storage.h"

#include "audio/wav_builder.h"
#include "core/json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace listening::storage {
namespace {

[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view value) {
#ifdef _WIN32
    if (value.empty()) {
        return {};
    }
    const int sourceLength = static_cast<int>(value.size());
    const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                value.data(), sourceLength, nullptr, 0);
    if (wideLength <= 0) {
        throw ProjectFormatError("Invalid UTF-8 resource path");
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength,
                            wide.data(), wideLength) != wideLength) {
        throw ProjectFormatError("Invalid UTF-8 resource path");
    }
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(std::string(value));
#endif
}

[[nodiscard]] std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool isWavPath(const std::filesystem::path& path) {
    return lowerAscii(path.extension().string()) == ".wav";
}

[[nodiscard]] std::string resourceDescription(const MissingResource& resource) {
    std::ostringstream output;
    output << resource.kind << " resource '" << resource.reference << "'";
    if (!resource.resolvedPath.empty()) {
        output << " (resolved to " << resource.resolvedPath.string() << ')';
    }
    return output.str();
}

[[nodiscard]] std::string sanitizeDirectoryName(std::string value) {
    if (value.empty()) {
        value = "audiloquy-project";
    }
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        const bool invalid = character == '<' || character == '>' || character == ':' ||
                             character == '"' || character == '/' || character == '\\' ||
                             character == '|' || character == '?' || character == '*';
        result.push_back(invalid || character < 0x20U ? '_' : static_cast<char>(character));
    }
    while (!result.empty() && (result.back() == '.' || result.back() == ' ')) {
        result.pop_back();
    }
    return result.empty() ? "audiloquy-project" : result;
}

[[nodiscard]] std::filesystem::path uniqueChildDirectory(
    const std::filesystem::path& destinationRoot,
    std::string baseName) {
    std::error_code error;
    std::filesystem::create_directories(destinationRoot, error);
    if (error) {
        throw ProjectFormatError("Cannot create package destination: " + error.message());
    }

    for (std::size_t suffix = 0; suffix < 100000; ++suffix) {
        std::string candidateName = baseName;
        if (suffix != 0) {
            candidateName += '-' + std::to_string(suffix + 1);
        }
        const auto candidate = destinationRoot / pathFromUtf8(candidateName);
        error.clear();
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error) {
            throw ProjectFormatError("Cannot create package directory: " + error.message());
        }
    }
    throw ProjectFormatError("Cannot find an unused package directory");
}

void validateAudioReference(
    const std::filesystem::path& projectPath,
    std::string_view reference,
    std::string_view kind) {
    if (reference.empty()) {
        return;
    }
    const auto resolved = resolveResource(projectPath, reference);
    if (!std::filesystem::is_regular_file(resolved)) {
        throw ProjectFormatError(resourceDescription(MissingResource{
            std::string(kind), std::string(reference), resolved}));
    }
    if (!isWavPath(resolved)) {
        throw ProjectFormatError(std::string(kind) + " resource must be a .wav file: " +
                                 resolved.string());
    }
    audio::WavInfo info;
    std::string error;
    if (!audio::inspectPcmWav(resolved, &info, &error)) {
        throw ProjectFormatError(std::string(kind) + " resource is not a valid PCM WAV: " +
                                 (error.empty() ? resolved.string() : error));
    }
}

[[nodiscard]] std::string resourceReference(const std::filesystem::path& relative) {
    const auto utf8 = relative.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

}  // namespace

std::filesystem::path resolveResource(
    const std::filesystem::path& projectPath,
    std::string_view reference) {
    const auto stored = pathFromUtf8(reference);
    if (stored.empty() || stored.is_absolute() || projectPath.empty()) {
        return stored;
    }
    return projectPath.parent_path() / stored;
}

LoadResult load(const std::filesystem::path& projectPath, ValidationPurpose purpose) {
    LoadResult result;
    result.project = listening::loadProject(projectPath, purpose);
    for (const Segment& segment : result.project.segments) {
        if (segment.recording) {
            try {
                validateAudioReference(projectPath, segment.recording->audioFile, "source recording");
            } catch (const ProjectFormatError&) {
                result.missingResources.push_back({"source recording", segment.recording->audioFile,
                    resolveResource(projectPath, segment.recording->audioFile)});
            }
        }
        if (!segment.renderedAudioFile.empty()) {
            const auto resolved = resolveResource(projectPath, segment.renderedAudioFile);
            if (!std::filesystem::is_regular_file(resolved)) {
                result.missingResources.push_back(MissingResource{
                    "segment audio", segment.renderedAudioFile, resolved});
            } else if (!isWavPath(resolved)) {
                result.missingResources.push_back(MissingResource{
                    "segment audio (not .wav)", segment.renderedAudioFile, resolved});
            } else {
                audio::WavInfo info;
                std::string error;
                if (!audio::inspectPcmWav(resolved, &info, &error)) {
                    result.missingResources.push_back(MissingResource{
                        "segment audio (invalid PCM WAV)", segment.renderedAudioFile, resolved});
                }
            }
        }
    }
    if (!result.project.renderedProgramFile.empty()) {
        const auto resolved = resolveResource(projectPath, result.project.renderedProgramFile);
        if (!std::filesystem::is_regular_file(resolved)) {
            result.missingResources.push_back(MissingResource{
                "complete program", result.project.renderedProgramFile, resolved});
        } else if (!isWavPath(resolved)) {
            result.missingResources.push_back(MissingResource{
                "complete program (not .wav)", result.project.renderedProgramFile, resolved});
        } else {
            audio::WavInfo info;
            std::string error;
            if (!audio::inspectPcmWav(resolved, &info, &error)) {
                result.missingResources.push_back(MissingResource{
                    "complete program (invalid PCM WAV)", result.project.renderedProgramFile,
                    resolved});
            }
        }
    }
    return result;
}

void save(const Project& project,
          const std::filesystem::path& projectPath,
          ValidationPurpose purpose) {
    listening::saveProjectAtomic(project, projectPath, purpose);
}

Project prepareForSave(const Project& project,
                       const std::filesystem::path& sourceProjectPath,
                       const std::filesystem::path& targetProjectPath) {
    Project prepared = project;
    const auto rebase = [&](const std::string& reference) {
        if (reference.empty()) {
            return reference;
        }
        auto resolved = resolveResource(sourceProjectPath, reference);
        if (resolved.is_relative()) {
            std::error_code error;
            resolved = std::filesystem::absolute(resolved, error);
            if (error) {
                throw ProjectFormatError("Cannot resolve audio reference during Save As: " +
                                         reference);
            }
        }
        if (targetProjectPath.empty()) {
            return reference;
        }
        std::error_code targetError;
        const auto targetAbsolute = std::filesystem::absolute(targetProjectPath, targetError);
        if (targetError) {
            throw ProjectFormatError("Cannot resolve target project path during Save As: " +
                                     targetError.message());
        }
        std::error_code error;
        const auto relative = std::filesystem::relative(
            resolved, targetAbsolute.parent_path(), error);
        if (error || relative.empty()) {
            return resourceReference(resolved);
        }
        return resourceReference(relative);
    };
    for (Segment& segment : prepared.segments) {
        segment.renderedAudioFile = rebase(segment.renderedAudioFile);
        if (segment.recording) segment.recording->audioFile = rebase(segment.recording->audioFile);
    }
    prepared.renderedProgramFile = rebase(prepared.renderedProgramFile);
    return prepared;
}

PackageResult package(const Project& project,
                      const std::filesystem::path& sourceProjectPath,
                      const std::filesystem::path& destinationRoot) {
    // Package accepts drafts so the teacher can move an unfinished lesson
    // between machines. It still validates every referenced output as a WAV
    // before creating the new directory.
    requireValid(project, ValidationPurpose::Draft);

    for (const Segment& segment : project.segments) {
        validateAudioReference(sourceProjectPath, segment.renderedAudioFile,
                               "segment audio");
        if (segment.recording) validateAudioReference(sourceProjectPath, segment.recording->audioFile, "source recording");
    }
    validateAudioReference(sourceProjectPath, project.renderedProgramFile,
                           "complete program");

    const std::string title = project.title.empty() ? project.id : project.title;
    const auto packageDirectory = uniqueChildDirectory(
        destinationRoot, sanitizeDirectoryName(title) + "-audiloquy");
    bool keepDirectory = false;
    try {
        const auto audioDirectory = packageDirectory / "audio";
        std::error_code error;
        std::filesystem::create_directory(audioDirectory, error);
        if (error) {
            throw ProjectFormatError("Cannot create package audio directory: " + error.message());
        }

        Project packaged = project;
        std::size_t copied = 0;
        std::map<std::filesystem::path, std::string> sourceReferences;
        for (std::size_t index = 0; index < packaged.segments.size(); ++index) {
            Segment& segment = packaged.segments[index];
            if (segment.recording) {
                const auto source = std::filesystem::weakly_canonical(resolveResource(sourceProjectPath, segment.recording->audioFile));
                auto found = sourceReferences.find(source);
                if (found == sourceReferences.end()) {
                    const std::string filename = "source-" + std::to_string(sourceReferences.size()+1) + ".wav";
                    std::filesystem::copy_file(source, audioDirectory / filename);
                    found = sourceReferences.emplace(source, "audio/"+filename).first;
                    ++copied;
                }
                segment.recording->audioFile = found->second;
            }
            if (segment.renderedAudioFile.empty()) {
                continue;
            }
            const auto source = resolveResource(sourceProjectPath, segment.renderedAudioFile);
            const auto target = audioDirectory /
                                ("segment-" + std::to_string(index + 1) + ".wav");
            std::filesystem::copy_file(source, target, std::filesystem::copy_options::none,
                                       error);
            if (error) {
                throw ProjectFormatError("Cannot copy segment audio: " + error.message());
            }
            segment.renderedAudioFile = resourceReference(
                std::filesystem::path("audio") /
                ("segment-" + std::to_string(index + 1) + ".wav"));
            ++copied;
        }
        if (!packaged.renderedProgramFile.empty()) {
            const auto source = resolveResource(sourceProjectPath, packaged.renderedProgramFile);
            const auto target = audioDirectory / "program.wav";
            std::filesystem::copy_file(source, target, std::filesystem::copy_options::none,
                                       error);
            if (error) {
                throw ProjectFormatError("Cannot copy complete program audio: " + error.message());
            }
            packaged.renderedProgramFile =
                resourceReference(std::filesystem::path("audio") / "program.wav");
            ++copied;
        }
        const auto projectPath = packageDirectory / "project.json";
        listening::saveProjectAtomic(packaged, projectPath, ValidationPurpose::Draft);
        keepDirectory = true;
        return PackageResult{packageDirectory, projectPath, copied};
    } catch (...) {
        if (!keepDirectory) {
            std::error_code ignored;
            std::filesystem::remove_all(packageDirectory, ignored);
        }
        throw;
    }
}

}  // namespace listening::storage
