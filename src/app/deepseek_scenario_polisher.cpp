#include "app/deepseek_scenario_polisher.h"

#include "core/project.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace listening::app {
namespace {

constexpr auto kEndpoint = "https://api.deepseek.com/chat/completions";
constexpr auto kModel = "deepseek-v4-flash";

QString qString(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QByteArray trimmedCredential(QByteArray value) {
    value = value.trimmed();
    const int equals = value.indexOf('=');
    if (equals >= 0) {
        value = value.mid(equals + 1).trimmed();
    }
    if ((value.startsWith('"') && value.endsWith('"')) ||
        (value.startsWith('\'') && value.endsWith('\''))) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

QByteArray readCredential(QString* source) {
    QByteArray value = trimmedCredential(qgetenv("DEEPSEEK_API_KEY"));
    if (!value.isEmpty()) {
        if (source != nullptr) {
            *source = QStringLiteral("环境变量 DEEPSEEK_API_KEY");
        }
        return value;
    }

    const QStringList candidates{
        QDir(qEnvironmentVariable("LOCALAPPDATA"))
            .filePath(QStringLiteral("Audiloquy/deepseek.txt")),
        QDir::current().filePath(QStringLiteral(".private/deepseek.txt")),
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral(".private/deepseek.txt")),
    };
    for (const QString& path : candidates) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        value = trimmedCredential(file.readAll());
        if (!value.isEmpty()) {
            if (source != nullptr) {
                *source = QStringLiteral("用户本机私有配置（未打包）");
            }
            return value;
        }
    }
    if (source != nullptr) {
        source->clear();
    }
    return {};
}

QString answerText(const ScenarioRequest& request) {
    if (!request.correctAnswer.has_value()) {
        return QStringLiteral("?");
    }
    return QString::fromLatin1(answerLabelCode(*request.correctAnswer).data(), 1);
}

QString evidenceRoleCode(EvidenceRole role) {
    return role == EvidenceRole::Supports ? QStringLiteral("supports")
                                          : QStringLiteral("rejects");
}

QString canonicalFact(std::string_view option) {
    QString fact = qString(option).trimmed();
    while (!fact.isEmpty() && QStringLiteral(".?!;:，。！？；：").contains(fact.back())) {
        fact.chop(1);
    }
    return fact.trimmed();
}

std::size_t dialogueWords(const ScenarioDraft& draft) {
    std::size_t count = 0;
    for (const auto& turn : draft.turns) {
        count += countReadableWords(turn.text);
    }
    return count;
}

QJsonObject requestBody(const ScenarioRequest& request, const ScenarioDraft& localDraft) {
    QJsonArray turns;
    for (const auto& turn : localDraft.turns) {
        const QString id = qString(turn.id);
        turns.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("speaker"),
             turn.speaker == SpeakerGender::Male ? QStringLiteral("MAN")
                                                 : QStringLiteral("WOMAN")},
            {QStringLiteral("text"), qString(turn.text)},
        });
    }
    QJsonArray evidence;
    for (const auto& item : localDraft.evidence) {
        const std::size_t optionIndex = static_cast<std::size_t>(item.option);
        evidence.append(QJsonObject{
            {QStringLiteral("option"), qString(answerLabelCode(item.option))},
            {QStringLiteral("role"), evidenceRoleCode(item.role)},
            {QStringLiteral("turn_id"), qString(item.turnId)},
            {QStringLiteral("quote"), qString(item.quote)},
            {QStringLiteral("canonical_fact"), qString(request.options[optionIndex])},
        });
    }
    QJsonObject source{
        {QStringLiteral("question"), qString(request.questionStem)},
        {QStringLiteral("options"),
         QJsonArray{qString(request.options[0]), qString(request.options[1]),
                    qString(request.options[2])}},
        {QStringLiteral("correct_answer"), answerText(request)},
        {QStringLiteral("topic"), qString(request.topic)},
        {QStringLiteral("difficulty"),
         request.difficulty.has_value() ? qString(*request.difficulty) : QString()},
        {QStringLiteral("turns"), turns},
        {QStringLiteral("evidence"), evidence},
    };

    const QString system = QStringLiteral(
        "You polish short English listening-test dialogue for Chinese high-school learners. "
        "Return json only in exactly this shape: {\"turns\":[{\"id\":\"turn-01\","
        "\"text\":\"...\"}],\"evidence\":[{\"option\":\"A\",\"role\":\"rejects\","
        "\"turn_id\":\"turn-03\",\"quote\":\"...\"}]}. Keep every turn id, order, "
        "speaker allocation and turn count. You may rewrite every turn, including evidence turns, "
        "to sound like natural spoken classroom English at the requested CEFR level. Preserve "
        "exactly one evidence entry for each supplied option/role pair. Its quote must be an exact "
        "substring of that returned turn and must retain the supplied canonical_fact words "
        "verbatim (case may change) so the local verifier can anchor the fact. Do not add facts, "
        "change which option is supported, quote the question, add role labels, markdown or "
        "explanations. Keep total length within five words of the source dialogue.");
    const QString user = QStringLiteral(
                             "Polish this validated local-first draft. The JSON source is:\n%1")
                             .arg(QString::fromUtf8(
                                 QJsonDocument(source).toJson(QJsonDocument::Compact)));

    return QJsonObject{
        {QStringLiteral("model"), QString::fromLatin1(kModel)},
        {QStringLiteral("messages"),
         QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                {QStringLiteral("content"), system}},
                    QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), user}}}},
        {QStringLiteral("response_format"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}}},
        {QStringLiteral("thinking"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("disabled")}}},
        {QStringLiteral("max_tokens"), 1200},
        {QStringLiteral("stream"), false},
    };
}

[[noreturn]] void fail(QString message) {
    throw std::runtime_error(message.toUtf8().constData());
}

}  // namespace

bool DeepSeekScenarioPolisher::credentialAvailable() {
    return !readCredential(nullptr).isEmpty();
}

QString DeepSeekScenarioPolisher::credentialDescription() {
    QString source;
    (void)readCredential(&source);
    return source.isEmpty() ? QStringLiteral("未配置") : source;
}

ScenarioDraft DeepSeekScenarioPolisher::applyPolishJson(const ScenarioRequest& request,
                                                        const ScenarioDraft& localDraft,
                                                        const QByteArray& responseJson) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(responseJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(QStringLiteral("API 返回的润色内容不是有效 JSON。"));
    }
    const QJsonArray turns = document.object().value(QStringLiteral("turns")).toArray();
    if (turns.size() != static_cast<qsizetype>(localDraft.turns.size())) {
        fail(QStringLiteral("API 改变了对话轮次数，已拒绝采用。"));
    }

    ScenarioDraft candidate = localDraft;
    for (qsizetype index = 0; index < turns.size(); ++index) {
        const QJsonObject turn = turns[index].toObject();
        const QString expectedId = qString(localDraft.turns[static_cast<std::size_t>(index)].id);
        const QString id = turn.value(QStringLiteral("id")).toString();
        const QString text = turn.value(QStringLiteral("text")).toString().trimmed();
        if (id != expectedId || text.isEmpty() || text.size() > 2000) {
            fail(QStringLiteral("API 返回了无效或错位的对话轮次，已拒绝采用。"));
        }
        const QByteArray utf8Text = text.toUtf8();
        candidate.turns[static_cast<std::size_t>(index)].text =
            std::string(utf8Text.constData(), static_cast<std::size_t>(utf8Text.size()));
    }

    const QJsonArray evidence = document.object().value(QStringLiteral("evidence")).toArray();
    if (evidence.size() != static_cast<qsizetype>(localDraft.evidence.size())) {
        fail(QStringLiteral("API 未完整返回三个选项的事实证据，已拒绝采用。"));
    }
    candidate.evidence.clear();
    std::vector<bool> matched(localDraft.evidence.size(), false);
    for (const QJsonValue& value : evidence) {
        if (!value.isObject()) {
            fail(QStringLiteral("API 返回了无效的事实证据，已拒绝采用。"));
        }
        const QJsonObject object = value.toObject();
        const QString optionText = object.value(QStringLiteral("option")).toString().trimmed();
        const QString roleText = object.value(QStringLiteral("role")).toString().trimmed();
        const QString turnId = object.value(QStringLiteral("turn_id")).toString().trimmed();
        const QString quote = object.value(QStringLiteral("quote")).toString().trimmed();
        if (optionText.size() != 1 || quote.isEmpty() || turnId.isEmpty()) {
            fail(QStringLiteral("API 返回的事实证据字段不完整，已拒绝采用。"));
        }

        std::size_t expectedIndex = localDraft.evidence.size();
        for (std::size_t index = 0; index < localDraft.evidence.size(); ++index) {
            const auto& expected = localDraft.evidence[index];
            if (!matched[index] && optionText == qString(answerLabelCode(expected.option)) &&
                roleText == evidenceRoleCode(expected.role)) {
                expectedIndex = index;
                break;
            }
        }
        if (expectedIndex == localDraft.evidence.size()) {
            fail(QStringLiteral("API 改变了选项的支持/排除关系，已拒绝采用。"));
        }
        matched[expectedIndex] = true;
        const auto& expected = localDraft.evidence[expectedIndex];
        const QString fact = canonicalFact(request.options[static_cast<std::size_t>(expected.option)]);
        if (fact.isEmpty() || !quote.contains(fact, Qt::CaseInsensitive)) {
            fail(QStringLiteral("API 证据未保留选项事实“%1”，已拒绝采用。").arg(fact));
        }

        const auto turn = std::find_if(candidate.turns.begin(), candidate.turns.end(),
                                       [&](const DialogueTurn& item) {
                                           return qString(item.id) == turnId;
                                       });
        if (turn == candidate.turns.end()) {
            fail(QStringLiteral("API 证据引用了不存在的对话轮次，已拒绝采用。"));
        }
        const QByteArray turnBytes = QByteArray::fromStdString(turn->text);
        const QByteArray quoteBytes = quote.toUtf8();
        const qsizetype offset = turnBytes.indexOf(quoteBytes);
        if (offset < 0) {
            fail(QStringLiteral("API 证据不是返回文稿的精确片段，已拒绝采用。"));
        }
        candidate.evidence.push_back(ScenarioEvidence{
            expected.option,
            expected.role,
            turn->id,
            static_cast<std::size_t>(offset),
            static_cast<std::size_t>(quoteBytes.size()),
            std::string(quoteBytes.constData(), static_cast<std::size_t>(quoteBytes.size())),
        });
    }
    for (OptionJudgment& judgment : candidate.optionJudgments) {
        judgment.evidenceTurnIds.clear();
        for (const ScenarioEvidence& item : candidate.evidence) {
            if (item.option == judgment.option &&
                std::find(judgment.evidenceTurnIds.begin(), judgment.evidenceTurnIds.end(),
                          item.turnId) == judgment.evidenceTurnIds.end()) {
                judgment.evidenceTurnIds.push_back(item.turnId);
            }
        }
    }
    candidate.wordCount = dialogueWords(candidate);
    const auto issues = validateScenarioDraft(request, candidate);
    if (!issues.empty()) {
        fail(QStringLiteral("API 润色未通过本地答案与结构校验（%1），已保留本地初稿。")
                 .arg(qString(issues.front().path)));
    }
    return candidate;
}

DeepSeekPolishResult DeepSeekScenarioPolisher::polish(const ScenarioRequest& request,
                                                      const ScenarioDraft& localDraft) const {
    const auto localIssues = validateScenarioDraft(request, localDraft);
    if (!localIssues.empty()) {
        fail(QStringLiteral("只有通过本地校验的初稿才能提交润色。"));
    }
    const QByteArray credential = readCredential(nullptr);
    if (credential.isEmpty()) {
        fail(QStringLiteral("未找到 DeepSeek 密钥；本地生成仍可正常使用。"));
    }

    QNetworkAccessManager manager;
    QNetworkRequest networkRequest(QUrl(QString::fromLatin1(kEndpoint)));
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                             QStringLiteral("application/json"));
    networkRequest.setRawHeader("Authorization", "Bearer " + credential);
    networkRequest.setTransferTimeout(35000);
    const QByteArray payload =
        QJsonDocument(requestBody(request, localDraft)).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = manager.post(networkRequest, payload);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(35000);
    loop.exec();
    timer.stop();

    const auto networkError = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray response = reply->readAll();
    reply->deleteLater();
    if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
        fail(status == 0 ? QStringLiteral("DeepSeek 请求超时或网络不可用；已保留本地初稿。")
                         : QStringLiteral("DeepSeek 返回 HTTP %1；已保留本地初稿。")
                               .arg(status));
    }

    QJsonParseError parseError;
    const QJsonDocument envelope = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !envelope.isObject()) {
        fail(QStringLiteral("DeepSeek 响应无法解析；已保留本地初稿。"));
    }
    const QJsonObject root = envelope.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        fail(QStringLiteral("DeepSeek 未返回润色内容；已保留本地初稿。"));
    }
    const QJsonObject choice = choices.first().toObject();
    if (choice.value(QStringLiteral("finish_reason")).toString() != QStringLiteral("stop")) {
        fail(QStringLiteral("DeepSeek 输出未完整结束；已保留本地初稿。"));
    }
    const QString content =
        choice.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
    if (content.trimmed().isEmpty()) {
        fail(QStringLiteral("DeepSeek 返回了空内容；已保留本地初稿。"));
    }

    DeepSeekPolishResult result;
    result.draft = applyPolishJson(request, localDraft, content.toUtf8());
    result.model = root.value(QStringLiteral("model")).toString(QString::fromLatin1(kModel));
    const QJsonObject usage = root.value(QStringLiteral("usage")).toObject();
    result.promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt();
    result.completionTokens = usage.value(QStringLiteral("completion_tokens")).toInt();
    return result;
}

}  // namespace listening::app
