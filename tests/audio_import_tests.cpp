#include "app/audio_import.h"
#include "core/project_storage.h"
#include "core/json.h"
#include "app/render_job.h"
#include "platform/windows/windows_audio.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool b,const char* what){if(!b)throw std::runtime_error(what);}
template<class T>void put(std::ostream& o,T n){for(unsigned i=0;i<sizeof(T);++i)o.put(static_cast<char>((n>>(8*i))&255));}
void fixture(const std::filesystem::path& p){
    std::ofstream o(p,std::ios::binary);o.write("RIFF",4);put<std::uint32_t>(o,36+192000);o.write("WAVEfmt ",8);
    put<std::uint32_t>(o,16);put<std::uint16_t>(o,1);put<std::uint16_t>(o,2);put<std::uint32_t>(o,48000);put<std::uint32_t>(o,192000);
    put<std::uint16_t>(o,4);put<std::uint16_t>(o,16);o.write("data",4);put<std::uint32_t>(o,192000);
    for(unsigned i=0;i<48000;++i){put<std::int16_t>(o,static_cast<std::int16_t>((i%100)*100));put<std::int16_t>(o,0);}
}
std::string utf8(const std::filesystem::path& p){auto s=p.generic_u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv); QTemporaryDir tmp;
    try {
        expect(tmp.isValid(),"temp");
        const std::filesystem::path root(tmp.path().toStdWString());
        const auto source=root/L"原录音.wav";fixture(source);
        listening::app::ImportedAudio imported;std::string error;
        expect(listening::app::importRecording(source,root/L"来源",&imported,&error),error.c_str());
        expect(imported.durationMs>=990 && imported.durationMs<=1010,"resampled duration");
        listening::platform::windows::WavInfo info;
        expect(listening::platform::windows::inspectPcmWav(imported.path,&info,&error),"valid normalized WAV");
        expect(info.format.sampleRate==44100 && info.format.channels==1 && info.format.bitsPerSample==16,"normalized format");
        const auto clip=root/"clip.wav";
        expect(listening::app::extractRecording(imported.path,200,800,clip,&error),error.c_str());
        expect(listening::platform::windows::inspectPcmWav(clip,&info,&error) && info.frameCount==26460,"exact half-open clip frames");
        expect(!listening::app::extractRecording(imported.path,900,1200,clip,&error),"reject end outside recording");
        expect(listening::platform::windows::inspectPcmWav(clip,&info,&error) && info.frameCount==26460,"failed trim preserves output");
        expect(!listening::app::importRecording(source,root/"cancelled",&imported,&error,[]{return true;}),"cancelled import");
        listening::Project p;p.id="recording";p.title="Recording";
        p.segments.emplace_back("s1",listening::QuestionRange{1,1},"Narrator","",0.1,2);
        p.segments[0].recording=listening::RecordingSource{utf8(imported.path),200,800};
        expect(listening::estimateProjectDuration(p).count()==1400,"recording duration uses range not WPM");
        listening::app::RenderJobRequest request;request.project=p;request.outputDirectory=root/"render";
        listening::app::RenderCancellationToken cancel;
        const auto result=listening::app::RenderJobRunner::run(request,cancel);
        expect(result.success && result.voiceUses.empty() && !result.programPath.empty(),result.error.c_str());
        expect(listening::platform::windows::inspectPcmWav(result.programPath,&info,&error) && info.frameCount==61740,"recorded program repetitions");
        p.segments[0].renderedAudioFile=utf8(result.completedSegments[0].path);p.renderedProgramFile=utf8(result.programPath);
        const auto bundle=listening::storage::package(p,{},root/"packages");
        const auto moved=root/L"搬移";std::filesystem::rename(bundle.directory,moved);
        const auto loaded=listening::storage::load(moved/"project.json");
        expect(loaded.missingResources.empty() && loaded.project.segments[0].recording.has_value(),"recording source portable");
        request.project=loaded.project;request.projectFile=moved/"project.json";request.outputDirectory=root/"rerender";
        expect(listening::app::RenderJobRunner::run(request,cancel).success,"render moved relative recording source");
        if(argc>1){const auto compressed=std::filesystem::path(QString::fromLocal8Bit(argv[1]).toStdWString());expect(listening::app::importRecording(compressed,root/"mp3",&imported,&error),error.c_str());}
        std::cout<<"audio_import_tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
