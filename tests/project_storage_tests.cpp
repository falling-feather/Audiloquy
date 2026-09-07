#include "audio/wav_builder.h"
#include "core/json.h"
#include "core/project_storage.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename T>
void writeLittle(std::ostream& output, T value) {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.put(static_cast<char>((value >> (index * 8U)) & 0xffU));
    }
}

void writePcmWav(const std::filesystem::path& path) {
    constexpr std::uint32_t sampleRate = 44100;
    constexpr std::uint32_t dataBytes = 882;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write("RIFF", 4);
    writeLittle<std::uint32_t>(output, 36 + dataBytes);
    output.write("WAVEfmt ", 8);
    writeLittle<std::uint32_t>(output, 16);
    writeLittle<std::uint16_t>(output, 1);
    writeLittle<std::uint16_t>(output, 1);
    writeLittle<std::uint32_t>(output, sampleRate);
    writeLittle<std::uint32_t>(output, sampleRate * 2);
    writeLittle<std::uint16_t>(output, 2);
    writeLittle<std::uint16_t>(output, 16);
    output.write("data", 4);
    writeLittle<std::uint32_t>(output, dataBytes);
    std::array<char, dataBytes> silence{};
    output.write(silence.data(), silence.size());
    output.close();
    expect(static_cast<bool>(output), "test WAV should be writable");
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

listening::Project draftProject() {
    listening::Project project;
    project.id = "complete-listening-program";
    project.title = "语澜课堂练习";
    project.segments.emplace_back("complete-listening-program",
                                  listening::QuestionRange{3, 3}, "Narrator", "", 3.0, 1);
    return project;
}

void draftSaveAndAtomicReplacement(const std::filesystem::path& root) {
    listening::Project project = draftProject();
    const auto path = root / "草稿" / "工程.json";
    std::filesystem::create_directories(path.parent_path());
    listening::saveProjectAtomic(project, path, listening::ValidationPurpose::Draft);
    const listening::Project restored =
        listening::loadProject(path, listening::ValidationPurpose::Draft);
    expect(restored == project, "draft save/load must preserve blank title/text state");
    expect(!listening::validate(project).empty(), "strict validation must reject the blank draft");

    std::ofstream sentinel(path, std::ios::binary | std::ios::trunc);
    sentinel << "sentinel";
    sentinel.close();
    listening::Project invalid = project;
    invalid.targetWpm = 0;
    try {
        listening::saveProjectAtomic(invalid, path);
    } catch (const listening::ProjectFormatError&) {
        // The invalid serialization must fail before touching the target.
    }
    std::ifstream check(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(check)), {});
    expect(content == "sentinel", "failed atomic save must leave the original file intact");
}

void packageUsesRelativeVerifiedWavAndUniqueDirectory(const std::filesystem::path& root) {
    const auto sourceRoot = root / "原始工程";
    const auto destinationRoot = root / "带走的材料";
    std::filesystem::create_directories(sourceRoot);
    const auto sourceProject = sourceRoot / "课堂工程.json";
    const auto wav = sourceRoot / "声音文件.wav";
    writePcmWav(wav);

    listening::Project project = draftProject();
    project.segments.front().renderedAudioFile = pathUtf8(wav.filename());
    project.renderedProgramFile = pathUtf8(wav.filename());
    listening::storage::save(project, sourceProject, listening::ValidationPurpose::Draft);

    const auto loaded = listening::storage::load(sourceProject,
                                                  listening::ValidationPurpose::Draft);
    expect(loaded.missingResources.empty(), "valid relative WAVs must resolve beside project JSON");

    const auto result = listening::storage::package(project, sourceProject, destinationRoot);
    expect(std::filesystem::is_regular_file(result.projectPath),
           "package must create project.json");
    expect(result.copiedResourceCount == 2, "segment and program WAVs must both be copied");
    const auto packaged = listening::storage::load(
        result.projectPath, listening::ValidationPurpose::Draft);
    expect(packaged.missingResources.empty(), "packaged relative WAVs must round trip");
    expect(packaged.project.segments.front().renderedAudioFile == "audio/segment-1.wav",
           "segment resource names must be independent of punctuation in stable IDs");
    expect(packaged.project.renderedProgramFile == "audio/program.wav",
           "program resource must use an independent package name");
    expect(std::filesystem::is_regular_file(result.directory / "audio/segment-1.wav"),
           "packaged segment WAV must exist");
    expect(std::filesystem::is_regular_file(result.directory / "audio/program.wav"),
           "packaged program WAV must exist");

    const auto saveAsRoot = root / "另存工程";
    std::filesystem::create_directories(saveAsRoot);
    const auto saveAsPath = saveAsRoot / "另存.json";
    const auto prepared = listening::storage::prepareForSave(
        project, sourceProject, saveAsPath);
    listening::storage::save(prepared, saveAsPath, listening::ValidationPurpose::Draft);
    const auto saveAsLoaded = listening::storage::load(
        saveAsPath, listening::ValidationPurpose::Draft);
    expect(saveAsLoaded.missingResources.empty(),
           "Save As must resolve an old relative audio reference before rebasing it");

    const auto second = listening::storage::package(project, sourceProject, destinationRoot);
    expect(second.directory != result.directory &&
               std::filesystem::is_directory(second.directory),
           "a second package must never overwrite the first package");

    const auto movedDirectory = root / "搬运后的材料";
    std::filesystem::rename(result.directory, movedDirectory);
    const auto moved = listening::storage::load(
        movedDirectory / "project.json", listening::ValidationPurpose::Draft);
    expect(moved.missingResources.empty(),
           "a package must remain portable after its containing directory is moved");
}

void replacementFailureLeavesExistingFile(const std::filesystem::path& root) {
#ifdef _WIN32
    const auto path = root / "locked-project.json";
    const auto project = draftProject();
    listening::saveProjectAtomic(project, path, listening::ValidationPurpose::Draft);
    std::ifstream before(path, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(before)), {});
    before.close();

    const HANDLE handle = CreateFileW(
        path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(handle != INVALID_HANDLE_VALUE, "test must be able to lock the project file");
    bool failed = false;
    try {
        listening::saveProjectAtomic(project, path, listening::ValidationPurpose::Draft);
    } catch (const listening::ProjectFormatError&) {
        failed = true;
    }
    CloseHandle(handle);
    expect(failed, "atomic replacement should report a sharing violation");

    std::ifstream after(path, std::ios::binary);
    const std::string retained((std::istreambuf_iterator<char>(after)), {});
    expect(retained == original, "failed replacement must retain the existing project contents");
    const auto filename = path.filename().wstring() + L".tmp-";
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto name = entry.path().filename().wstring();
        expect(name.rfind(filename, 0) != 0, "failed atomic replacement must clean its temp file");
    }
#else
    (void)root;
#endif
}

void invalidResourceFailsBeforePackageCreation(const std::filesystem::path& root) {
    const auto sourceRoot = root / "invalid";
    const auto destinationRoot = root / "invalid-output";
    std::filesystem::create_directories(sourceRoot);
    const auto projectPath = sourceRoot / "工程.json";
    const auto secret = sourceRoot / "密钥.txt";
    std::ofstream(secret) << "do not copy";
    listening::Project project = draftProject();
    project.segments.front().renderedAudioFile = pathUtf8(secret.filename());
    listening::storage::save(project, projectPath, listening::ValidationPurpose::Draft);
    bool failed = false;
    try {
        (void)listening::storage::package(project, projectPath, destinationRoot);
    } catch (const listening::ProjectFormatError&) {
        failed = true;
    }
    expect(failed, "non-WAV resource must block packaging");
    expect(!std::filesystem::exists(destinationRoot),
           "invalid package must not create an output directory");
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "语澜-project-storage-tests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    try {
        draftSaveAndAtomicReplacement(root);
        packageUsesRelativeVerifiedWavAndUniqueDirectory(root);
        invalidResourceFailsBeforePackageCreation(root);
        replacementFailureLeavesExistingFile(root);
        std::filesystem::remove_all(root, ignored);
        std::cout << "project_storage_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, ignored);
        std::cerr << "project_storage_tests failure: " << error.what() << '\n';
        return 1;
    }
}
