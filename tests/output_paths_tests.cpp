#include "core/output_paths.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Function>
void expectPathError(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const listening::OutputPathError&) {
        return;
    } catch (...) {
        fail(std::string(message) + " (wrong exception type)");
    }
    fail(message);
}

void savedProjectUsesJsonSibling() {
    const std::filesystem::path json =
        std::filesystem::path("workspace") / "projects" / "lesson.project.json";
    const auto result = listening::defaultOutputDirectory(json, {}, {});
    const auto expected =
        std::filesystem::path("workspace") / "projects" / "lesson.project_audio";
    expect(result == expected, "saved project output must be beside JSON and use its stem");

    const auto extensionless = listening::defaultOutputDirectory(
        std::filesystem::path("workspace") / "lesson", "ignored", "ignored");
    expect(
        extensionless == std::filesystem::path("workspace") / "lesson_audio",
        "an extensionless saved project path must still gain the _audio suffix");

    const auto relative = listening::defaultOutputDirectory("lesson.json", {}, {});
    expect(relative == "lesson_audio", "a relative saved JSON path must remain relative");
}

void unsavedProjectUsesFixedResultsFolder() {
    const std::filesystem::path cwd = std::filesystem::path("workspace") / "run";
    const auto result = listening::defaultOutputDirectory({}, cwd, "project-42");
    const auto expected = cwd / "ListeningStudioResults" / "project-42";
    expect(result == expected, "unsaved project output must use the fixed results folder");

    const auto fallback = listening::defaultOutputDirectory({}, cwd, {});
    expect(
        fallback == cwd / "ListeningStudioResults" / "untitled-project",
        "empty unsaved project id must use the documented fallback");
}

void invalidInputsFailClearly() {
    expectPathError(
        [] { (void)listening::defaultOutputDirectory({}, {}, "project-1"); },
        "empty cwd must be rejected for an unsaved project");
    expectPathError(
        [] { (void)listening::defaultOutputDirectory({}, "workspace", "../escape"); },
        "path traversal must not be accepted as a project id");
    expectPathError(
        [] {
            (void)listening::defaultOutputDirectory(
                std::filesystem::path("workspace") / "projects" / "", {}, {});
        },
        "a saved path without a filename must be rejected");
}

}  // namespace

int main() {
    try {
        savedProjectUsesJsonSibling();
        unsavedProjectUsesFixedResultsFolder();
        invalidInputsFailClearly();
        std::cout << "All output path tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Output path test failure: " << error.what() << '\n';
        return 1;
    }
}
