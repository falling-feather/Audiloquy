// Audiloquy adapter for the separately installed upstream Kokoro CLI.
// This program communicates with the engine through command-line text and WAV
// files; it does not link or redistribute the engine's GPL dependencies.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
struct Handle {
    HANDLE h{};
    ~Handle(){if(h && h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
};
std::wstring quote(const std::wstring& value) {
    std::wstring result=L"\"";
    unsigned slashes=0;
    for(wchar_t c:value) {
        if(c==L'\\'){++slashes;continue;}
        if(c==L'\"'){result.append(slashes*2+1,L'\\');result+=c;}
        else {result.append(slashes,L'\\');result+=c;}
        slashes=0;
    }
    result.append(slashes*2,L'\\'); result+=L'\"';return result;
}
std::wstring englishText(const std::string& text) {
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(n<=0)throw std::runtime_error("Invalid UTF-8 listening text");
    std::wstring wide(n,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),wide.data(),n);
    const int capacity=NormalizeString(NormalizationKD,wide.data(),n,nullptr,0);
    if(capacity<=0)throw std::runtime_error("Cannot normalize English punctuation");
    std::wstring normalized(capacity,L'\0');
    const int actual=NormalizeString(NormalizationKD,wide.data(),n,normalized.data(),capacity);
    if(actual<=0)throw std::runtime_error("Cannot normalize English text");
    normalized.resize(actual);
    std::wstring result;
    for(auto c:normalized) {
        if(c>=0x300 && c<=0x36f)continue;
        if(c==0x2018 || c==0x2019)c=L'\'';
        if(c==0x201c || c==0x201d)c=L'"';
        if(c>=0x2010 && c<=0x2015)c=L'-';
        if(c==0xa0)c=L' ';
        if(c==L'\r'||c==L'\n'||c==L'\t')c=L' ';
        if(c<32 || c>126)throw std::runtime_error("This voice pack supports English text and Latin names; remove non-English symbols before synthesis");
        result+=c;
    }
    if(result.empty())throw std::runtime_error("Listening text is empty");
    return result;
}
template<class T> T read(std::istream& input) {
    std::uint64_t n=0;
    for(unsigned i=0;i<sizeof(T);++i){int c=input.get();if(c<0)throw std::runtime_error("Truncated engine WAV");n|=std::uint64_t(c)<<(i*8);}
    return static_cast<T>(n);
}
template<class T> void write(std::ostream& output,T n) {
    for(unsigned i=0;i<sizeof(T);++i)output.put(static_cast<char>((static_cast<std::uint64_t>(n)>>(i*8))&255));
}
std::vector<std::int16_t> readSamples(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);char tag[4]{};
    in.read(tag,4);if(std::string(tag,4)!="RIFF")throw std::runtime_error("Engine output is not WAV");
    read<std::uint32_t>(in);in.read(tag,4);if(std::string(tag,4)!="WAVE")throw std::runtime_error("Engine output is not WAVE");
    bool format=false;
    while(in.read(tag,4)) {
        const auto size=read<std::uint32_t>(in); const auto start=in.tellg();
        if(std::string(tag,4)=="fmt ") {
            if(size<16 || read<std::uint16_t>(in)!=1 || read<std::uint16_t>(in)!=1 || read<std::uint32_t>(in)!=24000)
                throw std::runtime_error("Expected mono 24 kHz engine PCM");
            read<std::uint32_t>(in);read<std::uint16_t>(in);
            if(read<std::uint16_t>(in)!=16)throw std::runtime_error("Expected PCM16 engine output");
            format=true;
        } else if(std::string(tag,4)=="data") {
            if(!format || !size || size%2 || size>64*1024*1024)throw std::runtime_error("Invalid engine audio payload");
            std::vector<std::int16_t> samples(size/2);
            if(!in.read(reinterpret_cast<char*>(samples.data()),size))throw std::runtime_error("Incomplete engine audio");
            return samples;
        }
        in.seekg(start+static_cast<std::streamoff>(size)+static_cast<std::streamoff>(size&1U));
    }
    throw std::runtime_error("Engine returned no audio");
}
void engine(const fs::path& root,const std::wstring& text,int sid,bool british,double scale,const fs::path& output) {
    const fs::path executable=root/L"runtime/bin/sherpa-onnx-offline-tts.exe";
    std::vector<std::wstring> args={executable.wstring(),L"--kokoro-model=model/model.int8.onnx",L"--kokoro-voices=model/voices.bin",
        L"--kokoro-tokens=model/tokens.txt",L"--kokoro-data-dir=model/espeak-ng-data",
        british?L"--kokoro-lexicon=model/lexicon-gb-en.txt":L"--kokoro-lexicon=model/lexicon-us-en.txt",
        L"--num-threads=2",L"--sid="+std::to_wstring(sid),L"--kokoro-length-scale="+std::to_wstring(scale),
        L"--output-filename="+fs::relative(output,root).generic_wstring(),L"--",text};
    std::wstring command;
    for(const auto& arg:args){if(!command.empty())command+=L' ';command+=quote(arg);}
    Handle job{CreateJobObjectW(nullptr,nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job.h || !SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))throw std::runtime_error("Cannot protect voice-engine process lifecycle");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    Handle nullFile{CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr)};
    if(nullFile.h==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot configure voice engine output");
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=startup.hStdOutput=startup.hStdError=nullFile.h;
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,root.c_str(),&startup,&process))
        throw std::runtime_error("Cannot launch installed Kokoro runtime: "+std::to_string(GetLastError()));
    Handle running{process.hProcess};Handle thread{process.hThread};
    if(!AssignProcessToJobObject(job.h,running.h)){TerminateProcess(running.h,2);throw std::runtime_error("Cannot bind voice engine to cancellation job");}
    ResumeThread(thread.h);
    if(WaitForSingleObject(running.h,110000)!=WAIT_OBJECT_0)throw std::runtime_error("Kokoro generation timed out");
    DWORD code{};GetExitCodeProcess(running.h,&code);
    if(code)throw std::runtime_error("Kokoro engine failed (exit "+std::to_string(code)+"); verify the installed model and runtime");
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        std::map<std::wstring,std::wstring> args;
        for(int i=1;i+1<argc;i+=2)args[argv[i]]=argv[i+1];
        for(const auto* key:{L"--manifest",L"--voice",L"--locale",L"--wpm",L"--text-file",L"--output-wav"})
            if(!args.count(key))throw std::runtime_error("Missing helper argument");
        // Baseline WPM measured on the same 17-word English calibration passage
        // for each pinned model voice. The application also reports actual WPM.
        const std::map<std::wstring,std::pair<int,double>> ids={
            {L"af_heart",{3,192}},{L"am_michael",{16,171}},
            {L"bf_emma",{21,186}},{L"bm_george",{26,162}}};
        const auto found=ids.find(args[L"--voice"]);if(found==ids.end())throw std::runtime_error("Unknown curated voice");
        const bool british=args[L"--voice"].front()==L'b';
        if(args[L"--locale"]!=(british?L"en-GB":L"en-US"))throw std::runtime_error("Voice locale mismatch");
        const int wpm=std::stoi(args[L"--wpm"]);if(wpm<40 || wpm>300)throw std::runtime_error("WPM must be 40 through 300");
        std::array<wchar_t,32768> module{};
        const auto length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
        if(!length || length>=module.size())throw std::runtime_error("Cannot locate installed helper");
        const fs::path root=fs::path(std::wstring(module.data(),length)).parent_path();
        if(!fs::exists(root/L"model/model.int8.onnx"))throw std::runtime_error("Install the Kokoro model before using this voice");
        std::ifstream input{fs::path(args.at(L"--text-file")),std::ios::binary};
        if(!input)throw std::runtime_error("Cannot read voice input");
        input.seekg(0,std::ios::end);const auto bytes=input.tellg();
        if(bytes<=0 || bytes>4*1024*1024)throw std::runtime_error("Voice input is empty or too large");
        input.seekg(0);std::string text(static_cast<std::size_t>(bytes),'\0');input.read(text.data(),bytes);input.close();
        const auto clean=englishText(text);
        const auto work=root/L"work"/(L"job-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        fs::create_directories(work);
        struct Cleanup{fs::path p;~Cleanup(){std::error_code e;fs::remove_all(p,e);}} cleanup{work};
        std::vector<std::int16_t> samples;
        std::size_t offset=0,index=0;
        while(offset<clean.size()) {
            auto end=std::min(offset+1800,clean.size());
            if(end<clean.size()) {auto split=clean.find_last_of(L".!?; ",end);if(split!=std::wstring::npos && split>offset)end=split+1;}
            const auto wav=work/(L"chunk-"+std::to_wstring(index++)+L".wav");
            engine(root,clean.substr(offset,end-offset),found->second.first,british,found->second.second/wpm,wav);
            auto chunk=readSamples(wav);samples.insert(samples.end(),chunk.begin(),chunk.end());offset=end;
            if(samples.size()>24000ULL*60*20)throw std::runtime_error("One voice turn exceeds twenty minutes");
        }
        const auto frames=static_cast<std::uint32_t>(samples.size()*44100ULL/24000ULL);
        const fs::path outputPath(args[L"--output-wav"]);
        if(outputPath.has_parent_path())fs::create_directories(outputPath.parent_path());
        std::ofstream output(outputPath,std::ios::binary|std::ios::trunc);
        output.write("RIFF",4);write(output,frames*2+36);output.write("WAVEfmt ",8);write<std::uint32_t>(output,16);
        write<std::uint16_t>(output,1);write<std::uint16_t>(output,1);write<std::uint32_t>(output,44100);write<std::uint32_t>(output,88200);
        write<std::uint16_t>(output,2);write<std::uint16_t>(output,16);output.write("data",4);write(output,frames*2);
        for(std::uint32_t i=0;i<frames;++i){double at=double(i)*24000/44100;auto a=static_cast<std::size_t>(at);auto b=std::min(a+1,samples.size()-1);write<std::int16_t>(output,static_cast<std::int16_t>(std::lround(samples[a]*(1-(at-a))+samples[b]*(at-a))));}
        output.flush();output.close();if(!output)throw std::runtime_error("Cannot write synthesized PCM");
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
