#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "platform/windows/windows_audio.h"

#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "winmm.lib")
#endif

// MinGW's import libraries expose the SAPI classes/interfaces but some builds
// omit this format GUID from libsapi. sapi.h only declares it, so provide the
// one process-wide C-linkage definition for this translation unit/toolchain.
// {C31ADBAE-527F-4FF5-A230-F62BB61FF70C}
#if !defined(_MSC_VER)
extern "C" const GUID SPDFID_WaveFormatEx = {
    0xC31ADBAE,
    0x527F,
    0x4FF5,
    {0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C}};
#endif

namespace listening::platform::windows {
namespace {

template <typename T>
class ComPtr final {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] T* get() const noexcept { return pointer_; }
    [[nodiscard]] T* operator->() const noexcept { return pointer_; }

    T** put() noexcept {
        reset();
        return &pointer_;
    }

    void reset() noexcept {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_{};
};

class CoTaskString final {
public:
    CoTaskString() = default;
    ~CoTaskString() { ::CoTaskMemFree(value_); }

    CoTaskString(const CoTaskString&) = delete;
    CoTaskString& operator=(const CoTaskString&) = delete;

    LPWSTR* put() noexcept {
        ::CoTaskMemFree(value_);
        value_ = nullptr;
        return &value_;
    }

    [[nodiscard]] const wchar_t* get() const noexcept { return value_; }

private:
    LPWSTR value_{};
};

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

void clearError(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

std::string utf16ToUtf8Unchecked(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return "<UTF-16 text too long>";
    }
    const int required = ::WideCharToMultiByte(CP_UTF8,
                                                WC_ERR_INVALID_CHARS,
                                                text.data(),
                                                static_cast<int>(text.size()),
                                                nullptr,
                                                0,
                                                nullptr,
                                                nullptr);
    if (required <= 0) {
        return "<invalid UTF-16>";
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8,
                              WC_ERR_INVALID_CHARS,
                              text.data(),
                              static_cast<int>(text.size()),
                              result.data(),
                              required,
                              nullptr,
                              nullptr) != required) {
        return "<invalid UTF-16>";
    }
    return result;
}

std::string pathForMessage(const std::filesystem::path& path) {
    return utf16ToUtf8Unchecked(path.native());
}

std::string hexadecimalCode(long code) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(code);
    return stream.str();
}

std::string systemMessage(long code) {
    LPWSTR rawMessage = nullptr;
    const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                              FORMAT_MESSAGE_FROM_SYSTEM |
                                              FORMAT_MESSAGE_IGNORE_INSERTS,
                                          nullptr,
                                          static_cast<DWORD>(code),
                                          0,
                                          reinterpret_cast<LPWSTR>(&rawMessage),
                                          0,
                                          nullptr);
    if (length == 0 || rawMessage == nullptr) {
        return {};
    }
    std::wstring message(rawMessage, length);
    ::LocalFree(rawMessage);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ' ||
            message.back() == L'.')) {
        message.pop_back();
    }
    return utf16ToUtf8Unchecked(message);
}

std::string nativeFailure(std::string_view operation, long code) {
    std::string result(operation);
    result += " failed (";
    result += hexadecimalCode(code);
    const std::string message = systemMessage(code);
    if (!message.empty()) {
        result += ": ";
        result += message;
    }
    result += ")";
    return result;
}

std::string waveOutputFailure(std::string_view operation, MMRESULT code) {
    std::array<wchar_t, MAXERRORLENGTH> buffer{};
    std::string result(operation);
    result += " failed (waveOut ";
    result += std::to_string(static_cast<unsigned long>(code));
    if (::waveOutGetErrorTextW(code, buffer.data(), static_cast<UINT>(buffer.size())) ==
        MMSYSERR_NOERROR) {
        result += ": ";
        result += utf16ToUtf8Unchecked(buffer.data());
    }
    result += ')';
    return result;
}

std::string lowerAscii(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

std::wstring trimWide(std::wstring value) {
    const auto isSpace = [](wchar_t character) {
        return character == L' ' || character == L'\t' || character == L'\r' ||
               character == L'\n';
    };
    while (!value.empty() && isSpace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::string localeFromLanguageCode(std::wstring code) {
    code = trimWide(std::move(code));
    if (code.empty()) {
        return {};
    }

    wchar_t* end = nullptr;
    const unsigned long language = std::wcstoul(code.c_str(), &end, 16);
    if (end == code.c_str() || *end != L'\0' || language > 0xFFFFUL) {
        return utf16ToUtf8Unchecked(code);
    }

    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> localeName{};
    const LCID locale = MAKELCID(static_cast<LANGID>(language), SORT_DEFAULT);
    if (::LCIDToLocaleName(locale,
                           localeName.data(),
                           static_cast<int>(localeName.size()),
                           0) > 0) {
        return utf16ToUtf8Unchecked(localeName.data());
    }

    if (language == 0x0409UL) {
        return "en-US";
    }
    if (language == 0x0809UL) {
        return "en-GB";
    }
    return utf16ToUtf8Unchecked(code);
}

std::vector<std::string> parseLocales(const wchar_t* languageCodes) {
    std::vector<std::string> locales;
    if (languageCodes == nullptr) {
        return locales;
    }
    std::wstring remaining(languageCodes);
    std::size_t begin = 0;
    while (begin <= remaining.size()) {
        const std::size_t end = remaining.find(L';', begin);
        const auto length = end == std::wstring::npos ? std::wstring::npos : end - begin;
        std::string locale = localeFromLanguageCode(remaining.substr(begin, length));
        if (!locale.empty()) {
            locales.push_back(std::move(locale));
        }
        if (end == std::wstring::npos) {
            break;
        }
        begin = end + 1;
    }
    return locales;
}

bool hasLocale(const VoiceInfo& voice, std::string_view wanted) {
    const std::string normalizedWanted = lowerAscii(std::string(wanted));
    return std::any_of(voice.locales.begin(), voice.locales.end(), [&](const std::string& locale) {
        return lowerAscii(locale) == normalizedWanted;
    });
}

bool hasEnglishLocale(const VoiceInfo& voice) {
    return std::any_of(voice.locales.begin(), voice.locales.end(), [](const std::string& locale) {
        const std::string normalized = lowerAscii(locale);
        return normalized == "en" || normalized.rfind("en-", 0) == 0;
    });
}

std::string preferredLocale(Accent accent) {
    return accent == Accent::BritishEnglish ? "en-GB" : "en-US";
}

VoiceGender parseGender(const wchar_t* value) {
    if (value == nullptr) {
        return VoiceGender::Any;
    }
    const std::string normalized = lowerAscii(utf16ToUtf8Unchecked(value));
    if (normalized == "male") {
        return VoiceGender::Male;
    }
    if (normalized == "female") {
        return VoiceGender::Female;
    }
    return VoiceGender::Any;
}

struct EnumeratedVoice {
    VoiceInfo info;
    std::wstring tokenIdWide;
    ComPtr<ISpObjectToken> token;
};

bool enumerateVoices(std::vector<EnumeratedVoice>* voices, std::string* error) {
    if (voices == nullptr) {
        return fail(error, "Voice output collection is null.");
    }
    voices->clear();

    ComPtr<ISpObjectTokenCategory> category;
    HRESULT hr = ::CoCreateInstance(CLSID_SpObjectTokenCategory,
                                    nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_ISpObjectTokenCategory,
                                    reinterpret_cast<void**>(category.put()));
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Creating the SAPI voice category", hr));
    }
    hr = category->SetId(SPCAT_VOICES, FALSE);
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Opening the SAPI voice category", hr));
    }

    CoTaskString defaultTokenId;
    const HRESULT defaultResult = category->GetDefaultTokenId(defaultTokenId.put());

    ComPtr<IEnumSpObjectTokens> enumerator;
    hr = category->EnumTokens(nullptr, nullptr, enumerator.put());
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Enumerating installed SAPI voices", hr));
    }

    ULONG count = 0;
    hr = enumerator->GetCount(&count);
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Counting installed SAPI voices", hr));
    }
    voices->reserve(count);

    for (ULONG index = 0; index < count; ++index) {
        EnumeratedVoice entry;
        ULONG fetched = 0;
        hr = enumerator->Next(1, entry.token.put(), &fetched);
        if (hr == S_FALSE || fetched == 0) {
            break;
        }
        if (FAILED(hr)) {
            return fail(error, nativeFailure("Reading an installed SAPI voice", hr));
        }

        CoTaskString tokenId;
        hr = entry.token->GetId(tokenId.put());
        if (FAILED(hr) || tokenId.get() == nullptr) {
            return fail(error, nativeFailure("Reading a SAPI voice token id", hr));
        }
        entry.tokenIdWide = tokenId.get();
        entry.info.tokenId = utf16ToUtf8Unchecked(entry.tokenIdWide);
        entry.info.isSystemDefault =
            SUCCEEDED(defaultResult) && defaultTokenId.get() != nullptr &&
            _wcsicmp(entry.tokenIdWide.c_str(), defaultTokenId.get()) == 0;

        CoTaskString description;
        if (SUCCEEDED(entry.token->GetStringValue(nullptr, description.put())) &&
            description.get() != nullptr) {
            entry.info.name = utf16ToUtf8Unchecked(description.get());
        }
        if (entry.info.name.empty()) {
            const std::size_t separator = entry.tokenIdWide.find_last_of(L"\\/");
            entry.info.name = utf16ToUtf8Unchecked(entry.tokenIdWide.substr(
                separator == std::wstring::npos ? 0 : separator + 1));
        }

        ComPtr<ISpDataKey> attributes;
        if (SUCCEEDED(entry.token->OpenKey(L"Attributes", attributes.put()))) {
            CoTaskString languages;
            if (SUCCEEDED(attributes->GetStringValue(L"Language", languages.put()))) {
                entry.info.locales = parseLocales(languages.get());
            }
            CoTaskString gender;
            if (SUCCEEDED(attributes->GetStringValue(L"Gender", gender.put()))) {
                entry.info.gender = parseGender(gender.get());
            }
        }
        if (!entry.info.locales.empty()) {
            entry.info.locale = entry.info.locales.front();
        }
        voices->push_back(std::move(entry));
    }

    if (voices->empty()) {
        return fail(error,
                    "No Windows SAPI voices are installed. Install an English speech voice in "
                    "Windows Settings, then retry.");
    }
    return true;
}

std::filesystem::path temporarySibling(const std::filesystem::path& output) {
    static std::atomic<std::uint64_t> sequence{0};
    std::wostringstream suffix;
    suffix << L".sapi-" << ::GetCurrentProcessId() << L'-' << ::GetCurrentThreadId() << L'-'
           << sequence.fetch_add(1, std::memory_order_relaxed) << L".tmp.wav";
    std::filesystem::path filename = output.filename();
    filename += suffix.str();
    return output.parent_path() / filename;
}

class TemporaryFile final {
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile() {
        if (armed_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    void release() noexcept { armed_ = false; }

private:
    std::filesystem::path path_;
    bool armed_{true};
};

template <typename T>
bool readLittle(std::istream& input, T* value) {
    static_assert(std::is_unsigned_v<T>);
    std::array<unsigned char, sizeof(T)> bytes{};
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        return false;
    }
    *value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        *value |= static_cast<T>(bytes[index]) << (index * 8U);
    }
    return true;
}

}  // namespace

std::string_view voiceGenderName(VoiceGender gender) noexcept {
    switch (gender) {
    case VoiceGender::Male:
        return "male";
    case VoiceGender::Female:
        return "female";
    case VoiceGender::Any:
        return "unknown";
    }
    return "unknown";
}

bool chooseBestVoice(const std::vector<VoiceInfo>& voices,
                     const VoiceSelectionCriteria& criteria,
                     VoiceSelectionOutcome* outcome,
                     std::string* error) {
    clearError(error);
    if (outcome == nullptr) {
        return fail(error, "Voice selection outcome is null.");
    }
    *outcome = {};
    if (voices.empty()) {
        return fail(error, "No Windows SAPI voices are installed.");
    }

    const std::string requestedLocale = preferredLocale(criteria.accent);
    auto exactLocale = [&](const VoiceInfo& voice) { return hasLocale(voice, requestedLocale); };
    auto exactGender = [&](const VoiceInfo& voice) {
        return criteria.preferredGender == VoiceGender::Any ||
               voice.gender == criteria.preferredGender;
    };

    std::optional<std::size_t> explicitIndex;
    if (!criteria.voiceTokenId.empty()) {
        const std::string wanted = lowerAscii(criteria.voiceTokenId);
        for (std::size_t index = 0; index < voices.size(); ++index) {
            if (lowerAscii(voices[index].tokenId) == wanted) {
                explicitIndex = index;
                break;
            }
        }
        if (!explicitIndex.has_value()) {
            return fail(error, "Requested SAPI voice is not installed.");
        }
    }

    if (criteria.requireExactLocale &&
        std::none_of(voices.begin(), voices.end(), exactLocale)) {
        return fail(error,
                    "No installed Windows voice exactly matches " + requestedLocale +
                        ". Install that language's speech voice or disable strict accent "
                        "matching explicitly.");
    }
    if (criteria.requireExactGender && criteria.preferredGender != VoiceGender::Any &&
        std::none_of(voices.begin(), voices.end(), [&](const VoiceInfo& voice) {
            return exactLocale(voice) && exactGender(voice);
        })) {
        return fail(error,
                    "No installed " + requestedLocale + " " +
                        std::string(voiceGenderName(criteria.preferredGender)) +
                        " voice is available. Install a matching voice or explicitly allow "
                        "gender fallback.");
    }

    std::size_t best = explicitIndex.value_or(0);
    if (!explicitIndex.has_value()) {
        int bestScore = std::numeric_limits<int>::max();
        for (std::size_t index = 0; index < voices.size(); ++index) {
            const VoiceInfo& voice = voices[index];
            int score = 3000;
            if (exactLocale(voice)) {
                score = 0;
            } else if (hasEnglishLocale(voice)) {
                score = 1000;
            } else if (voice.isSystemDefault) {
                score = 2000;
            }
            if (criteria.preferredGender != VoiceGender::Any && !exactGender(voice)) {
                score += 200;
            }
            const std::string normalizedName = lowerAscii(voice.name);
            if (normalizedName.find("natural") != std::string::npos ||
                normalizedName.find("neural") != std::string::npos) {
                score -= 8;
            }
            if (voice.isSystemDefault) {
                --score;
            }
            if (score < bestScore) {
                best = index;
                bestScore = score;
            }
        }
    }

    const VoiceInfo& selected = voices[best];
    const bool localeMatches = exactLocale(selected);
    const bool genderMatches = exactGender(selected);
    if (criteria.requireExactLocale && !localeMatches) {
        return fail(error, "The explicitly selected voice does not match " + requestedLocale + ".");
    }
    if (criteria.requireExactGender && !genderMatches) {
        return fail(error, "The explicitly selected voice does not match the requested gender.");
    }

    outcome->index = best;
    outcome->exactLocaleMatch = localeMatches;
    outcome->exactGenderMatch = genderMatches;
    outcome->usedLocaleFallback = !localeMatches;
    outcome->usedGenderFallback = criteria.preferredGender != VoiceGender::Any && !genderMatches;
    return true;
}

bool utf8ToUtf16(std::string_view utf8, std::wstring* utf16, std::string* error) {
    clearError(error);
    if (utf16 == nullptr) {
        return fail(error, "UTF-16 output is null.");
    }
    utf16->clear();
    if (utf8.empty()) {
        return true;
    }
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return fail(error, "UTF-8 input is too large for the Windows conversion API.");
    }

    ::SetLastError(ERROR_SUCCESS);
    const int required = ::MultiByteToWideChar(CP_UTF8,
                                                MB_ERR_INVALID_CHARS,
                                                utf8.data(),
                                                static_cast<int>(utf8.size()),
                                                nullptr,
                                                0);
    if (required <= 0) {
        const DWORD code = ::GetLastError();
        return fail(error,
                    nativeFailure("Converting UTF-8 to UTF-16",
                                  code == ERROR_SUCCESS ? ERROR_NO_UNICODE_TRANSLATION : code));
    }
    utf16->resize(static_cast<std::size_t>(required));
    if (::MultiByteToWideChar(CP_UTF8,
                              MB_ERR_INVALID_CHARS,
                              utf8.data(),
                              static_cast<int>(utf8.size()),
                              utf16->data(),
                              required) != required) {
        const DWORD code = ::GetLastError();
        utf16->clear();
        return fail(error,
                    nativeFailure("Converting UTF-8 to UTF-16",
                                  code == ERROR_SUCCESS ? ERROR_NO_UNICODE_TRANSLATION : code));
    }
    return true;
}

bool utf16ToUtf8(std::wstring_view utf16, std::string* utf8, std::string* error) {
    clearError(error);
    if (utf8 == nullptr) {
        return fail(error, "UTF-8 output is null.");
    }
    utf8->clear();
    if (utf16.empty()) {
        return true;
    }
    if (utf16.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return fail(error, "UTF-16 input is too large for the Windows conversion API.");
    }

    ::SetLastError(ERROR_SUCCESS);
    const int required = ::WideCharToMultiByte(CP_UTF8,
                                                WC_ERR_INVALID_CHARS,
                                                utf16.data(),
                                                static_cast<int>(utf16.size()),
                                                nullptr,
                                                0,
                                                nullptr,
                                                nullptr);
    if (required <= 0) {
        const DWORD code = ::GetLastError();
        return fail(error,
                    nativeFailure("Converting UTF-16 to UTF-8",
                                  code == ERROR_SUCCESS ? ERROR_NO_UNICODE_TRANSLATION : code));
    }
    utf8->resize(static_cast<std::size_t>(required));
    if (::WideCharToMultiByte(CP_UTF8,
                              WC_ERR_INVALID_CHARS,
                              utf16.data(),
                              static_cast<int>(utf16.size()),
                              utf8->data(),
                              required,
                              nullptr,
                              nullptr) != required) {
        const DWORD code = ::GetLastError();
        utf8->clear();
        return fail(error,
                    nativeFailure("Converting UTF-16 to UTF-8",
                                  code == ERROR_SUCCESS ? ERROR_NO_UNICODE_TRANSLATION : code));
    }
    return true;
}

ComApartment::ComApartment(ComApartmentModel model) noexcept {
    const DWORD flags = model == ComApartmentModel::MultiThreaded ? COINIT_MULTITHREADED
                                                                  : COINIT_APARTMENTTHREADED;
    const HRESULT result = ::CoInitializeEx(nullptr, flags | COINIT_DISABLE_OLE1DDE);
    result_ = result;
    if (SUCCEEDED(result)) {
        ready_ = true;
        ownsInitialization_ = true;
    } else if (result == RPC_E_CHANGED_MODE) {
        // The caller already owns a usable COM apartment with another model.
        ready_ = true;
    }
}

ComApartment::~ComApartment() {
    if (ownsInitialization_) {
        ::CoUninitialize();
    }
}

bool ComApartment::ready() const noexcept {
    return ready_;
}

long ComApartment::nativeResult() const noexcept {
    return result_;
}

std::string ComApartment::error() const {
    return ready_ ? std::string{} : nativeFailure("Initializing COM", result_);
}

bool listVoices(std::vector<VoiceInfo>* voices, std::string* error) {
    clearError(error);
    if (voices == nullptr) {
        return fail(error, "Voice output collection is null.");
    }
    voices->clear();
    ComApartment apartment;
    if (!apartment.ready()) {
        return fail(error, apartment.error());
    }
    std::vector<EnumeratedVoice> enumerated;
    if (!enumerateVoices(&enumerated, error)) {
        return false;
    }
    voices->reserve(enumerated.size());
    for (auto& voice : enumerated) {
        voices->push_back(std::move(voice.info));
    }
    return true;
}

bool selectVoice(Accent accent, VoiceInfo* voice, std::string* error) {
    VoiceSelectionCriteria criteria;
    criteria.accent = accent;
    return selectVoice(criteria, voice, nullptr, error);
}

bool selectVoice(const VoiceSelectionCriteria& criteria,
                 VoiceInfo* voice,
                 VoiceSelectionOutcome* outcome,
                 std::string* error) {
    clearError(error);
    if (voice == nullptr) {
        return fail(error, "Selected voice output is null.");
    }
    *voice = {};
    ComApartment apartment;
    if (!apartment.ready()) {
        return fail(error, apartment.error());
    }
    std::vector<EnumeratedVoice> voices;
    if (!enumerateVoices(&voices, error)) {
        return false;
    }
    std::vector<VoiceInfo> infos;
    infos.reserve(voices.size());
    for (const auto& candidate : voices) {
        infos.push_back(candidate.info);
    }
    VoiceSelectionOutcome selected;
    if (!chooseBestVoice(infos, criteria, &selected, error)) {
        return false;
    }
    voices[selected.index].info.exactLocaleMatch = selected.exactLocaleMatch;
    *voice = std::move(voices[selected.index].info);
    if (outcome != nullptr) {
        *outcome = selected;
    }
    return true;
}

int approximateSapiRateForWpm(int targetWpm) noexcept {
    constexpr double referenceWpm = 150.0;
    constexpr double stepMultiplier = 1.10;
    const double safeWpm = static_cast<double>(std::max(targetWpm, 1));
    const long mapped = std::lround(std::log(safeWpm / referenceWpm) /
                                    std::log(stepMultiplier));
    return static_cast<int>(std::clamp(mapped, -10L, 10L));
}

bool inspectPcmWav(const std::filesystem::path& path, WavInfo* info, std::string* error) {
    clearError(error);
    if (info == nullptr) {
        return fail(error, "WAV information output is null.");
    }
    *info = {};
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail(error, "Cannot open WAV file: " + pathForMessage(path));
    }

    char riff[4]{};
    std::uint32_t riffSize = 0;
    char wave[4]{};
    if (!input.read(riff, sizeof(riff)) || !readLittle(input, &riffSize) ||
        !input.read(wave, sizeof(wave)) || std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(wave, "WAVE", 4) != 0) {
        return fail(error, "Not a RIFF/WAVE file: " + pathForMessage(path));
    }
    (void)riffSize;

    bool haveFormat = false;
    bool haveData = false;
    std::uint16_t formatTag = 0;
    while (input && !(haveFormat && haveData)) {
        char id[4]{};
        std::uint32_t size = 0;
        if (!input.read(id, sizeof(id)) || !readLittle(input, &size)) {
            break;
        }
        const std::streampos payload = input.tellg();
        if (payload == std::streampos(-1)) {
            break;
        }

        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (size < 16 || !readLittle(input, &formatTag) ||
                !readLittle(input, &info->format.channels) ||
                !readLittle(input, &info->format.sampleRate) ||
                !readLittle(input, &info->byteRate) || !readLittle(input, &info->blockAlign) ||
                !readLittle(input, &info->format.bitsPerSample)) {
                return fail(error, "Invalid WAV fmt chunk: " + pathForMessage(path));
            }
            haveFormat = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            info->dataBytes = size;
            haveData = true;
        }

        const std::uint64_t paddedSize = static_cast<std::uint64_t>(size) + (size & 1U);
        if (paddedSize > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return fail(error, "WAV chunk is too large: " + pathForMessage(path));
        }
        input.clear();
        input.seekg(payload + static_cast<std::streamoff>(paddedSize));
    }

    if (!haveFormat || !haveData) {
        return fail(error, "WAV file is missing fmt or data: " + pathForMessage(path));
    }
    if (formatTag != WAVE_FORMAT_PCM || info->format.sampleRate == 0 ||
        info->format.channels == 0 || info->format.bitsPerSample == 0 ||
        info->blockAlign == 0 || info->byteRate == 0) {
        return fail(error, "WAV is not valid uncompressed PCM: " + pathForMessage(path));
    }
    const std::uint32_t expectedBlockAlign =
        static_cast<std::uint32_t>(info->format.channels) * info->format.bitsPerSample / 8U;
    if (expectedBlockAlign != info->blockAlign || info->dataBytes % info->blockAlign != 0) {
        return fail(error, "WAV PCM frames are not aligned: " + pathForMessage(path));
    }
    const std::uint64_t expectedByteRate =
        static_cast<std::uint64_t>(info->format.sampleRate) * info->blockAlign;
    if (expectedByteRate != info->byteRate) {
        return fail(error, "WAV byte rate does not match its PCM format: " +
                               pathForMessage(path));
    }
    info->frameCount = info->dataBytes / info->blockAlign;
    info->durationSeconds = static_cast<double>(info->dataBytes) / info->byteRate;
    return true;
}

bool synthesizeToPcmWav(const SynthesisRequest& request,
                        SynthesisResult* result,
                        std::string* error,
                        const std::function<bool()>& shouldCancel) {
    clearError(error);
    if (result == nullptr) {
        return fail(error, "Synthesis result output is null.");
    }
    *result = {};
    if (request.textUtf8.empty()) {
        return fail(error, "Cannot synthesize empty text.");
    }
    if (request.targetWpm <= 0) {
        return fail(error, "Target WPM must be greater than zero.");
    }
    if (request.outputWav.empty()) {
        return fail(error, "Output WAV path is empty.");
    }
    if (shouldCancel && shouldCancel()) {
        return fail(error, "SAPI synthesis cancelled.");
    }

    std::wstring text;
    if (!utf8ToUtf16(request.textUtf8, &text, error)) {
        return false;
    }
    if (text.find(L'\0') != std::wstring::npos) {
        return fail(error, "Text contains U+0000, which SAPI cannot synthesize safely.");
    }

    std::error_code directoryError;
    if (request.outputWav.has_parent_path()) {
        std::filesystem::create_directories(request.outputWav.parent_path(), directoryError);
        if (directoryError) {
            return fail(error, "Cannot create output directory: " + directoryError.message());
        }
    }

    ComApartment apartment;
    if (!apartment.ready()) {
        return fail(error, apartment.error());
    }

    std::vector<EnumeratedVoice> voices;
    if (!enumerateVoices(&voices, error)) {
        return false;
    }

    std::vector<VoiceInfo> infos;
    infos.reserve(voices.size());
    for (const auto& candidate : voices) {
        infos.push_back(candidate.info);
    }
    VoiceSelectionCriteria criteria;
    criteria.accent = request.accent;
    criteria.preferredGender = request.preferredGender;
    criteria.voiceTokenId = request.voiceTokenId;
    criteria.requireExactLocale = request.requireExactLocale;
    criteria.requireExactGender = request.requireExactGender;
    VoiceSelectionOutcome selection;
    if (!chooseBestVoice(infos, criteria, &selection, error)) {
        return false;
    }
    if (shouldCancel && shouldCancel()) {
        return fail(error, "SAPI synthesis cancelled.");
    }
    const std::size_t selected = selection.index;
    voices[selected].info.exactLocaleMatch = selection.exactLocaleMatch;

    ComPtr<ISpVoice> sapiVoice;
    HRESULT hr = ::CoCreateInstance(CLSID_SpVoice,
                                    nullptr,
                                    CLSCTX_ALL,
                                    IID_ISpVoice,
                                    reinterpret_cast<void**>(sapiVoice.put()));
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Creating the SAPI speech engine", hr));
    }
    hr = sapiVoice->SetVoice(voices[selected].token.get());
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Selecting the SAPI voice", hr));
    }

    const int sapiRate = approximateSapiRateForWpm(request.targetWpm);
    hr = sapiVoice->SetRate(sapiRate);
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Setting the approximate SAPI speech rate", hr));
    }

    WAVEFORMATEX waveFormat{};
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.nChannels = kSynthesisPcmFormat.channels;
    waveFormat.nSamplesPerSec = kSynthesisPcmFormat.sampleRate;
    waveFormat.wBitsPerSample = kSynthesisPcmFormat.bitsPerSample;
    waveFormat.nBlockAlign = static_cast<WORD>(
        waveFormat.nChannels * waveFormat.wBitsPerSample / static_cast<WORD>(8));
    waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;

    const std::filesystem::path temporaryPath = temporarySibling(request.outputWav);
    TemporaryFile cleanup(temporaryPath);
    ComPtr<ISpStream> fileStream;
    hr = ::CoCreateInstance(CLSID_SpStream,
                            nullptr,
                            CLSCTX_INPROC_SERVER,
                            IID_ISpStream,
                            reinterpret_cast<void**>(fileStream.put()));
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Creating the SAPI WAV stream", hr));
    }
    hr = fileStream->BindToFile(temporaryPath.c_str(),
                                SPFM_CREATE_ALWAYS,
                                &SPDFID_WaveFormatEx,
                                &waveFormat,
                                SPFEI_ALL_EVENTS);
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Opening the temporary WAV output", hr));
    }
    hr = sapiVoice->SetOutput(fileStream.get(), TRUE);
    if (FAILED(hr)) {
        fileStream->Close();
        return fail(error, nativeFailure("Binding SAPI to the WAV output", hr));
    }
    hr = sapiVoice->Speak(text.c_str(), SPF_ASYNC, nullptr);
    bool cancelled = false;
    if (SUCCEEDED(hr)) {
        const HANDLE completeEvent = sapiVoice->SpeakCompleteEvent();
        const auto synthesisDeadline =
            std::chrono::steady_clock::now() + std::chrono::hours(1);
        while (true) {
            if (shouldCancel && shouldCancel()) {
                cancelled = true;
                // Purge the queued/current utterance before the worker leaves
                // the COM apartment. The bounded status poll below gives SAPI
                // time to release the output stream cleanly.
                (void)sapiVoice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                hr = E_ABORT;
                break;
            }
            if (completeEvent != nullptr) {
                const DWORD waitResult = ::WaitForSingleObject(completeEvent, 20);
                if (waitResult == WAIT_OBJECT_0) {
                    SPVOICESTATUS status{};
                    const HRESULT statusResult = sapiVoice->GetStatus(&status, nullptr);
                    hr = FAILED(statusResult) ? statusResult : status.hrLastResult;
                    break;
                }
                if (waitResult == WAIT_FAILED) {
                    hr = HRESULT_FROM_WIN32(::GetLastError());
                    break;
                }
            } else {
                SPVOICESTATUS status{};
                const HRESULT statusResult = sapiVoice->GetStatus(&status, nullptr);
                if (FAILED(statusResult)) {
                    hr = statusResult;
                    break;
                }
                if (status.dwRunningState == SPRS_DONE) {
                    hr = status.hrLastResult;
                    break;
                }
                ::Sleep(20);
            }
            if (std::chrono::steady_clock::now() >= synthesisDeadline) {
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                if (completeEvent == nullptr) {
                    (void)sapiVoice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                }
                break;
            }
        }
        if (cancelled) {
            // A purge can return before the completion event is signalled.
            // Wait briefly so releasing ISpVoice does not race SAPI's worker.
            if (completeEvent != nullptr) {
                (void)::WaitForSingleObject(completeEvent, 5000);
            } else {
                (void)sapiVoice->WaitUntilDone(5000);
            }
        } else if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
            // The timeout path already purged the utterance when no event
            // handle was available. Purge once more for the normal event path.
            (void)sapiVoice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
            if (completeEvent != nullptr) {
                (void)::WaitForSingleObject(completeEvent, 5000);
            } else {
                (void)sapiVoice->WaitUntilDone(5000);
            }
        }
    }
    if (shouldCancel && shouldCancel() && !cancelled) {
        // Cancellation can race the final event signal. Treat a completed
        // utterance as successful only when cancellation was not requested at
        // the handoff point.
        cancelled = true;
        (void)sapiVoice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
        (void)sapiVoice->WaitUntilDone(5000);
        hr = E_ABORT;
    }
    sapiVoice.reset();  // Releases SAPI's reference to the stream before Close().
    const HRESULT closeResult = fileStream->Close();
    fileStream.reset();
    if (cancelled) {
        return fail(error, "SAPI synthesis cancelled.");
    }
    if (FAILED(hr)) {
        return fail(error, nativeFailure("Synthesizing text with SAPI", hr));
    }
    if (FAILED(closeResult)) {
        return fail(error, nativeFailure("Finalizing the SAPI WAV stream", closeResult));
    }

    WavInfo wavInfo;
    if (!inspectPcmWav(temporaryPath, &wavInfo, error)) {
        return false;
    }
    if (wavInfo.format.sampleRate != kSynthesisPcmFormat.sampleRate ||
        wavInfo.format.channels != kSynthesisPcmFormat.channels ||
        wavInfo.format.bitsPerSample != kSynthesisPcmFormat.bitsPerSample) {
        return fail(error,
                    "The installed SAPI voice could not produce the required PCM16 mono 44.1 "
                    "kHz format.");
    }

    if (!::MoveFileExW(temporaryPath.c_str(),
                       request.outputWav.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail(error,
                    nativeFailure("Publishing the synthesized WAV", ::GetLastError()));
    }
    cleanup.release();

    result->outputWav = request.outputWav;
    result->voice = voices[selected].info;
    result->format = wavInfo.format;
    result->targetWpm = request.targetWpm;
    result->sapiRate = sapiRate;
    result->pcmFrameCount = wavInfo.frameCount;
    result->durationSeconds = wavInfo.durationSeconds;
    result->usedLocaleFallback = selection.usedLocaleFallback;
    result->usedGenderFallback = selection.usedGenderFallback;
    result->rateNotice =
        "Approximate mapping only: SAPI rate is engine-dependent; measure rendered speech "
        "duration for calibrated WPM.";
    return true;
}

struct WavPlayer::PlaybackContext {
    HWAVEOUT device{};
    WAVEHDR header{};
    std::vector<char> pcm;
    WavInfo info;
    std::uint64_t offsetBytes{};
    bool prepared{};
};

namespace {

bool loadPcmPayload(const std::filesystem::path& path,
                    std::uint64_t expectedBytes,
                    std::vector<char>* pcm,
                    std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail(error, "Cannot open WAV file for playback: " + pathForMessage(path));
    }
    input.seekg(12, std::ios::beg);
    while (input) {
        char id[4]{};
        std::uint32_t size = 0;
        if (!input.read(id, sizeof(id)) || !readLittle(input, &size)) {
            break;
        }
        if (std::memcmp(id, "data", 4) == 0) {
            if (size != expectedBytes ||
                static_cast<std::uint64_t>(size) >
                    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return fail(error, "WAV data size changed while opening playback");
            }
            pcm->assign(static_cast<std::size_t>(size), 0);
            if (size != 0 &&
                !input.read(pcm->data(), static_cast<std::streamsize>(pcm->size()))) {
                pcm->clear();
                return fail(error, "Cannot read WAV sample data for playback");
            }
            return true;
        }
        const std::uint64_t padded = static_cast<std::uint64_t>(size) + (size & 1U);
        if (padded > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return fail(error, "WAV chunk is too large for playback");
        }
        input.seekg(static_cast<std::streamoff>(padded), std::ios::cur);
    }
    return fail(error, "WAV data chunk could not be read for playback");
}

}  // namespace

WavPlayer::WavPlayer() = default;

WavPlayer::~WavPlayer() {
    stop(nullptr);
}

bool WavPlayer::playAsync(const std::filesystem::path& wavPath,
                          bool loop,
                          std::string* error) {
    clearError(error);
    WavInfo info;
    if (!inspectPcmWav(wavPath, &info, error)) {
        return false;
    }

    std::error_code absoluteError;
    std::filesystem::path stablePath = std::filesystem::absolute(wavPath, absoluteError);
    if (absoluteError) {
        return fail(error, "Cannot resolve WAV path: " + absoluteError.message());
    }

    std::scoped_lock lock(mutex_);
    if (!closeUnlocked(error)) {
        return false;
    }
    currentPath_ = std::move(stablePath);
    context_ = std::make_unique<PlaybackContext>();
    context_->info = info;
    if (!loadPcmPayload(currentPath_, info.dataBytes, &context_->pcm, error)) {
        closeUnlocked(nullptr);
        return false;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = info.format.channels;
    format.nSamplesPerSec = info.format.sampleRate;
    format.nAvgBytesPerSec = info.byteRate;
    format.nBlockAlign = info.blockAlign;
    format.wBitsPerSample = info.format.bitsPerSample;
    const MMRESULT openResult = ::waveOutOpen(
        &context_->device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (openResult != MMSYSERR_NOERROR) {
        const std::string message = waveOutputFailure("Opening the audio output", openResult);
        closeUnlocked(nullptr);
        return fail(error, message);
    }
    looping_ = loop;
    paused_ = false;
    durationMilliseconds_ = info.byteRate == 0
                                ? 0
                                : (info.dataBytes * 1000U) / info.byteRate;
    return startUnlocked(0, error);
}

bool WavPlayer::startUnlocked(std::uint64_t offsetBytes, std::string* error) {
    if (!context_ || context_->device == nullptr || context_->pcm.empty()) {
        return fail(error, "No PCM playback buffer is available");
    }
    const std::uint64_t blockAlign = context_->info.blockAlign;
    const std::uint64_t maximumOffset = context_->pcm.size() > blockAlign
                                            ? context_->pcm.size() - blockAlign
                                            : 0;
    offsetBytes = std::min(offsetBytes, maximumOffset);
    offsetBytes -= offsetBytes % blockAlign;
    context_->offsetBytes = offsetBytes;
    context_->header = {};
    context_->header.lpData = context_->pcm.data() + static_cast<std::size_t>(offsetBytes);
    context_->header.dwBufferLength = static_cast<DWORD>(
        std::min<std::uint64_t>(context_->pcm.size() - offsetBytes,
                                std::numeric_limits<DWORD>::max()));
    if (context_->header.dwBufferLength == 0) {
        return fail(error, "The WAV playback buffer is empty");
    }
    MMRESULT result = ::waveOutPrepareHeader(
        context_->device, &context_->header, sizeof(context_->header));
    if (result != MMSYSERR_NOERROR) {
        return fail(error, waveOutputFailure("Preparing the audio buffer", result));
    }
    context_->prepared = true;
    result = ::waveOutWrite(context_->device, &context_->header, sizeof(context_->header));
    if (result != MMSYSERR_NOERROR) {
        ::waveOutUnprepareHeader(context_->device, &context_->header, sizeof(context_->header));
        context_->prepared = false;
        return fail(error, waveOutputFailure("Starting WAV playback", result));
    }
    active_ = true;
    return true;
}

bool WavPlayer::pause(std::string* error) {
    clearError(error);
    std::scoped_lock lock(mutex_);
    if (!active_) {
        return fail(error, "No WAV is currently playing");
    }
    if (paused_) {
        return true;
    }
    const MMRESULT result = ::waveOutPause(context_->device);
    if (result != MMSYSERR_NOERROR) {
        return fail(error, waveOutputFailure("Pausing WAV playback", result));
    }
    paused_ = true;
    return true;
}

bool WavPlayer::resume(std::string* error) {
    clearError(error);
    std::scoped_lock lock(mutex_);
    if (!active_) {
        return fail(error, "No paused WAV is available");
    }
    if (!paused_) {
        return true;
    }
    const MMRESULT result = ::waveOutRestart(context_->device);
    if (result != MMSYSERR_NOERROR) {
        return fail(error, waveOutputFailure("Resuming WAV playback", result));
    }
    paused_ = false;
    return true;
}

bool WavPlayer::seek(std::uint64_t positionMilliseconds, std::string* error) {
    clearError(error);
    std::scoped_lock lock(mutex_);
    if (!active_) {
        return fail(error, "No WAV is open for seeking");
    }

    const bool wasPaused = paused_;
    const std::uint64_t clamped = std::min(positionMilliseconds, durationMilliseconds_);
    std::uint64_t offset = (clamped * context_->info.byteRate) / 1000U;
    offset -= offset % context_->info.blockAlign;
    MMRESULT result = ::waveOutReset(context_->device);
    if (result != MMSYSERR_NOERROR) {
        return fail(error, waveOutputFailure("Resetting WAV playback for seek", result));
    }
    if (context_->prepared) {
        result = ::waveOutUnprepareHeader(
            context_->device, &context_->header, sizeof(context_->header));
        if (result != MMSYSERR_NOERROR) {
            return fail(error, waveOutputFailure("Releasing the previous audio buffer", result));
        }
        context_->prepared = false;
    }
    paused_ = false;
    if (!startUnlocked(offset, error)) {
        return false;
    }
    if (wasPaused) {
        result = ::waveOutPause(context_->device);
        if (result != MMSYSERR_NOERROR) {
            return fail(error, waveOutputFailure("Restoring paused state after seek", result));
        }
        paused_ = true;
    }
    return true;
}

bool WavPlayer::stop(std::string* error) noexcept {
    try {
        clearError(error);
        std::scoped_lock lock(mutex_);
        return closeUnlocked(error);
    } catch (...) {
        // Destruction must never throw. A caller-provided error string may itself
        // fail to allocate, so there is no safe diagnostic action here.
        return false;
    }
}

bool WavPlayer::closeUnlocked(std::string* error) noexcept {
    try {
        bool succeeded = true;
        std::string firstError;
        if (context_ && context_->device != nullptr) {
            MMRESULT result = ::waveOutReset(context_->device);
            if (result != MMSYSERR_NOERROR) {
                succeeded = false;
                firstError = waveOutputFailure("Stopping WAV playback", result);
            }
            if (context_->prepared) {
                result = ::waveOutUnprepareHeader(
                    context_->device, &context_->header, sizeof(context_->header));
                if (result != MMSYSERR_NOERROR && succeeded) {
                    succeeded = false;
                    firstError = waveOutputFailure("Releasing the audio buffer", result);
                }
                context_->prepared = false;
            }
            result = ::waveOutClose(context_->device);
            if (result != MMSYSERR_NOERROR && succeeded) {
                succeeded = false;
                firstError = waveOutputFailure("Closing the audio output", result);
            }
        }
        context_.reset();
        active_ = false;
        looping_ = false;
        paused_ = false;
        currentPath_.clear();
        durationMilliseconds_ = 0;
        return succeeded ? true : fail(error, firstError);
    } catch (...) {
        return false;
    }
}

WavPlayer::Snapshot WavPlayer::snapshot(std::string* error) noexcept {
    try {
        clearError(error);
        std::scoped_lock lock(mutex_);
        Snapshot result;
        result.durationMilliseconds = durationMilliseconds_;
        result.looping = looping_;
        if (!active_) {
            return result;
        }

        if (!context_ || context_->device == nullptr) {
            active_ = false;
            return result;
        }

        if ((context_->header.dwFlags & WHDR_DONE) != 0U) {
            if (looping_) {
                if (context_->prepared) {
                    const MMRESULT unprepare = ::waveOutUnprepareHeader(
                        context_->device, &context_->header, sizeof(context_->header));
                    if (unprepare != MMSYSERR_NOERROR) {
                        fail(error, waveOutputFailure("Releasing a completed loop buffer", unprepare));
                        active_ = false;
                        return result;
                    }
                    context_->prepared = false;
                }
                if (startUnlocked(0, error)) {
                    paused_ = false;
                    result.state = State::Playing;
                    result.positionMilliseconds = 0;
                } else {
                    active_ = false;
                }
                return result;
            }
            result.positionMilliseconds = durationMilliseconds_;
            result.state = State::Stopped;
            closeUnlocked(nullptr);
            return result;
        }

        MMTIME position{};
        position.wType = TIME_BYTES;
        const MMRESULT positionResult =
            ::waveOutGetPosition(context_->device, &position, sizeof(position));
        if (positionResult != MMSYSERR_NOERROR) {
            fail(error, waveOutputFailure("Reading WAV playback position", positionResult));
            active_ = false;
            return result;
        }
        std::uint64_t playedBytes = 0;
        if (position.wType == TIME_BYTES) {
            playedBytes = position.u.cb;
        } else if (position.wType == TIME_SAMPLES) {
            playedBytes = static_cast<std::uint64_t>(position.u.sample) *
                          context_->info.blockAlign;
        } else if (position.wType == TIME_MS) {
            playedBytes = static_cast<std::uint64_t>(position.u.ms) *
                          context_->info.byteRate / 1000U;
        }
        const std::uint64_t absoluteBytes = std::min<std::uint64_t>(
            context_->offsetBytes + playedBytes, context_->info.dataBytes);
        result.positionMilliseconds = context_->info.byteRate == 0
                                          ? 0
                                          : (absoluteBytes * 1000U) /
                                                context_->info.byteRate;
        result.state = paused_ ? State::Paused : State::Playing;
        return result;
    } catch (...) {
        return {};
    }
}

bool WavPlayer::active() noexcept {
    return snapshot(nullptr).state != State::Stopped;
}

bool WavPlayer::looping() const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return looping_;
    } catch (...) {
        return false;
    }
}

std::filesystem::path WavPlayer::currentPath() const {
    std::scoped_lock lock(mutex_);
    return currentPath_;
}

}  // namespace listening::platform::windows
