#include "core/recovery_store.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

listening::Project draft() {
    listening::Project project;
    project.id = "recovery-project";
    project.title.clear();
    project.segments.emplace_back("segment-1", listening::QuestionRange{1, 1},
                                  "Narrator", "", 2.0, 1);
    return project;
}

void activeSessionIsNotOffered(const std::filesystem::path& root) {
    listening::recovery::Store owner(root);
    owner.start();
    owner.write(draft(), root / "原工程" / "原工程.json");
    listening::recovery::Store observer(root);
    expect(observer.candidates().empty(), "an active process session must not be offered to itself");
    owner.shutdown();

    const auto candidates = observer.candidates();
    expect(candidates.size() == 1, "released recovery session should be discoverable");
    const auto recovered = observer.load(candidates.front());
    expect(recovered.title.empty() && recovered.segments.front().text.empty(),
           "recovery must preserve blank title and text");
    expect(candidates.front().originalProjectPath ==
               root / "原工程" / "原工程.json",
           "recovery must preserve the original project path for resource resolution");

    observer.discard(candidates.front());
    expect(observer.candidates().empty(), "consumed recovery session must not reappear");
    observer.shutdown();
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "语澜-recovery-store-tests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    try {
        activeSessionIsNotOffered(root);
        std::filesystem::remove_all(root, ignored);
        std::cout << "recovery_store_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, ignored);
        std::cerr << "recovery_store_tests failure: " << error.what() << '\n';
        return 1;
    }
}
