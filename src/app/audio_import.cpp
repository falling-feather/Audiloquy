#include "app/audio_import.h"
#include "platform/windows/windows_audio.h"

#include <QCryptographicHash>
#include <QFile>
#include <QUuid>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace listening::app {
namespace {
template<class T> struct ComPtr {
    T* p{};
    ~ComPtr() { if (p) p->Release(); }
    T** out() { return &p; }
    T* operator->() const { return p; }
};
void check(HRESULT hr, const char* context) {
    if (FAILED(hr)) throw std::runtime_error(std::string(context) + " (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")");
}
void cancellation(const std::function<bool()>& probe) {
    if (probe && probe()) throw std::runtime_error("Audio import cancelled");
}
template<class T> void little(std::ostream& out, T n) {
    for (unsigned i=0;i<sizeof(T);++i) out.put(static_cast<char>((n>>(i*8))&255));
}
void header(std::ostream& out, std::uint32_t bytes) {
    out.seekp(0);
    out.write("RIFF",4); little(out,bytes+36); out.write("WAVEfmt ",8);
    little<std::uint32_t>(out,16); little<std::uint16_t>(out,1); little<std::uint16_t>(out,1);
    little<std::uint32_t>(out,44100); little<std::uint32_t>(out,88200);
    little<std::uint16_t>(out,2); little<std::uint16_t>(out,16);
    out.write("data",4); little(out,bytes);
}
struct Temporary {
    std::filesystem::path path;
    ~Temporary() { std::error_code e; std::filesystem::remove(path,e); }
};
std::filesystem::path tempBeside(const std::filesystem::path& parent) {
    return parent / (QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()+".part.wav");
}
void publish(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish audio file: " + std::to_string(GetLastError()));
}
std::uint32_t read32(std::istream& in) {
    std::array<unsigned char,4> b{};
    if (!in.read(reinterpret_cast<char*>(b.data()),4)) throw std::runtime_error("Truncated WAV header");
    return b[0]|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);
}
void seekData(std::ifstream& input) {
    input.seekg(12);
    while (input) {
        std::array<char,4> id{};
        if (!input.read(id.data(),4)) break;
        const auto size=read32(input);
        if (std::string_view(id.data(),4)=="data") return;
        input.seekg(static_cast<std::streamoff>(size)+(size&1U),std::ios::cur);
    }
    throw std::runtime_error("WAV data chunk is missing");
}
void requireManagedFormat(const platform::windows::WavInfo& info) {
    if (info.format.sampleRate!=44100 || info.format.channels!=1 || info.format.bitsPerSample!=16 || !info.frameCount)
        throw std::runtime_error("Recording must be nonempty PCM16 mono 44100 Hz");
}
}

bool importRecording(const std::filesystem::path& source,
                     const std::filesystem::path& destinationDirectory,
                     ImportedAudio* result, std::string* error,
                     const std::function<bool()>& shouldCancel) {
    try {
        if (error) error->clear();
        if (!result || destinationDirectory.empty() || !std::filesystem::is_regular_file(source))
            throw std::runtime_error("Select an existing audio file and a destination directory");
        cancellation(shouldCancel);
        platform::windows::ComApartment apartment(platform::windows::ComApartmentModel::MultiThreaded);
        if (!apartment.ready()) throw std::runtime_error(apartment.error());
        check(MFStartup(MF_VERSION),"Starting Windows audio decoder");
        struct Shutdown { ~Shutdown(){MFShutdown();} } shutdown;
        ComPtr<IMFSourceReader> reader;
        check(MFCreateSourceReaderFromURL(source.c_str(),nullptr,reader.out()),"Opening audio recording");
        check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),"Selecting audio stream");
        check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM,TRUE),"Selecting audio stream");
        ComPtr<IMFMediaType> type;
        check(MFCreateMediaType(type.out()),"Creating PCM format");
        check(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),"Setting audio format");
        check(type->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM),"Setting PCM format");
        check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16),"Setting PCM precision");
        check(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,1),"Setting mono channel");
        check(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,44100),"Setting PCM rate");
        check(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,2),"Setting PCM alignment");
        check(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,88200),"Setting PCM byte rate");
        check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,nullptr,type.p),"Converting audio to PCM16 mono 44100 Hz");
        std::filesystem::create_directories(destinationDirectory);
        Temporary temporary{tempBeside(destinationDirectory)};
        std::ofstream output(temporary.path,std::ios::binary|std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot create imported audio");
        header(output,0);
        std::uint64_t bytes=0;
        constexpr std::uint64_t maximumBytes=88200ULL*60*120;
        while (true) {
            cancellation(shouldCancel);
            DWORD flags{};
            ComPtr<IMFSample> sample;
            check(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,nullptr,&flags,nullptr,sample.out()),"Decoding audio");
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) throw std::runtime_error("Recording changes its audio format mid-stream");
            if (sample.p) {
                ComPtr<IMFMediaBuffer> buffer;
                check(sample->ConvertToContiguousBuffer(buffer.out()),"Reading decoded audio");
                BYTE* data{}; DWORD count{};
                check(buffer->Lock(&data,nullptr,&count),"Locking decoded audio");
                if (bytes+count>maximumBytes || count%2) { buffer->Unlock(); throw std::runtime_error("Recording exceeds two hours or has invalid PCM alignment"); }
                output.write(reinterpret_cast<char*>(data),count);
                buffer->Unlock();
                if (!output) throw std::runtime_error("Cannot write imported audio");
                bytes+=count;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        if (!bytes) throw std::runtime_error("Recording contains no decodable audio");
        header(output,static_cast<std::uint32_t>(bytes));
        output.flush(); output.close();
        if (!output) throw std::runtime_error("Cannot finalize imported recording");
        cancellation(shouldCancel);
        QFile file(QString::fromStdWString(temporary.path.wstring()));
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot inspect imported audio");
        QCryptographicHash digest(QCryptographicHash::Sha256);
        while (!file.atEnd()) { cancellation(shouldCancel); digest.addData(file.read(1024*1024)); }
        file.close();
        const auto destination=destinationDirectory/(digest.result().toHex().toStdString()+".wav");
        publish(temporary.path,destination);
        result->path=std::filesystem::absolute(destination);
        result->durationMs=(bytes/2)*1000/44100;
        return true;
    } catch (const std::exception& failure) { if(error)*error=failure.what(); return false; }
}

bool extractRecording(const std::filesystem::path& source, std::uint64_t startMs,
                      std::uint64_t endMs, const std::filesystem::path& destination,
                      std::string* error, const std::function<bool()>& shouldCancel) {
    try {
        if (error) error->clear();
        platform::windows::WavInfo info;
        if (!platform::windows::inspectPcmWav(source,&info,error)) return false;
        requireManagedFormat(info);
        if (destination.empty() || startMs>=endMs || endMs>info.frameCount*1000/44100)
            throw std::runtime_error("Recording range must be inside the audio and have positive duration");
        const auto first=startMs*44100/1000;
        const auto last=endMs*44100/1000;
        if (last<=first) throw std::runtime_error("Recording range contains no audio frames");
        cancellation(shouldCancel);
        const auto parent=destination.has_parent_path()?destination.parent_path():std::filesystem::current_path();
        std::error_code sameFileError;
        if (std::filesystem::exists(destination) && std::filesystem::equivalent(source,destination,sameFileError))
            throw std::runtime_error("The source recording cannot be replaced by its preview");
        std::filesystem::create_directories(parent);
        Temporary temporary{tempBeside(parent)};
        std::ifstream input(source,std::ios::binary);
        seekData(input);
        input.seekg(static_cast<std::streamoff>(first*2),std::ios::cur);
        std::ofstream output(temporary.path,std::ios::binary|std::ios::trunc);
        header(output,static_cast<std::uint32_t>((last-first)*2));
        std::array<char,65536> buffer{};
        auto remaining=(last-first)*2;
        while (remaining) {
            cancellation(shouldCancel);
            const auto count=static_cast<std::streamsize>(std::min<std::uint64_t>(remaining,buffer.size()));
            if (!input.read(buffer.data(),count) || !output.write(buffer.data(),count))
                throw std::runtime_error("Cannot copy recording range");
            remaining-=static_cast<std::uint64_t>(count);
        }
        output.flush(); output.close(); input.close();
        if (!output) throw std::runtime_error("Cannot finalize recording range");
        cancellation(shouldCancel);
        publish(temporary.path,destination);
        return true;
    } catch (const std::exception& failure) { if(error)*error=failure.what(); return false; }
}
}
