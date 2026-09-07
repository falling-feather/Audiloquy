#include "core/json.h"
#include "core/project.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using listening::Accent;
using listening::Project;
using listening::Segment;

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Exception, typename Function>
void expectThrows(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(message) + " (wrong exception type)");
    }
    fail(message);
}

Project sampleProject() {
    Project project;
    project.id = "demo-project";
    project.title = "\xe8\x8b\xb1\xe8\xaf\xad\xe5\x90\xac\xe5\x8a\x9b \xf0\x9f\x8e\xa7";
    project.accent = Accent::British;
    project.targetWpm = 120.0;
    project.voiceSettings.maleVoiceTokenId = "sapi:male:test";
    project.voiceSettings.femaleVoiceTokenId = "sapi:female:test";
    project.voiceSettings.strictAccent = true;
    project.voiceSettings.allowGenderFallback = false;
    project.renderedProgramFile = "C:/portable/result/complete.wav";
    project.segments = {
        Segment{
            "segment-6-7",
            {6, 7},
            "Teacher \"A\"",
            "She said, \"Hello!\"\\nPath: C:\\\\audio\n\xe4\xbd\xa0\xe5\xa5\xbd, world \xf0\x9f\x98\x83",
            3.5,
            2,
        },
        Segment{
            "segment-8",
            {8, 8},
            "Narrator",
            "One two three four five six seven eight nine ten.",
            1.0,
            1,
        },
    };
    project.segments.front().renderedAudioFile = "C:/portable/result/questions-6-7.wav";
    project.segments.front().generation = listening::GenerationRecord{
        "deepseek",
        "deepseek-chat",
        "When will the tour begin?",
        {"At nine.", "At ten.", "At eleven."},
        "B",
        {listening::GenerationEvidence{"B", "supports", "turn-3", "at ten"}},
        true,
        false,
    };
    return project;
}

void jsonRoundTripPreservesUtf8AndEscapes() {
    const Project source = sampleProject();
    const std::string json = listening::toJson(source);

    expect(json.find("\\\"Hello!\\\"") != std::string::npos, "quotes must be escaped");
    expect(json.find("C:\\\\\\\\audio") != std::string::npos, "backslashes must be escaped");
    expect(json.find("\\n") != std::string::npos, "newlines must be escaped");
    expect(json.find("\xe4\xbd\xa0\xe5\xa5\xbd") != std::string::npos, "valid UTF-8 should remain readable");

    const Project restored = listening::fromJson(json);
    expect(restored == source, "JSON round trip must preserve every domain field");

    const std::string escaped =
        R"({"schemaVersion":1,"id":"unicode-project","title":"\u4f60\u597d \ud83d\ude03","accent":"en-US","targetWpm":100,"segments":[{"id":"s1","questionStart":1,"questionEnd":1,"speaker":"Ren\u00e9e","text":"Line\n\u4e2d\u6587 \ud83c\udfa7","pauseAfterSeconds":0,"repeatCount":1}]})";
    const Project decoded = listening::fromJson(escaped);
    expect(decoded.schemaVersion == Project::currentSchemaVersion,
           "schema 1 documents must migrate to the current schema");
    expect(decoded.voiceSettings.strictAccent &&
               !decoded.voiceSettings.allowGenderFallback,
           "schema 1 migration must use safe voice defaults");
    expect(decoded.title == "\xe4\xbd\xa0\xe5\xa5\xbd \xf0\x9f\x98\x83", "Unicode escapes must decode to UTF-8");
    expect(decoded.segments.front().speaker == "Ren\xc3\xa9" "e", "BMP escape must decode to UTF-8");
    expect(decoded.segments.front().text.find("\n") != std::string::npos, "escaped newline must decode");
}

void fileRoundTripWorks() {
    const Project source = sampleProject();
    const auto path = std::filesystem::temp_directory_path() / "listening-core-roundtrip.json";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};

    listening::saveProject(source, path);
    expect(listening::loadProject(path) == source, "file save/load must round trip");
}

void validationFindsBadInput() {
    Project project = sampleProject();
    expect(listening::validate(project).empty(), "sample project should validate");

    project.targetWpm = 0.0;
    project.segments[1].id = project.segments[0].id;
    project.segments[1].questions = {9, 8};
    const auto issues = listening::validate(project);
    expect(issues.size() >= 3, "invalid WPM, duplicate id and question range must be reported");
    expectThrows<listening::ValidationError>(
        [&] { listening::requireValid(project); },
        "requireValid must reject invalid input");
    expectThrows<listening::ProjectFormatError>(
        [&] { (void)listening::toJson(project); },
        "serialization must not persist invalid projects");

    expectThrows<listening::ProjectFormatError>(
        [] {
            (void)listening::fromJson(
                R"({"schemaVersion":1,"id":"p","title":"x","accent":"en-US","targetWpm":120,"segments":[],})");
        },
        "malformed JSON must be rejected");
}

void durationsAreDeterministic() {
    std::string sixtyWords;
    for (int index = 0; index < 60; ++index) {
        if (!sixtyWords.empty()) {
            sixtyWords.push_back(' ');
        }
        sixtyWords += "word";
    }

    expect(listening::countReadableWords("Don't stop - well-known, 2026!") == 5, "word counting should be predictable");
    expect(
        listening::countReadableWords("We\xe2\x80\x99re ready") == 2,
        "a typographic apostrophe must remain inside an English word");
    expect(listening::estimateReadingDuration(sixtyWords, 120.0) == 30s, "60 words at 120 WPM is 30 seconds");

    Segment repeated{"s1", {1, 1}, "A", sixtyWords, 2.5, 2};
    expect(
        listening::estimateSegmentDuration(repeated, 120.0) == 65s,
        "segment duration must include speech and a pause after each play");

    Project project;
    project.id = "duration-project";
    project.title = "Duration";
    project.targetWpm = 120.0;
    project.segments = {
        repeated,
        Segment{"s2", {2, 2}, "B", sixtyWords, 5.0, 1},
    };
    expect(listening::estimateProjectDuration(project) == 100s, "project duration must sum every segment");
}

void stableIdLookupWorks() {
    Project project = sampleProject();
    Segment* segment = project.findSegment("segment-8");
    expect(segment != nullptr && segment->questions.first == 8, "mutable lookup must find stable id");
    segment->speaker = "Updated";

    const Project& constant = project;
    const Segment* found = constant.findSegment("segment-8");
    expect(found != nullptr && found->speaker == "Updated", "const lookup must return the same segment");
    expect(constant.findSegment("missing") == nullptr, "missing stable id must return nullptr");
}

}  // namespace

int main() {
    try {
        jsonRoundTripPreservesUtf8AndEscapes();
        fileRoundTripWorks();
        validationFindsBadInput();
        durationsAreDeterministic();
        stableIdLookupWorks();
        std::cout << "All core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Core test failure: " << error.what() << '\n';
        return 1;
    }
}
