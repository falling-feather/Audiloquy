#include "audio/wav_builder.h"
#include "core/project.h"
#include "platform/windows/windows_audio.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main() {
    namespace audio = listening::audio;
    namespace win = listening::platform::windows;

    try {
        listening::Project project;
        project.id = "integration-demo";
        project.title = "Integration demo";
        project.accent = listening::Accent::American;
        project.targetWpm = 118;
        project.segments = {
            {"questions-1-2", {1, 2}, "Woman",
             "Welcome to the school library. It opens at eight thirty on weekdays.", 1.0, 1},
            {"questions-3-4", {3, 4}, "Man",
             "The English club meets every Wednesday after class in Room 204.", 1.0, 2},
        };
        listening::requireValid(project);
        const auto root = std::filesystem::temp_directory_path() /
                          "english-listening-prototype-integration";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);

        std::vector<audio::ProgramClip> program;
        for (const auto& segment : project.segments) {
            const auto raw = root / (segment.id + ".raw.wav");
            win::SynthesisRequest request;
            request.textUtf8 = segment.text;
            request.accent = project.accent == listening::Accent::American
                                 ? win::Accent::AmericanEnglish
                                 : win::Accent::BritishEnglish;
            request.targetWpm = static_cast<int>(std::lround(project.targetWpm));
            request.outputWav = raw;

            win::SynthesisResult result;
            std::string error;
            if (!win::synthesizeToPcmWav(request, &result, &error)) {
                std::cerr << "Synthesis failed for " << segment.id << ": " << error << '\n';
                return 2;
            }
            program.push_back(audio::ProgramClip{
                raw,
                segment.repeatCount,
                static_cast<int>(std::lround(segment.pauseAfterSeconds * 1000.0)),
            });
        }

        const auto complete = root / "complete-listening-program.wav";
        audio::WavInfo info;
        std::string error;
        if (!audio::buildProgramWav(program, complete, &info, &error)) {
            std::cerr << "Program assembly failed: " << error << '\n';
            return 3;
        }
        if (!std::filesystem::is_regular_file(complete) || info.sampleRate != 44100 ||
            info.channels != 1 || info.bitsPerSample != 16 || info.frameCount == 0) {
            std::cerr << "Complete program does not satisfy the prototype PCM contract\n";
            return 4;
        }

        std::filesystem::remove_all(root, ignored);
        std::cout << "prototype_integration_tests passed: " << project.segments.size()
                  << " semantic groups rendered\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Integration failure: " << error.what() << '\n';
        return 5;
    }
}
