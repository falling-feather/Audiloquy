#include "core/json.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace listening {
namespace {

constexpr std::size_t maximumJsonBytes = 64 * 1024 * 1024;
constexpr std::size_t maximumJsonDepth = 64;

struct JsonValue {
    enum class Kind { Null, Boolean, Number, String, Array, Object };

    Kind kind{Kind::Null};
    bool boolean{};
    double number{};
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue, std::less<>> object;
};

[[nodiscard]] std::string locationMessage(
    std::string_view input,
    std::size_t offset,
    std::string_view message) {
    std::size_t line = 1;
    std::size_t column = 1;
    const auto boundedOffset = std::min(offset, input.size());
    for (std::size_t index = 0; index < boundedOffset; ++index) {
        if (input[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }

    std::ostringstream output;
    output << "JSON parse error at line " << line << ", column " << column << ": " << message;
    return output.str();
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    [[nodiscard]] JsonValue parse() {
        skipWhitespace();
        JsonValue result = parseValue(0);
        skipWhitespace();
        if (offset_ != input_.size()) {
            fail("unexpected characters after the root value");
        }
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw ProjectFormatError(locationMessage(input_, offset_, message));
    }

    void skipWhitespace() noexcept {
        while (offset_ < input_.size()) {
            const char value = input_[offset_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++offset_;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        if (offset_ < input_.size() && input_[offset_] == expected) {
            ++offset_;
            return true;
        }
        return false;
    }

    void expect(char expected, std::string_view message) {
        if (!consume(expected)) {
            fail(message);
        }
    }

    void expectLiteral(std::string_view literal) {
        if (input_.substr(offset_, literal.size()) != literal) {
            fail("invalid literal");
        }
        offset_ += literal.size();
    }

    [[nodiscard]] JsonValue parseValue(std::size_t depth) {
        if (depth > maximumJsonDepth) {
            fail("maximum nesting depth exceeded");
        }
        if (offset_ >= input_.size()) {
            fail("expected a JSON value");
        }

        switch (input_[offset_]) {
        case 'n': {
            expectLiteral("null");
            return JsonValue{};
        }
        case 't': {
            expectLiteral("true");
            JsonValue value;
            value.kind = JsonValue::Kind::Boolean;
            value.boolean = true;
            return value;
        }
        case 'f': {
            expectLiteral("false");
            JsonValue value;
            value.kind = JsonValue::Kind::Boolean;
            value.boolean = false;
            return value;
        }
        case '"': {
            JsonValue value;
            value.kind = JsonValue::Kind::String;
            value.string = parseString();
            return value;
        }
        case '[':
            return parseArray(depth + 1);
        case '{':
            return parseObject(depth + 1);
        default:
            if (input_[offset_] == '-' ||
                (input_[offset_] >= '0' && input_[offset_] <= '9')) {
                return parseNumber();
            }
            fail("expected a JSON value");
        }
    }

    [[nodiscard]] static int hexValue(char character) noexcept {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return 10 + character - 'a';
        }
        if (character >= 'A' && character <= 'F') {
            return 10 + character - 'A';
        }
        return -1;
    }

    [[nodiscard]] std::uint32_t parseHexQuad() {
        if (input_.size() - offset_ < 4) {
            fail("incomplete Unicode escape");
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = hexValue(input_[offset_++]);
            if (digit < 0) {
                fail("invalid hexadecimal digit in Unicode escape");
            }
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    static void appendUtf8(std::string& output, std::uint32_t codePoint) {
        if (codePoint <= 0x7fU) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
        } else if (codePoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
        }
    }

    [[nodiscard]] std::string parseString() {
        expect('"', "expected a string");
        std::string output;

        while (offset_ < input_.size()) {
            const auto character = static_cast<unsigned char>(input_[offset_++]);
            if (character == '"') {
                if (!isValidUtf8(output)) {
                    fail("string contains invalid UTF-8");
                }
                return output;
            }
            if (character < 0x20U) {
                fail("unescaped control character in string");
            }
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                continue;
            }

            if (offset_ >= input_.size()) {
                fail("incomplete string escape");
            }
            const char escaped = input_[offset_++];
            switch (escaped) {
            case '"':
                output.push_back('"');
                break;
            case '\\':
                output.push_back('\\');
                break;
            case '/':
                output.push_back('/');
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                std::uint32_t codePoint = parseHexQuad();
                if (codePoint >= 0xd800U && codePoint <= 0xdbffU) {
                    if (input_.size() - offset_ < 2 || input_[offset_] != '\\' ||
                        input_[offset_ + 1] != 'u') {
                        fail("high surrogate must be followed by a low surrogate");
                    }
                    offset_ += 2;
                    const std::uint32_t low = parseHexQuad();
                    if (low < 0xdc00U || low > 0xdfffU) {
                        fail("high surrogate must be followed by a low surrogate");
                    }
                    codePoint = 0x10000U + ((codePoint - 0xd800U) << 10U) +
                                (low - 0xdc00U);
                } else if (codePoint >= 0xdc00U && codePoint <= 0xdfffU) {
                    fail("unexpected low surrogate");
                }
                appendUtf8(output, codePoint);
                break;
            }
            default:
                fail("unsupported string escape");
            }
        }

        fail("unterminated string");
    }

    [[nodiscard]] JsonValue parseNumber() {
        const std::size_t start = offset_;
        (void)consume('-');

        if (consume('0')) {
            if (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                fail("leading zero in number");
            }
        } else {
            if (offset_ >= input_.size() || input_[offset_] < '1' || input_[offset_] > '9') {
                fail("invalid number");
            }
            while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                ++offset_;
            }
        }

        if (consume('.')) {
            if (offset_ >= input_.size() || input_[offset_] < '0' || input_[offset_] > '9') {
                fail("fraction requires a digit");
            }
            while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                ++offset_;
            }
        }

        if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < input_.size() && (input_[offset_] == '+' || input_[offset_] == '-')) {
                ++offset_;
            }
            if (offset_ >= input_.size() || input_[offset_] < '0' || input_[offset_] > '9') {
                fail("exponent requires a digit");
            }
            while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                ++offset_;
            }
        }

        const std::string_view token = input_.substr(start, offset_ - start);
        double parsed = 0.0;
        const auto result =
            std::from_chars(token.data(), token.data() + token.size(), parsed, std::chars_format::general);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
            !std::isfinite(parsed)) {
            fail("number is outside the supported range");
        }

        JsonValue value;
        value.kind = JsonValue::Kind::Number;
        value.number = parsed;
        return value;
    }

    [[nodiscard]] JsonValue parseArray(std::size_t depth) {
        expect('[', "expected an array");
        JsonValue value;
        value.kind = JsonValue::Kind::Array;
        skipWhitespace();
        if (consume(']')) {
            return value;
        }

        while (true) {
            skipWhitespace();
            value.array.push_back(parseValue(depth));
            skipWhitespace();
            if (consume(']')) {
                return value;
            }
            expect(',', "expected ',' or ']' in array");
            skipWhitespace();
            if (offset_ < input_.size() && input_[offset_] == ']') {
                fail("trailing comma in array");
            }
        }
    }

    [[nodiscard]] JsonValue parseObject(std::size_t depth) {
        expect('{', "expected an object");
        JsonValue value;
        value.kind = JsonValue::Kind::Object;
        skipWhitespace();
        if (consume('}')) {
            return value;
        }

        while (true) {
            skipWhitespace();
            if (offset_ >= input_.size() || input_[offset_] != '"') {
                fail("object key must be a string");
            }
            std::string key = parseString();
            skipWhitespace();
            expect(':', "expected ':' after object key");
            skipWhitespace();
            JsonValue member = parseValue(depth);
            if (!value.object.emplace(std::move(key), std::move(member)).second) {
                fail("duplicate object key");
            }
            skipWhitespace();
            if (consume('}')) {
                return value;
            }
            expect(',', "expected ',' or '}' in object");
            skipWhitespace();
            if (offset_ < input_.size() && input_[offset_] == '}') {
                fail("trailing comma in object");
            }
        }
    }

    std::string_view input_;
    std::size_t offset_{};
};

[[nodiscard]] const JsonValue& requireMember(
    const JsonValue& object,
    std::string_view key,
    JsonValue::Kind expectedKind,
    std::string_view path) {
    if (object.kind != JsonValue::Kind::Object) {
        throw ProjectFormatError(std::string(path) + " must be an object");
    }
    const auto iterator = object.object.find(key);
    if (iterator == object.object.end()) {
        throw ProjectFormatError(std::string(path) + "." + std::string(key) + " is required");
    }
    if (iterator->second.kind != expectedKind) {
        throw ProjectFormatError(std::string(path) + "." + std::string(key) + " has the wrong type");
    }
    return iterator->second;
}

[[nodiscard]] const JsonValue* optionalMember(
    const JsonValue& object,
    std::string_view key) noexcept {
    if (object.kind != JsonValue::Kind::Object) {
        return nullptr;
    }
    const auto iterator = object.object.find(key);
    return iterator == object.object.end() ? nullptr : &iterator->second;
}

[[nodiscard]] bool requireBoolean(
    const JsonValue& object,
    std::string_view key,
    std::string_view path) {
    return requireMember(object, key, JsonValue::Kind::Boolean, path).boolean;
}

[[nodiscard]] int requireInteger(
    const JsonValue& object,
    std::string_view key,
    std::string_view path) {
    const double number = requireMember(object, key, JsonValue::Kind::Number, path).number;
    if (std::trunc(number) != number || number < static_cast<double>(std::numeric_limits<int>::min()) ||
        number > static_cast<double>(std::numeric_limits<int>::max())) {
        throw ProjectFormatError(std::string(path) + "." + std::string(key) + " must be an integer");
    }
    return static_cast<int>(number);
}

[[nodiscard]] std::string escapeString(std::string_view value) {
    if (!isValidUtf8(value)) {
        throw ProjectFormatError("Cannot serialize invalid UTF-8");
    }

    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (character < 0x20U) {
                output += "\\u00";
                output.push_back(hex[(character >> 4U) & 0x0fU]);
                output.push_back(hex[character & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    output.push_back('"');
    return output;
}

[[nodiscard]] std::string formatNumber(double value) {
    if (!std::isfinite(value)) {
        throw ProjectFormatError("Cannot serialize a non-finite number");
    }
    char buffer[64];
    const auto result = std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) {
        throw ProjectFormatError("Cannot serialize number");
    }
    return std::string(buffer, result.ptr);
}

void appendFieldPrefix(
    std::string& output,
    std::string_view key,
    bool pretty,
    int indentation,
    bool first) {
    if (!first) {
        output.push_back(',');
    }
    if (pretty) {
        output.push_back('\n');
        output.append(static_cast<std::size_t>(indentation), ' ');
    }
    output += escapeString(key);
    output += pretty ? ": " : ":";
}

[[nodiscard]] std::string validationSummary(const ValidationError& error) {
    return error.what();
}

}  // namespace

std::string toJson(const Project& project, bool pretty, ValidationPurpose purpose) {
    try {
        requireValid(project, purpose);
    } catch (const ValidationError& error) {
        throw ProjectFormatError(validationSummary(error));
    }

    std::string output;
    output.reserve(512 + project.segments.size() * 256);
    output.push_back('{');

    appendFieldPrefix(output, "schemaVersion", pretty, 2, true);
    output += std::to_string(project.schemaVersion);
    appendFieldPrefix(output, "id", pretty, 2, false);
    output += escapeString(project.id);
    appendFieldPrefix(output, "title", pretty, 2, false);
    output += escapeString(project.title);
    appendFieldPrefix(output, "accent", pretty, 2, false);
    output += escapeString(accentCode(project.accent));
    appendFieldPrefix(output, "targetWpm", pretty, 2, false);
    output += formatNumber(project.targetWpm);
    appendFieldPrefix(output, "voiceSettings", pretty, 2, false);
    output.push_back('{');
    appendFieldPrefix(output, "maleVoiceTokenId", pretty, 4, true);
    output += escapeString(project.voiceSettings.maleVoiceTokenId);
    appendFieldPrefix(output, "femaleVoiceTokenId", pretty, 4, false);
    output += escapeString(project.voiceSettings.femaleVoiceTokenId);
    appendFieldPrefix(output, "strictAccent", pretty, 4, false);
    output += project.voiceSettings.strictAccent ? "true" : "false";
    appendFieldPrefix(output, "allowGenderFallback", pretty, 4, false);
    output += project.voiceSettings.allowGenderFallback ? "true" : "false";
    if (pretty) {
        output.push_back('\n');
        output.append(2, ' ');
    }
    output.push_back('}');
    appendFieldPrefix(output, "segments", pretty, 2, false);
    output.push_back('[');

    for (std::size_t index = 0; index < project.segments.size(); ++index) {
        const Segment& segment = project.segments[index];
        if (index != 0) {
            output.push_back(',');
        }
        if (pretty) {
            output.push_back('\n');
            output.append(4, ' ');
        }
        output.push_back('{');
        appendFieldPrefix(output, "id", pretty, 6, true);
        output += escapeString(segment.id);
        appendFieldPrefix(output, "questionStart", pretty, 6, false);
        output += std::to_string(segment.questions.first);
        appendFieldPrefix(output, "questionEnd", pretty, 6, false);
        output += std::to_string(segment.questions.last);
        appendFieldPrefix(output, "speaker", pretty, 6, false);
        output += escapeString(segment.speaker);
        appendFieldPrefix(output, "text", pretty, 6, false);
        output += escapeString(segment.text);
        appendFieldPrefix(output, "pauseAfterSeconds", pretty, 6, false);
        output += formatNumber(segment.pauseAfterSeconds);
        appendFieldPrefix(output, "repeatCount", pretty, 6, false);
        output += std::to_string(segment.repeatCount);
        appendFieldPrefix(output, "renderedAudioFile", pretty, 6, false);
        output += escapeString(segment.renderedAudioFile);
        appendFieldPrefix(output, "recording", pretty, 6, false);
        if (!segment.recording.has_value()) {
            output += "null";
        } else {
            const RecordingSource& recording = *segment.recording;
            output.push_back('{');
            appendFieldPrefix(output, "audioFile", pretty, 8, true);
            output += escapeString(recording.audioFile);
            appendFieldPrefix(output, "startMs", pretty, 8, false);
            output += std::to_string(recording.startMs);
            appendFieldPrefix(output, "endMs", pretty, 8, false);
            output += std::to_string(recording.endMs);
            if (pretty) {
                output.push_back('\n');
                output.append(6, ' ');
            }
            output.push_back('}');
        }
        appendFieldPrefix(output, "generation", pretty, 6, false);
        if (!segment.generation.has_value()) {
            output += "null";
        } else {
            const GenerationRecord& record = *segment.generation;
            output.push_back('{');
            appendFieldPrefix(output, "provider", pretty, 8, true);
            output += escapeString(record.provider);
            appendFieldPrefix(output, "model", pretty, 8, false);
            output += escapeString(record.model);
            appendFieldPrefix(output, "questionStem", pretty, 8, false);
            output += escapeString(record.questionStem);
            appendFieldPrefix(output, "options", pretty, 8, false);
            output.push_back('[');
            for (std::size_t option = 0; option < record.options.size(); ++option) {
                if (option != 0) {
                    output.push_back(',');
                }
                if (pretty) {
                    output.push_back(' ');
                }
                output += escapeString(record.options[option]);
            }
            if (pretty && !record.options.empty()) {
                output.push_back(' ');
            }
            output.push_back(']');
            appendFieldPrefix(output, "correctAnswer", pretty, 8, false);
            output += escapeString(record.correctAnswer);
            appendFieldPrefix(output, "requiresTeacherReview", pretty, 8, false);
            output += record.requiresTeacherReview ? "true" : "false";
            appendFieldPrefix(output, "teacherReviewed", pretty, 8, false);
            output += record.teacherReviewed ? "true" : "false";
            appendFieldPrefix(output, "evidence", pretty, 8, false);
            output.push_back('[');
            for (std::size_t evidenceIndex = 0;
                 evidenceIndex < record.evidence.size();
                 ++evidenceIndex) {
                const GenerationEvidence& item = record.evidence[evidenceIndex];
                if (evidenceIndex != 0) {
                    output.push_back(',');
                }
                if (pretty) {
                    output.push_back('\n');
                    output.append(10, ' ');
                }
                output.push_back('{');
                appendFieldPrefix(output, "option", pretty, 12, true);
                output += escapeString(item.option);
                appendFieldPrefix(output, "role", pretty, 12, false);
                output += escapeString(item.role);
                appendFieldPrefix(output, "turnId", pretty, 12, false);
                output += escapeString(item.turnId);
                appendFieldPrefix(output, "quote", pretty, 12, false);
                output += escapeString(item.quote);
                if (pretty) {
                    output.push_back('\n');
                    output.append(10, ' ');
                }
                output.push_back('}');
            }
            if (pretty && !record.evidence.empty()) {
                output.push_back('\n');
                output.append(8, ' ');
            }
            output.push_back(']');
            appendFieldPrefix(output, "additionalQuestions", pretty, 8, false);
            output.push_back('[');
            for (std::size_t questionIndex = 0;
                 questionIndex < record.additionalQuestions.size(); ++questionIndex) {
                const GenerationQuestion& question = record.additionalQuestions[questionIndex];
                if (questionIndex != 0) {
                    output.push_back(',');
                }
                if (pretty) {
                    output.push_back('\n');
                    output.append(10, ' ');
                }
                output.push_back('{');
                appendFieldPrefix(output, "questionStem", pretty, 12, true);
                output += escapeString(question.questionStem);
                appendFieldPrefix(output, "options", pretty, 12, false);
                output.push_back('[');
                for (std::size_t option = 0; option < question.options.size(); ++option) {
                    if (option != 0) {
                        output.push_back(',');
                    }
                    if (pretty) {
                        output.push_back(' ');
                    }
                    output += escapeString(question.options[option]);
                }
                if (pretty && !question.options.empty()) {
                    output.push_back(' ');
                }
                output.push_back(']');
                appendFieldPrefix(output, "correctAnswer", pretty, 12, false);
                output += escapeString(question.correctAnswer);
                appendFieldPrefix(output, "evidence", pretty, 12, false);
                output.push_back('[');
                for (std::size_t evidenceIndex = 0;
                     evidenceIndex < question.evidence.size(); ++evidenceIndex) {
                    const GenerationEvidence& item = question.evidence[evidenceIndex];
                    if (evidenceIndex != 0) {
                        output.push_back(',');
                    }
                    if (pretty) {
                        output.push_back('\n');
                        output.append(14, ' ');
                    }
                    output.push_back('{');
                    appendFieldPrefix(output, "option", pretty, 16, true);
                    output += escapeString(item.option);
                    appendFieldPrefix(output, "role", pretty, 16, false);
                    output += escapeString(item.role);
                    appendFieldPrefix(output, "turnId", pretty, 16, false);
                    output += escapeString(item.turnId);
                    appendFieldPrefix(output, "quote", pretty, 16, false);
                    output += escapeString(item.quote);
                    if (pretty) {
                        output.push_back('\n');
                        output.append(14, ' ');
                    }
                    output.push_back('}');
                }
                if (pretty && !question.evidence.empty()) {
                    output.push_back('\n');
                    output.append(12, ' ');
                }
                output.push_back(']');
                if (pretty) {
                    output.push_back('\n');
                    output.append(10, ' ');
                }
                output.push_back('}');
            }
            if (pretty && !record.additionalQuestions.empty()) {
                output.push_back('\n');
                output.append(8, ' ');
            }
            output.push_back(']');
            if (pretty) {
                output.push_back('\n');
                output.append(6, ' ');
            }
            output.push_back('}');
        }
        if (pretty) {
            output.push_back('\n');
            output.append(4, ' ');
        }
        output.push_back('}');
    }

    if (pretty && !project.segments.empty()) {
        output.push_back('\n');
        output.append(2, ' ');
    }
    output.push_back(']');
    appendFieldPrefix(output, "renderedProgramFile", pretty, 2, false);
    output += escapeString(project.renderedProgramFile);
    if (pretty) {
        output.push_back('\n');
    }
    output.push_back('}');
    if (pretty) {
        output.push_back('\n');
    }
    return output;
}

Project fromJson(std::string_view json, ValidationPurpose purpose) {
    if (json.size() > maximumJsonBytes) {
        throw ProjectFormatError("Project JSON is larger than 64 MiB");
    }

    const JsonValue root = Parser(json).parse();
    if (root.kind != JsonValue::Kind::Object) {
        throw ProjectFormatError("Project JSON root must be an object");
    }

    Project project;
    const int sourceSchemaVersion = requireInteger(root, "schemaVersion", "$");
    if (sourceSchemaVersion < 1 || sourceSchemaVersion > Project::currentSchemaVersion) {
        throw ProjectFormatError("$.schemaVersion must be 1, 2 or 3");
    }
    project.schemaVersion = Project::currentSchemaVersion;
    project.id = requireMember(root, "id", JsonValue::Kind::String, "$").string;
    project.title = requireMember(root, "title", JsonValue::Kind::String, "$").string;

    const auto& accentValue = requireMember(root, "accent", JsonValue::Kind::String, "$").string;
    if (!tryParseAccent(accentValue, project.accent)) {
        throw ProjectFormatError("$.accent must be 'en-US' or 'en-GB'");
    }
    project.targetWpm = requireMember(root, "targetWpm", JsonValue::Kind::Number, "$").number;

    if (sourceSchemaVersion >= 2) {
        const JsonValue& voiceSettings =
            requireMember(root, "voiceSettings", JsonValue::Kind::Object, "$");
        project.voiceSettings.maleVoiceTokenId =
            requireMember(voiceSettings, "maleVoiceTokenId", JsonValue::Kind::String,
                          "$.voiceSettings").string;
        project.voiceSettings.femaleVoiceTokenId =
            requireMember(voiceSettings, "femaleVoiceTokenId", JsonValue::Kind::String,
                          "$.voiceSettings").string;
        project.voiceSettings.strictAccent =
            requireBoolean(voiceSettings, "strictAccent", "$.voiceSettings");
        project.voiceSettings.allowGenderFallback =
            requireBoolean(voiceSettings, "allowGenderFallback", "$.voiceSettings");
        project.renderedProgramFile =
            requireMember(root, "renderedProgramFile", JsonValue::Kind::String, "$").string;
    }

    const auto& segments = requireMember(root, "segments", JsonValue::Kind::Array, "$").array;
    project.segments.reserve(segments.size());
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const JsonValue& value = segments[index];
        const std::string path = "$.segments[" + std::to_string(index) + "]";
        if (value.kind != JsonValue::Kind::Object) {
            throw ProjectFormatError(path + " must be an object");
        }

        Segment segment;
        segment.id = requireMember(value, "id", JsonValue::Kind::String, path).string;
        segment.questions.first = requireInteger(value, "questionStart", path);
        segment.questions.last = requireInteger(value, "questionEnd", path);
        segment.speaker = requireMember(value, "speaker", JsonValue::Kind::String, path).string;
        segment.text = requireMember(value, "text", JsonValue::Kind::String, path).string;
        segment.pauseAfterSeconds =
            requireMember(value, "pauseAfterSeconds", JsonValue::Kind::Number, path).number;
        segment.repeatCount = requireInteger(value, "repeatCount", path);
        if (sourceSchemaVersion >= 2) {
            segment.renderedAudioFile =
                requireMember(value, "renderedAudioFile", JsonValue::Kind::String, path).string;
            const JsonValue* recording = optionalMember(value, "recording");
            if (recording != nullptr && recording->kind != JsonValue::Kind::Null) {
                if (recording->kind != JsonValue::Kind::Object) {
                    throw ProjectFormatError(path + ".recording must be an object or null");
                }
                RecordingSource source;
                const std::string recordingPath = path + ".recording";
                source.audioFile = requireMember(*recording, "audioFile", JsonValue::Kind::String,
                                                 recordingPath).string;
                const double startNumber = requireMember(
                    *recording, "startMs", JsonValue::Kind::Number, recordingPath).number;
                const double endNumber = requireMember(
                    *recording, "endMs", JsonValue::Kind::Number, recordingPath).number;
                // The largest uint64 value rounds to 2^64 in a double. Reject
                // that boundary and above before the narrowing conversion.
                constexpr double maximumUnsignedExclusive = 18446744073709551616.0;
                if (!std::isfinite(startNumber) || !std::isfinite(endNumber) ||
                    std::trunc(startNumber) != startNumber || std::trunc(endNumber) != endNumber ||
                    startNumber < 0.0 || endNumber < 0.0 ||
                    startNumber >= maximumUnsignedExclusive || endNumber >= maximumUnsignedExclusive) {
                    throw ProjectFormatError(recordingPath + ".startMs/endMs must be non-negative integers");
                }
                source.startMs = static_cast<std::uint64_t>(startNumber);
                source.endMs = static_cast<std::uint64_t>(endNumber);
                segment.recording = std::move(source);
            }
            const JsonValue* generation = optionalMember(value, "generation");
            if (generation == nullptr) {
                throw ProjectFormatError(path + ".generation is required");
            }
            if (generation->kind != JsonValue::Kind::Null) {
                if (generation->kind != JsonValue::Kind::Object) {
                    throw ProjectFormatError(path + ".generation must be an object or null");
                }
                GenerationRecord record;
                const std::string generationPath = path + ".generation";
                record.provider = requireMember(*generation, "provider", JsonValue::Kind::String,
                                                generationPath).string;
                record.model = requireMember(*generation, "model", JsonValue::Kind::String,
                                             generationPath).string;
                record.questionStem =
                    requireMember(*generation, "questionStem", JsonValue::Kind::String,
                                  generationPath).string;
                const auto& options = requireMember(*generation, "options", JsonValue::Kind::Array,
                                                    generationPath).array;
                if (options.size() != record.options.size()) {
                    throw ProjectFormatError(generationPath + ".options must contain three strings");
                }
                for (std::size_t option = 0; option < options.size(); ++option) {
                    if (options[option].kind != JsonValue::Kind::String) {
                        throw ProjectFormatError(generationPath + ".options must contain strings");
                    }
                    record.options[option] = options[option].string;
                }
                record.correctAnswer =
                    requireMember(*generation, "correctAnswer", JsonValue::Kind::String,
                                  generationPath).string;
                record.requiresTeacherReview =
                    requireBoolean(*generation, "requiresTeacherReview", generationPath);
                record.teacherReviewed =
                    requireBoolean(*generation, "teacherReviewed", generationPath);
                const auto& evidence =
                    requireMember(*generation, "evidence", JsonValue::Kind::Array,
                                  generationPath).array;
                record.evidence.reserve(evidence.size());
                for (std::size_t evidenceIndex = 0;
                     evidenceIndex < evidence.size();
                     ++evidenceIndex) {
                    const JsonValue& evidenceValue = evidence[evidenceIndex];
                    const std::string evidencePath = generationPath + ".evidence[" +
                                                     std::to_string(evidenceIndex) + "]";
                    if (evidenceValue.kind != JsonValue::Kind::Object) {
                        throw ProjectFormatError(evidencePath + " must be an object");
                    }
                    record.evidence.push_back(GenerationEvidence{
                        requireMember(evidenceValue, "option", JsonValue::Kind::String,
                                      evidencePath).string,
                        requireMember(evidenceValue, "role", JsonValue::Kind::String,
                                      evidencePath).string,
                        requireMember(evidenceValue, "turnId", JsonValue::Kind::String,
                                      evidencePath).string,
                        requireMember(evidenceValue, "quote", JsonValue::Kind::String,
                                      evidencePath).string,
                    });
                }
                const JsonValue* additionalQuestions =
                    optionalMember(*generation, "additionalQuestions");
                if (additionalQuestions != nullptr) {
                    if (additionalQuestions->kind != JsonValue::Kind::Array) {
                        throw ProjectFormatError(generationPath +
                                                 ".additionalQuestions must be an array");
                    }
                    record.additionalQuestions.reserve(additionalQuestions->array.size());
                    for (std::size_t questionIndex = 0;
                         questionIndex < additionalQuestions->array.size(); ++questionIndex) {
                        const JsonValue& questionValue = additionalQuestions->array[questionIndex];
                        const std::string questionPath = generationPath +
                            ".additionalQuestions[" + std::to_string(questionIndex) + "]";
                        if (questionValue.kind != JsonValue::Kind::Object) {
                            throw ProjectFormatError(questionPath + " must be an object");
                        }
                        GenerationQuestion question;
                        question.questionStem = requireMember(
                            questionValue, "questionStem", JsonValue::Kind::String,
                            questionPath).string;
                        const auto& questionOptions = requireMember(
                            questionValue, "options", JsonValue::Kind::Array,
                            questionPath).array;
                        if (questionOptions.size() != question.options.size()) {
                            throw ProjectFormatError(questionPath +
                                                     ".options must contain three strings");
                        }
                        for (std::size_t option = 0; option < questionOptions.size(); ++option) {
                            if (questionOptions[option].kind != JsonValue::Kind::String) {
                                throw ProjectFormatError(questionPath +
                                                         ".options must contain strings");
                            }
                            question.options[option] = questionOptions[option].string;
                        }
                        question.correctAnswer = requireMember(
                            questionValue, "correctAnswer", JsonValue::Kind::String,
                            questionPath).string;
                        const auto& questionEvidence = requireMember(
                            questionValue, "evidence", JsonValue::Kind::Array,
                            questionPath).array;
                        question.evidence.reserve(questionEvidence.size());
                        for (std::size_t evidenceIndex = 0;
                             evidenceIndex < questionEvidence.size(); ++evidenceIndex) {
                            const JsonValue& evidenceValue = questionEvidence[evidenceIndex];
                            const std::string evidencePath = questionPath + ".evidence[" +
                                std::to_string(evidenceIndex) + "]";
                            if (evidenceValue.kind != JsonValue::Kind::Object) {
                                throw ProjectFormatError(evidencePath + " must be an object");
                            }
                            question.evidence.push_back(GenerationEvidence{
                                requireMember(evidenceValue, "option", JsonValue::Kind::String,
                                              evidencePath).string,
                                requireMember(evidenceValue, "role", JsonValue::Kind::String,
                                              evidencePath).string,
                                requireMember(evidenceValue, "turnId", JsonValue::Kind::String,
                                              evidencePath).string,
                                requireMember(evidenceValue, "quote", JsonValue::Kind::String,
                                              evidencePath).string,
                            });
                        }
                        record.additionalQuestions.push_back(std::move(question));
                    }
                }
                segment.generation = std::move(record);
            }
        }
        project.segments.push_back(std::move(segment));
    }

    try {
        requireValid(project, purpose);
    } catch (const ValidationError& error) {
        throw ProjectFormatError(validationSummary(error));
    }
    return project;
}

void saveProject(const Project& project,
                 const std::filesystem::path& path,
                 ValidationPurpose purpose) {
    saveProjectAtomic(project, path, purpose);
}

void saveProjectAtomic(const Project& project,
                       const std::filesystem::path& path,
                       ValidationPurpose purpose) {
    if (path.empty()) {
        throw ProjectFormatError("Cannot atomically save to an empty project path");
    }
    const std::string json = toJson(project, true, purpose);

    const std::filesystem::path parent = path.has_parent_path()
                                             ? path.parent_path()
                                             : std::filesystem::path{"."};
    static std::atomic<std::uint64_t> sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto threadPart = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path temporary = parent / path.filename();
    temporary += ".tmp-" + std::to_string(now) + "-" + std::to_string(threadPart) + "-" +
                 std::to_string(serial);

    auto cleanup = [&temporary] {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    };

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ProjectFormatError("Cannot open temporary project file for writing: " +
                                     temporary.string());
        }
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        output.flush();
        output.close();
        if (!output) {
            cleanup();
            throw ProjectFormatError("Cannot write temporary project file: " +
                                     temporary.string());
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD errorCode = GetLastError();
        cleanup();
        throw ProjectFormatError("Cannot atomically replace project file (Win32 error " +
                                 std::to_string(errorCode) + "): " + path.string());
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        cleanup();
        throw ProjectFormatError("Cannot atomically replace project file: " + error.message());
    }
#endif
}

Project loadProject(const std::filesystem::path& path, ValidationPurpose purpose) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw ProjectFormatError("Cannot open project file: " + path.string());
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        throw ProjectFormatError("Cannot determine project file size: " + path.string());
    }
    if (static_cast<std::uintmax_t>(size) > maximumJsonBytes) {
        throw ProjectFormatError("Project file is larger than 64 MiB: " + path.string());
    }
    input.seekg(0, std::ios::beg);

    std::string json(static_cast<std::size_t>(size), '\0');
    if (!json.empty()) {
        input.read(json.data(), static_cast<std::streamsize>(json.size()));
    }
    if (!input && !input.eof()) {
        throw ProjectFormatError("Cannot read project file: " + path.string());
    }
    return fromJson(json, purpose);
}

}  // namespace listening
