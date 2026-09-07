#include "core/recovery_store.h"

#include "core/json.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace listening::recovery {
namespace {

[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& value) {
#ifdef _WIN32
    const std::wstring wide = value.wstring();
    if (wide.empty()) {
        return {};
    }
    const int byteCount = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                               static_cast<int>(wide.size()), nullptr, 0,
                                               nullptr, nullptr);
    if (byteCount <= 0) {
        throw ProjectFormatError("Cannot encode recovery project path as UTF-8");
    }
    std::string result(static_cast<std::size_t>(byteCount), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), result.data(), byteCount,
                            nullptr, nullptr) != byteCount) {
        throw ProjectFormatError("Cannot encode recovery project path as UTF-8");
    }
    return result;
#else
    const auto utf8 = value.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#endif
}

[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view value) {
#ifdef _WIN32
    if (value.empty()) {
        return {};
    }
    const int wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               value.data(), static_cast<int>(value.size()),
                                               nullptr, 0);
    if (wideCount <= 0) {
        throw ProjectFormatError("Invalid UTF-8 recovery project path");
    }
    std::wstring wide(static_cast<std::size_t>(wideCount), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), wide.data(), wideCount) != wideCount) {
        throw ProjectFormatError("Invalid UTF-8 recovery project path");
    }
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(std::string(value));
#endif
}

[[nodiscard]] std::uint64_t currentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

[[nodiscard]] bool processIsAlive(std::uint64_t processId) noexcept {
    if (processId == 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                static_cast<DWORD>(processId));
    if (handle == nullptr) {
        // Access denied is treated as alive; prompting recovery could race a
        // process we cannot inspect.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exitCode = 0;
    const bool queried = GetExitCodeProcess(handle, &exitCode) != FALSE;
    CloseHandle(handle);
    return queried && exitCode == STILL_ACTIVE;
#else
    if (kill(static_cast<pid_t>(processId), 0) == 0) {
        return true;
    }
    return errno == EPERM;
#endif
}

[[nodiscard]] bool readOwner(
    const std::filesystem::path& directory,
    std::uint64_t& processId) noexcept {
    std::ifstream input(directory / "owner.pid", std::ios::binary);
    if (!input) {
        return false;
    }
    input >> processId;
    return static_cast<bool>(input);
}

[[nodiscard]] std::filesystem::path candidateSessionDirectory(
    const std::filesystem::path& root) {
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto threadPart = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return root / ("session-" + std::to_string(currentProcessId()) + "-" +
                   std::to_string(timestamp) + "-" + std::to_string(threadPart));
}

[[nodiscard]] bool readOriginalPath(
    const std::filesystem::path& directory,
    std::filesystem::path& path) noexcept {
    std::ifstream input(directory / "original-project-path", std::ios::binary);
    if (!input) {
        return false;
    }
    std::string lengthLine;
    if (!std::getline(input, lengthLine)) {
        return false;
    }
    if (lengthLine.empty() || lengthLine.size() > 6) {
        return false;
    }
    std::size_t length = 0;
    for (const unsigned char digit : lengthLine) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        length = length * 10 + static_cast<std::size_t>(digit - '0');
        if (length > 32 * 1024) {
            return false;
        }
    }
    std::string value(length, '\0');
    if (length != 0 && !input.read(value.data(), static_cast<std::streamsize>(length))) {
        return false;
    }
    try {
        path = pathFromUtf8(value);
    } catch (...) {
        return false;
    }
    return true;
}

void writeOriginalPathAtomic(const std::filesystem::path& directory,
                             const std::filesystem::path& originalPath) {
    const std::filesystem::path destination = directory / "original-project-path";
    const std::filesystem::path temporary = directory / "original-project-path.tmp";
    const std::string original = pathToUtf8(originalPath);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw ProjectFormatError("Cannot open recovery project path metadata");
    }
    output << original.size() << '\n';
    if (!original.empty()) {
        output.write(original.data(), static_cast<std::streamsize>(original.size()));
    }
    output.flush();
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw ProjectFormatError("Cannot write recovery project path metadata");
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), destination.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD errorCode = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw ProjectFormatError("Cannot publish recovery metadata (Win32 error " +
                                 std::to_string(errorCode) + ")");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw ProjectFormatError("Cannot publish recovery metadata: " + error.message());
    }
#endif
}

[[nodiscard]] bool isOwnedCandidate(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) noexcept {
    return !root.empty() && !candidate.empty() && candidate.filename().string().rfind("session-", 0) == 0 &&
           candidate.parent_path().lexically_normal() == root.lexically_normal();
}

}  // namespace

Store::Store(std::filesystem::path root) : root_(std::move(root)) {}

Store::~Store() {
    shutdown();
}

void Store::start() {
    if (started_) {
        return;
    }
    if (root_.empty()) {
        throw ProjectFormatError("Recovery store root is empty");
    }
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) {
        throw ProjectFormatError("Cannot create recovery store: " + error.message());
    }
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = candidateSessionDirectory(root_);
        error.clear();
        if (!std::filesystem::create_directory(candidate, error)) {
            if (error) {
                throw ProjectFormatError("Cannot create recovery session: " + error.message());
            }
            continue;
        }
        sessionDirectory_ = candidate;
        started_ = true;
        std::ofstream owner(sessionDirectory_ / "owner.pid", std::ios::binary | std::ios::trunc);
        owner << currentProcessId() << '\n';
        owner.close();
        if (!owner) {
            shutdown();
            throw ProjectFormatError("Cannot claim recovery session");
        }
        return;
    }
    throw ProjectFormatError("Cannot allocate a recovery session");
}

std::vector<Candidate> Store::candidates() const {
    std::vector<Candidate> result;
    if (root_.empty() || !std::filesystem::is_directory(root_)) {
        return result;
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it(root_, error), end; it != end && !error;
         it.increment(error)) {
        if (!it->is_directory(error)) {
            continue;
        }
        const auto directory = it->path();
        if (started_ && directory == sessionDirectory_) {
            continue;
        }
        std::uint64_t owner = 0;
        if (readOwner(directory, owner) && processIsAlive(owner)) {
            continue;
        }
        const auto draftPath = directory / "draft.json";
        if (!std::filesystem::is_regular_file(draftPath)) {
            continue;
        }
        std::filesystem::path originalPath;
        if (!readOriginalPath(directory, originalPath)) {
            continue;
        }
        result.push_back(Candidate{directory, draftPath, std::move(originalPath)});
    }
    return result;
}

Project Store::load(const Candidate& candidate) const {
    if (candidate.draftPath.empty() || !std::filesystem::is_regular_file(candidate.draftPath)) {
        throw ProjectFormatError("Recovery draft does not exist");
    }
    return listening::loadProject(candidate.draftPath, ValidationPurpose::Draft);
}

void Store::write(const Project& project, const std::filesystem::path& originalProjectPath) {
    if (!started_) {
        start();
    }
    if (metadataReady_ && boundOriginalProjectPath_ != originalProjectPath) {
        clear();
        start();
    }
    if (!metadataReady_) {
        writeOriginalPathAtomic(sessionDirectory_, originalProjectPath);
        boundOriginalProjectPath_ = originalProjectPath;
        metadataReady_ = true;
    }
    listening::saveProjectAtomic(project, sessionDirectory_ / "draft.json",
                                 ValidationPurpose::Draft);
}

void Store::discard(const Candidate& candidate) {
    if (!isOwnedCandidate(root_, candidate.directory)) {
        throw ProjectFormatError("Recovery candidate is outside the recovery store");
    }
    std::error_code error;
    std::filesystem::remove_all(candidate.directory, error);
    if (error) {
        throw ProjectFormatError("Cannot discard recovery draft: " + error.message());
    }
}

void Store::clear() {
    if (!started_ || sessionDirectory_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(sessionDirectory_, error);
    if (error) {
        throw ProjectFormatError("Cannot clear recovery draft: " + error.message());
    }
    started_ = false;
    sessionDirectory_.clear();
    boundOriginalProjectPath_.clear();
    metadataReady_ = false;
}

void Store::shutdown() noexcept {
    if (!started_ || sessionDirectory_.empty()) {
        return;
    }
    std::error_code ignored;
    // Release ownership while preserving an unfinished draft. A process can
    // terminate without reaching MainWindow::closeEvent; the next launch can
    // then offer this record for recovery.
    std::filesystem::remove(sessionDirectory_ / "owner.pid", ignored);
    started_ = false;
    sessionDirectory_.clear();
    boundOriginalProjectPath_.clear();
    metadataReady_ = false;
}

}  // namespace listening::recovery
