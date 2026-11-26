// The logic of the translate plugin (src/translate-plugin/Translate): which
// language to translate into, what is sent to DeepL and to an OpenAI-
// compatible endpoint, how the answers and the services' errors are read,
// and the rows in both languages. HTTP runs only against a local test server.

#include "Translate.h"
#include "Http.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include <winhttp.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>

using namespace Qt::StringLiterals;
using winshun::Json;

namespace {

Json parse(const std::string& text)
{
    const auto json = Json::parse(text);
    return json ? *json : Json();
}

tr::Settings openai()
{
    tr::Settings s;
    s.provider = "openai";
    s.key = "sk-test";
    return s;
}

tr::Settings deepl(const char* key)
{
    tr::Settings s;
    s.provider = "deepl";
    s.key = key;
    return s;
}

bool validUtf8(const std::string& s)
{
    // Json::quote writes U+FFFD for any byte that does not fit; so a clean
    // string survives a round trip unchanged.
    const auto back = Json::parse(Json::quote(s));
    return back && back->toString() == s;
}

tr::Settings local()
{
    return tr::settingsFrom(Json::Object {{"provider", "local"}});
}

QString text(const std::string& s) { return QString::fromStdString(s); }

// One server-sent event of a chat completion, as llama.cpp sends it.
QByteArray sse(const char* content)
{
    const Json data(Json::Object {{"choices", Json::Array {Json::Object {{"delta", Json::Object {{"content", content}}}}}}});
    return "data: " + QByteArray::fromStdString(data.dump()) + "\n\n";
}

QByteArray chunk(const QByteArray& data) { return QByteArray::number(data.size(), 16) + "\r\n" + data + "\r\n"; }

// WinShunTranslate.exe spoken to as Win顺 speaks to it: the answers by id,
// and how many times it asked to be asked again (winshun/refresh).
class Runner {
public:
    Runner()
    {
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [this] {
            output += process.readAllStandardOutput();
            while (output.contains('\n')) {
                const auto end = output.indexOf('\n');
                const auto message = Json::parse(output.first(end).toStdString());
                output.remove(0, end + 1);
                if (message && (*message)["id"].isNumber())
                    replies[(*message)["id"].toInt()] = *message;
                else if (message && (*message)["method"].toString() == "winshun/refresh")
                    ++refreshes;
            }
        });
        process.start(QCoreApplication::applicationDirPath() + QStringLiteral("/WinShunTranslate.exe"));
    }
    ~Runner()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
        }
    }

    void send(const char* method, Json params, int id = 0)
    {
        Json message(Json::Object {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
        if (id)
            message.set("id", id);
        process.write(QByteArray::fromStdString(message.dump()) + '\n');
    }
    bool answered(long long id, int ms = 5000)
    {
        return QTest::qWaitFor([&] { return replies.contains(id); }, ms);
    }
    Json item(long long id, int index = 0) { return replies[id]["result"]["items"].toArray()[index]; }

    QProcess process;
    QByteArray output;
    std::map<long long, Json> replies;
    int refreshes = 0;
};

} // namespace

class TestTranslate : public QObject {
    Q_OBJECT

private slots:
    void protocolKeepsReadingWhileTranslationIsPending()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        bool oldReceived = false;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto* socket = server.nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, input, sent = false]() mutable {
                    input->append(socket->readAll());
                    if (sent || !input->contains("\r\n\r\n") || !input->endsWith('}'))
                        return;
                    sent = true;
                    const bool old = input->contains("old query");
                    oldReceived |= old;
                    const auto respond = [socket] {
                        const QByteArray body = R"({"choices":[{"message":{"content":"new answer"}}]})";
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    };
                    if (old)
                        QTimer::singleShot(2500, socket, respond);
                    else
                        respond();
                });
            }
        });
        QTemporaryDir data;
        QVERIFY(data.isValid());
        QProcess process;
        QByteArray output;
        std::map<long long, Json> replies;
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [&] {
            output += process.readAllStandardOutput();
            while (output.contains('\n')) {
                const auto end = output.indexOf('\n');
                const auto message = Json::parse(output.first(end).toStdString());
                output.remove(0, end + 1);
                if (message && (*message)["id"].isNumber())
                    replies[(*message)["id"].toInt()] = *message;
            }
        });
        process.start(QCoreApplication::applicationDirPath() + QStringLiteral("/WinShunTranslate.exe"));
        QVERIFY(process.waitForStarted());
        const auto send = [&](const char* method, Json params, int id = 0) {
            Json message(Json::Object {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
            if (id)
                message.set("id", id);
            process.write(QByteArray::fromStdString(message.dump()) + '\n');
        };
        send("initialize", Json::Object {{"locale", "en"}, {"dataDir", data.path().toStdString()},
            {"settings", Json::Object {{"provider", "openai"}, {"apiKey", "fixture"},
                {"endpoint", "http://127.0.0.1:" + std::to_string(server.serverPort())}}}}, 1);
        QVERIFY(QTest::qWaitFor([&] { return replies.contains(1); }));
        send("query", Json::Object {{"queryId", 1}, {"command", "tr"}, {"text", "old query"}}, 2);
        QVERIFY(QTest::qWaitFor([&] { return oldReceived; }));
        QElapsedTimer elapsed;
        elapsed.start();
        send("cancel", Json::Object {{"queryId", 1}});
        send("query", Json::Object {{"queryId", 2}, {"command", "tr"}, {"text", "new query"}}, 3);
        QVERIFY(QTest::qWaitFor([&] { return replies.contains(3); }, 1500));
        QVERIFY(elapsed.elapsed() < 1200);
        QCOMPARE(replies[3]["result"]["items"].toArray()[0]["title"].toString(), std::string("new answer"));
        QCOMPARE(replies[3]["result"]["cache"].toInt(), 300LL);
        send("shutdown", Json::Object {}, 4);
        QVERIFY(QTest::qWaitFor([&] { return process.state() == QProcess::NotRunning; }));
        QCOMPARE(process.exitCode(), 0);
    }

    // A chat service typed to in the launcher: the reply as it comes, asked
    // again (rerun, refresh) until it is all there; the local models' own
    // "not yet" as a row that opens their settings card.
    void protocolStreamsAnAnswerThatTakesLong()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QByteArray request;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto* socket = server.nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, input, sent = false]() mutable {
                    input->append(socket->readAll());
                    if (sent || !input->contains("\r\n\r\n") || !input->endsWith('}'))
                        return;
                    sent = true;
                    request = *input;
                    // One request a connection (Connection: close), so that
                    // WinHTTP does not send the next on this one.
                    if (input->contains("Fehler")) {
                        const QByteArray body = R"({"error":{"code":503,"type":"winshun_local_model","message":"no model yet"}})";
                        socket->write("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: "
                            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        socket->disconnectFromHost();
                        return;
                    }
                    if (!input->contains(R"("stream":true)")) {
                        const QByteArray body = R"({"choices":[{"message":{"content":"你好"}}]})";
                        socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        socket->disconnectFromHost();
                        return;
                    }
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
                    socket->write(chunk(sse("你")));
                    QTimer::singleShot(2200, socket, [socket] {
                        socket->write(chunk(sse("好") + "data: [DONE]\n\n"));
                        socket->write("0\r\n\r\n");
                        socket->disconnectFromHost();
                    });
                });
            }
        });
        QTemporaryDir data;
        QVERIFY(data.isValid());
        Runner plugin;
        QVERIFY(plugin.process.waitForStarted());
        plugin.send("initialize", Json::Object {{"locale", "en"}, {"dataDir", data.path().toStdString()},
            {"capabilities", Json::Object {{"settingsLinks", true}}},
            {"settings", Json::Object {{"provider", "local"}, {"endpoint", "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/v1"}}}}, 1);
        QVERIFY(plugin.answered(1));

        QElapsedTimer elapsed;
        elapsed.start();
        plugin.send("query", Json::Object {{"queryId", 1}, {"command", "tr"}, {"text", "Hallo"}}, 2);
        QVERIFY(plugin.answered(2));
        QVERIFY2(elapsed.elapsed() < 2000, qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY(request.contains("POST /v1/chat/completions HTTP/1.1"));
        QVERIFY(request.contains(R"("stream":true)"));
        QVERIFY(!request.contains("Authorization"));
        QCOMPARE(text(plugin.item(2)["id"].toString()), u"working"_s);
        QCOMPARE(text(plugin.item(2)["title"].toString()), u"你"_s);
        QCOMPARE(plugin.replies[2]["result"]["rerun"].toInt(), 2LL);
        QCOMPARE(plugin.replies[2]["result"]["cache"].toInt(), 0LL);

        QVERIFY(QTest::qWaitFor([&] { return plugin.refreshes >= 2; }, 5000)); // the rest came, then the end
        QVERIFY(elapsed.elapsed() > 2200);
        plugin.send("query", Json::Object {{"queryId", 2}, {"command", "tr"}, {"text", "Hallo"}}, 3);
        QVERIFY(plugin.answered(3, 1000));
        QCOMPARE(text(plugin.item(3)["title"].toString()), u"你好"_s);
        QCOMPARE(plugin.replies[3]["result"]["cache"].toInt(), 300LL);
        QVERIFY(!plugin.replies[3]["result"].has("rerun"));

        plugin.send("query", Json::Object {{"queryId", 3}, {"command", "tr"}, {"text", "Fehler"}}, 4);
        QVERIFY(plugin.answered(4, 2000));
        QCOMPARE(text(plugin.item(4)["id"].toString()), u"setup"_s);
        QCOMPARE(text(plugin.item(4)["title"].toString()), u"The local model cannot translate yet: no model yet"_s);
        QCOMPARE(text(plugin.item(4)["open"]["url"].toString()), u"winshun:settings/plugin/winshun.localmodel"_s);

        // A command call (translate_text, the AI) is answered in one go.
        plugin.send("query", Json::Object {{"queryId", 4}, {"command", "tr"}, {"text", "Hallo Welt"}, {"source", "command"}}, 5);
        QVERIFY(plugin.answered(5, 5000));
        QCOMPARE(text(plugin.item(5)["title"].toString()), u"你好"_s);

        plugin.send("shutdown", Json::Object {}, 6);
        QVERIFY(QTest::qWaitFor([&] { return plugin.process.state() == QProcess::NotRunning; }));
        QCOMPARE(plugin.process.exitCode(), 0);
    }

    // Nothing listening at the local models' address: what to start.
    void protocolSaysWhatToStart()
    {
        quint16 port = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            port = probe.serverPort(); // free again when it closes
        }
        QTemporaryDir data;
        QVERIFY(data.isValid());
        Runner plugin;
        QVERIFY(plugin.process.waitForStarted());
        plugin.send("initialize", Json::Object {{"locale", "zh"}, {"dataDir", data.path().toStdString()},
            {"settings", Json::Object {{"provider", "local"}, {"endpoint", "http://127.0.0.1:" + std::to_string(port) + "/v1"}}}}, 1);
        QVERIFY(plugin.answered(1));
        plugin.send("query", Json::Object {{"queryId", 1}, {"command", "tr"}, {"text", "hello"}}, 2);
        QVERIFY(plugin.answered(2));
        long long last = 2;
        if (plugin.item(2)["id"].toString() == "working") {
            // Windows tries a refused connection on this computer again for
            // about two seconds: the answer comes after a refresh.
            QVERIFY(QTest::qWaitFor([&] { return plugin.refreshes > 0; }, 8000));
            plugin.send("query", Json::Object {{"queryId", 2}, {"command", "tr"}, {"text", "hello"}}, 3);
            QVERIFY(plugin.answered(3));
            last = 3;
        }
        QCOMPARE(text(plugin.item(last)["id"].toString()), u"setup"_s);
        QCOMPARE(text(plugin.item(last)["title"].toString()), u"本地模型没有在运行"_s);
        // An older Win顺 cannot open another plugin's card: its own settings.
        QCOMPARE(text(plugin.item(last)["open"]["url"].toString()), u"winshun:settings"_s);
        plugin.send("shutdown", Json::Object {}, 4);
        QVERIFY(QTest::qWaitFor([&] { return plugin.process.state() == QProcess::NotRunning; }));
    }

    void localService()
    {
        const tr::Settings s = local();
        QCOMPARE(s.endpoint, std::string(tr::kLocalEndpoint));
        QCOMPARE(s.model, std::string("local"));
        QVERIFY(!tr::needsKey(s));
        QVERIFY(tr::chatService(s));
        QVERIFY(tr::loopback("http://127.0.0.1:39281/v1"));
        QVERIFY(tr::loopback("http://LOCALHOST:11434/v1"));
        QVERIFY(tr::loopback("http://[::1]:1234/v1"));
        QVERIFY(tr::loopback("http://user@127.0.0.1/v1"));
        QVERIFY(!tr::loopback("https://127.0.0.1.example.com/v1"));
        QVERIFY(!tr::loopback("https://api.openai.com/v1?host=127.0.0.1"));
        QVERIFY(!tr::loopback("127.0.0.1:11434"));

        // Ollama or LM Studio as "OpenAI-compatible": no key needed, none sent.
        auto ollama = tr::settingsFrom(Json::Object {{"provider", "openai"}, {"endpoint", "http://localhost:11434/v1"}});
        QVERIFY(!tr::needsKey(ollama));
        QVERIFY(tr::buildRequest(ollama, "hi", "zh").authorization.empty());
        QVERIFY(tr::needsKey(tr::settingsFrom(Json::Object {{"provider", "openai"}})));
        QVERIFY(tr::needsKey(tr::settingsFrom(Json::Object {{"provider", "deepl"}})));
        QVERIFY(!tr::chatService(tr::settingsFrom(Json::Object {{"provider", "microsoft"}})));
        // Its port changed in the local models' settings: the endpoint says so.
        QCOMPARE(tr::settingsFrom(Json::Object {{"provider", "local"}, {"endpoint", "http://127.0.0.1:40000/v1"}}).endpoint,
            std::string("http://127.0.0.1:40000/v1"));
    }

    void localRequest()
    {
        const auto content = [](const tr::Request& r) { return text(parse(r.body)["messages"].toArray()[0]["content"].toString()); };
        tr::Request r = tr::buildRequest(local(), "你好", "en");
        QCOMPARE(r.url, std::string("http://127.0.0.1:39281/v1/chat/completions"));
        QVERIFY(r.authorization.empty());
        Json body = parse(r.body);
        QCOMPARE(body["messages"].toArray().size(), std::size_t(1)); // no system prompt: HY-MT has none
        QCOMPARE(content(r), u"将以下文本翻译为英语，注意只需要输出翻译后的结果，不要额外解释：\n\n你好"_s);
        QCOMPARE(body["chat_template_kwargs"]["enable_thinking"].toBool(), false);
        QCOMPARE(body["max_tokens"].toInt(), 256LL);
        QVERIFY(!body.has("stream"));
        QVERIFY(parse(tr::buildRequest(local(), "你好", "en", false, true).body)["stream"].toBool());

        QCOMPARE(content(tr::buildRequest(local(), "hello", "zh")), u"将以下文本翻译为中文，注意只需要输出翻译后的结果，不要额外解释：\n\nhello"_s);
        QCOMPARE(content(tr::buildRequest(local(), "hello", "ja")),
            u"Translate the following segment into Japanese, without additional explanation.\n\nhello"_s);
        QVERIFY(content(tr::buildRequest(local(), "one\n\ntwo", "zh")).endsWith(u"\n\n1. one\n2. two\n"_s));
        QVERIFY(content(tr::buildRequest(local(), "one\ntwo", "zh")).contains(u"以下 2 行文本"_s));

        // A long text may take as long an answer; a short one not much.
        QCOMPARE(parse(tr::buildRequest(local(), std::string(1000, 'a'), "zh").body)["max_tokens"].toInt(), 3064LL);
        QCOMPARE(parse(tr::buildRequest(local(), std::string(9000, 'a'), "zh").body)["max_tokens"].toInt(), 8192LL);

        body = parse(tr::buildRequest(local(), "serendipity", "zh", true, false).body);
        QCOMPARE(body["response_format"]["type"].toString(), std::string("json_object"));
        QVERIFY(text(body["messages"].toArray()[0]["content"].toString()).contains(u"\"serendipity\""_s));
    }

    void localNumberedAnswers()
    {
        QCOMPARE(text(tr::answerFromText(local(), "1. 甲\n2) 乙\n3、丙", 3).text), u"甲\n乙\n丙"_s);
        // One missing, or not numbered: kept as it came.
        QCOMPARE(text(tr::answerFromText(local(), "1. 甲\n2. 乙", 3).text), u"1. 甲\n2. 乙"_s);
        QCOMPARE(text(tr::answerFromText(local(), "甲\n乙", 2).text), u"甲\n乙"_s);
        QCOMPARE(text(tr::answerFromText(local(), "1. 甲\n3. 乙", 2).text), u"1. 甲\n3. 乙"_s);
        // One line sent: a number that is part of the text stays.
        QCOMPARE(text(tr::answerFromText(local(), "1. 甲", 1).text), u"1. 甲"_s);
        // The array other models are asked for is read too.
        QCOMPARE(text(tr::answerFromText(local(), R"(["甲","乙"])", 2).text), u"甲\n乙"_s);
        QCOMPARE(text(tr::answerFromText(openai(), "1. 甲\n2. 乙", 2).text), u"1. 甲\n2. 乙"_s);

        const std::string reply = R"({"choices":[{"message":{"content":"1. 甲\n2. 乙"}}]})";
        QCOMPARE(text(tr::parseAnswer(local(), reply, true, 2).text), u"甲\n乙"_s);
    }

    void streamReader()
    {
        tr::StreamReader reader;
        const QByteArray stream = ": comment\r\nevent: message\r\n" + sse("Hel") + sse("lo") + "data: [DONE]\n\n";
        for (qsizetype i = 0; i < stream.size(); i += 7) // in pieces that cut lines anywhere
            reader.feed(std::string_view(stream.constData() + i, std::min<qsizetype>(7, stream.size() - i)));
        QVERIFY(reader.events());
        QVERIFY(reader.done());
        QVERIFY(reader.error().empty());
        QCOMPARE(reader.text(), std::string("Hello"));

        tr::StreamReader partial;
        partial.feed(sse("你").toStdString() + "data: {\"choi");
        QCOMPARE(text(partial.text()), u"你"_s);
        QVERIFY(!partial.done());

        tr::StreamReader failed;
        failed.feed("data: {\"error\":{\"message\":\"context full\"}}\n\n");
        QCOMPARE(failed.error(), std::string("context full"));

        tr::StreamReader plain; // a server that does not stream
        plain.feed(R"({"choices":[{"message":{"content":"x"}}]})");
        QVERIFY(!plain.events());
        QVERIFY(plain.text().empty());
    }

    void localRows()
    {
        QCOMPARE(tr::localModelMessage(R"({"error":{"type":"winshun_local_model","message":"loading"}})"), std::string("loading"));
        QVERIFY(tr::localModelMessage(R"({"error":{"type":"server_error","message":"x"}})").empty());
        QVERIFY(tr::localModelMessage("not json").empty());

        winshun::Item row = tr::localModelRow("还没有模型", true, true);
        QCOMPARE(text(row.title), u"本地模型还不能用：还没有模型"_s);
        QCOMPARE(text(row.open["url"].toString()), u"winshun:settings/plugin/winshun.localmodel"_s);
        QVERIFY(row.valid);
        QCOMPARE(text(tr::localModelRow("x", false, false).open["url"].toString()), u"winshun:settings"_s);

        row = tr::notRunningRow(local(), "127.0.0.1:39281", true, true);
        QCOMPARE(text(row.open["url"].toString()), u"winshun:settings/plugin/winshun.localmodel"_s);
        const auto ollama = tr::settingsFrom(Json::Object {{"provider", "openai"}, {"endpoint", "http://127.0.0.1:11434/v1"}});
        row = tr::notRunningRow(ollama, "127.0.0.1:11434", false, true);
        QCOMPARE(text(row.title), u"No model server is running here (127.0.0.1:11434)"_s);
        QCOMPARE(text(row.open["url"].toString()), u"winshun:settings"_s);

        row = tr::streamingRow("hello", "", "zh", false, true);
        QCOMPARE(text(row.title), u"翻译中…"_s);
        QCOMPARE(text(row.subtitle), u"hello"_s);
        QVERIFY(!row.valid);
        row = tr::streamingRow("hello", "", "zh", true, true);
        QCOMPARE(text(row.subtitle), u"本地模型加载中，第一次要几秒…"_s);
        row = tr::streamingRow("hello", " 你好", "zh", false, false);
        QCOMPARE(text(row.title), u"你好"_s);
        QCOMPARE(text(row.subtitle), u"→ Chinese · translating…"_s);
    }

    // A streamed answer: each piece as it comes, and the whole in the body.
    void httpStreamsAndSaysWhatIsNotRunning()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket* socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, sent = false]() mutable {
                socket->readAll();
                if (std::exchange(sent, true))
                    return;
                socket->write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunk(sse("a")));
                QTimer::singleShot(300, socket, [socket] { socket->write(chunk(sse("b")) + "0\r\n\r\n"); });
            });
        });
        const std::string url = "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/v1/chat/completions";
        std::vector<std::string> pieces;
        std::mutex mutex;
        auto result = std::async(std::launch::async, [&] {
            return tr::post(url, "{}", {}, 3000, nullptr, {}, [&](std::string_view piece) {
                const std::lock_guard lock(mutex);
                pieces.emplace_back(piece);
            });
        });
        QVERIFY(QTest::qWaitFor([&] { return result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 3000));
        const tr::Response response = result.get();
        QCOMPARE(response.error, DWORD(0));
        QCOMPARE(response.host, "127.0.0.1:" + std::to_string(server.serverPort()));
        QVERIFY(pieces.size() >= 2);
        std::string joined;
        for (const std::string& piece : pieces)
            joined += piece;
        QCOMPARE(joined, response.body);
        tr::StreamReader reader;
        reader.feed(response.body);
        QCOMPARE(reader.text(), std::string("ab"));

        quint16 closed = 0; // a port nothing listens at (WinHTTP keeps the one above open)
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            closed = probe.serverPort();
        }
        const tr::Response refused = tr::post("http://127.0.0.1:" + std::to_string(closed) + "/v1/chat/completions", "{}", {}, 8000);
        QVERIFY2(tr::notConnected(refused), qPrintable(QString::number(refused.error)));
        QCOMPARE(text(tr::networkError(refused, true)), text("本机的 " + refused.host + " 没有程序在回答"));
    }

    void providerPresetsAndMicrosoftRequest()
    {
        auto s = tr::settingsFrom(Json::Object {{"provider", "deepseek"}, {"apiKey", "fixture"}});
        QCOMPARE(s.endpoint, std::string("https://api.deepseek.com"));
        QCOMPARE(s.model, std::string("deepseek-flash"));
        s = tr::settingsFrom(Json::Object {{"provider", "doubao"}, {"model", "ep-fixture"}});
        QCOMPARE(s.endpoint, std::string("https://ark.cn-beijing.volces.com/api/v3"));
        QCOMPARE(s.model, std::string("ep-fixture"));
        s = tr::settingsFrom(Json::Object {{"provider", "hunyuan"}, {"endpoint", "https://custom.example/v1"}});
        QCOMPARE(s.endpoint, std::string("https://custom.example/v1"));
        QCOMPARE(s.model, std::string("hunyuan-turbos-latest"));

        s = tr::settingsFrom(Json::Object {{"provider", "microsoft"}, {"apiKey", "fixture"}, {"region", "eastasia"}});
        const auto request = tr::buildRequest(s, "hello", "zh");
        QCOMPARE(request.url, std::string("https://api.cognitive.microsofttranslator.com/translate?api-version=3.0&to=zh-Hans"));
        QCOMPARE(request.headers.at("Ocp-Apim-Subscription-Key"), std::string("fixture"));
        QCOMPARE(request.headers.at("Ocp-Apim-Subscription-Region"), std::string("eastasia"));
        QCOMPARE(parse(request.body).toArray()[0]["Text"].toString(), std::string("hello"));
        const auto answer = tr::parseAnswer(s, R"([{"detectedLanguage":{"language":"en"},"translations":[{"text":"你好","to":"zh-Hans"}]}])", true);
        QVERIFY(answer.ok);
        QCOMPARE(answer.text, std::string("你好"));
        QVERIFY(!tr::parseAnswer(s, "[]", true).ok);
    }

    void dictionaryRequestAndRows()
    {
        const auto request = tr::buildRequest(openai(), "hello", "zh", true);
        QVERIFY(parse(request.body)["messages"].toArray()[0]["content"].toString().find("bilingual dictionary") != std::string::npos);
        const std::string body = R"({"term":"hello","phonetic":"/həˈləʊ/","definitions":[{"partOfSpeech":"interjection","meaning":"你好"}],"examples":[{"text":"Hello, world!","translation":"你好，世界！"}]})";
        const auto rows = tr::dictionaryRows("hello", "```json\n" + body + "\n```", true);
        QCOMPARE(rows.size(), std::size_t(3));
        QCOMPARE(rows[0].id, std::string("word"));
        QCOMPARE(rows[0].copy, std::string("hello"));
        QCOMPARE(rows[1].title, std::string("你好"));
        QCOMPARE(rows[2].copy, std::string("Hello, world!"));
        QVERIFY(!tr::dictionaryRows("unknown", "{\"definitions\":[]}", false)[0].valid);
        QVERIFY(!tr::dictionaryRows("unknown", "not json", false)[0].valid);
        QVERIFY(!tr::dictionaryRows("unknown", "{\"definitions\":[{}]}", false)[0].valid);
    }

    void httpCancellationDoesNotWaitForOldRequests()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        std::atomic_bool canceled = false;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket* socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                socket->readAll();
                canceled = true;
            });
        });
        const std::string url = "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/translate";
        QElapsedTimer elapsed;
        elapsed.start();
        auto result = std::async(std::launch::async, [&] { return tr::post(url, "{}", {}, 9000, &canceled); });
        QVERIFY(QTest::qWaitFor([&] { return result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 1500));
        QCOMPARE(result.get().error, DWORD(ERROR_CANCELLED));
        QVERIFY(elapsed.elapsed() < 1000);
    }

    void httpTimeoutAndRecovery()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        int mode = 0;
        QByteArray received;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, input, sent = false]() mutable {
                    input->append(socket->readAll());
                    const qsizetype end = input->indexOf("\r\n\r\n");
                    if (sent || end < 0 || input->size() < end + 6)
                        return;
                    sent = true;
                    received = *input;
                    if (mode == 0) {
                        // No response headers: cancellation must not wait for the server.
                        QTimer::singleShot(900, socket, [socket] { socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"); });
                    } else if (mode == 1) {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nabc");
                        QTimer::singleShot(900, socket, [socket] { socket->write("defgh"); });
                    } else
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}");
                });
            }
        });
        const std::string url = "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/translate?key=kept";
        for (int pass = 0; pass < 9; ++pass) {
            mode = pass % 3;
            QElapsedTimer elapsed;
            elapsed.start();
            auto result = std::async(std::launch::async, [&] { return tr::post(url, "{}", {}, mode == 2 ? 2000 : 120,
                nullptr, {{"X-Test-Region", "eastasia"}}); });
            QVERIFY(QTest::qWaitFor([&] { return result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 3000));
            const tr::Response response = result.get();
            QCOMPARE(response.error, mode == 2 ? DWORD(0) : DWORD(ERROR_WINHTTP_TIMEOUT));
            QVERIFY2(elapsed.elapsed() < 1200, qPrintable(QString::number(elapsed.elapsed())));
            if (mode == 2) {
                QCOMPARE(response.status, DWORD(200));
                QCOMPARE(response.body, std::string("{}"));
            }
            QVERIFY(received.startsWith("POST /translate?key=kept HTTP/1.1\r\n"));
            QVERIFY(received.contains("X-Test-Region: eastasia\r\n"));
        }
    }

    void httpBodyLimit()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket* socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, sent = false]() mutable {
                socket->readAll();
                if (std::exchange(sent, true))
                    return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 9437184\r\n\r\n");
                socket->write(QByteArray(9 << 20, 'x'));
            });
        });
        const std::string url = "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/translate";
        auto result = std::async(std::launch::async, [&] { return tr::post(url, "{}", {}, 3000); });
        QVERIFY(QTest::qWaitFor([&] { return result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 5000));
        const tr::Response response = result.get();
        QCOMPARE(response.error, DWORD(ERROR_FILE_TOO_LARGE));
        QVERIFY(response.body.size() <= (8u << 20));
    }

    void targetFollowsText_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QByteArray>("setting");
        QTest::addColumn<QByteArray>("target");
        const auto row = [](const char* name, const char* text, const char* setting, const char* target) {
            QTest::newRow(name) << QByteArray(text) << QByteArray(setting) << QByteArray(target);
        };
        row("latin", "hello world", "auto", "zh");
        row("chinese", "你好", "auto", "en");
        row("mixed", "buy 苹果 now", "auto", "en");
        row("kana", "こんにちは", "auto", "en");
        row("katakana", "コーヒー", "auto", "en");
        row("hangul", "안녕하세요", "auto", "en");
        row("extension A", "\xE3\x90\x80", "auto", "en");
        row("accents are latin", "café déjà vu", "auto", "zh");
        row("symbols", "→ 1 + 1 = 2 …", "auto", "zh");
        row("empty setting", "hello", "", "zh");
        row("fixed en", "你好", "en", "en");
        row("fixed ja upper", "hello", "JA", "ja");
        row("fixed zh", "你好", "zh", "zh");
    }
    void targetFollowsText()
    {
        QFETCH(QByteArray, text);
        QFETCH(QByteArray, setting);
        QFETCH(QByteArray, target);
        QCOMPARE(QByteArray::fromStdString(tr::targetOf(text.toStdString(), setting.toStdString())), target);
    }

    void settingsAreRead()
    {
        const tr::Settings s = tr::settingsFrom(
            parse("{\"provider\":\" DeepL \",\"apiKey\":\"  abc:fx \",\"endpoint\":\" https://x/v1/ \",\"model\":\"\",\"target\":\"EN\"}"));
        QCOMPARE(QString::fromStdString(s.provider), QString::fromUtf8("deepl"));
        QCOMPARE(QString::fromStdString(s.key), QString::fromUtf8("abc:fx"));
        QCOMPARE(QString::fromStdString(s.endpoint), QString::fromUtf8("https://x/v1/"));
        QVERIFY(s.model.empty());
        QCOMPARE(QString::fromStdString(s.target), QString::fromUtf8("en"));

        const tr::Settings none = tr::settingsFrom(parse("{}"));
        QCOMPARE(QString::fromStdString(none.provider), QString::fromUtf8("none"));
        QCOMPARE(QString::fromStdString(none.target), QString::fromUtf8("auto"));
        QVERIFY(none.key.empty());
    }

    // #10: each service its own key; switching back and forth keeps them apart,
    // and the shared apiKey of 2.1.1 stays in use where a service has none.
    void eachServiceHasItsOwnKey()
    {
        const std::string keys = "\"deeplKey\":\"d:fx\",\"openaiKey\":\" o \",\"apiKey\":\"shared\"";
        const auto keyFor = [&](const char* provider) -> QString {
            return QString::fromStdString(
                tr::settingsFrom(parse("{\"provider\":\"" + std::string(provider) + "\"," + keys + "}")).key);
        };
        QCOMPARE(keyFor("deepl"), QString::fromUtf8("d:fx"));
        QCOMPARE(keyFor("openai"), QString::fromUtf8("o"));
        QCOMPARE(keyFor("deepl"), QString::fromUtf8("d:fx"));
        QCOMPARE(keyFor("deepseek"), QString::fromUtf8("shared")); // none of its own yet
        QCOMPARE(keyFor("something-else"), QString::fromUtf8("o")); // spoken to as OpenAI

        for (const char* p : {"deepl", "microsoft", "openai", "deepseek", "doubao", "hunyuan"})
            QCOMPARE(QString::fromStdString(tr::keyField(p)),
                QString::fromUtf8(std::string(p) == "openai" ? "openaiKey" : (std::string(p) + "Key").c_str()));

        // The manifest declares every one of them, or the host never hands it over.
        QFile file(QStringLiteral(WINSHUN_TRANSLATE_MANIFEST));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QStringList declared;
        for (const QJsonValue& v : QJsonDocument::fromJson(file.readAll()).object().value(u"settings").toArray())
            if (v.toObject().value(u"type").toString() == u"password")
                declared << v.toObject().value(u"key").toString();
        for (const char* p : {"deepl", "microsoft", "openai", "deepseek", "doubao", "hunyuan"})
            QVERIFY2(declared.contains(QString::fromStdString(tr::keyField(p))), p);
        QVERIFY(declared.contains(QStringLiteral("apiKey")));
    }

    void deeplHostFollowsTheKey()
    {
        QCOMPARE(QString::fromStdString(tr::buildRequest(deepl("abc:fx"), "hi", "zh").url),
            QString::fromUtf8("https://api-free.deepl.com/v2/translate"));
        QCOMPARE(QString::fromStdString(tr::buildRequest(deepl("abc-def"), "hi", "zh").url),
            QString::fromUtf8("https://api.deepl.com/v2/translate"));
        QCOMPARE(QString::fromStdString(tr::buildRequest(deepl("fx"), "hi", "zh").url), QString::fromUtf8("https://api.deepl.com/v2/translate"));
        QCOMPARE(QString::fromStdString(tr::buildRequest(deepl("abc:fx"), "hi", "zh").authorization), QString::fromUtf8("DeepL-Auth-Key abc:fx"));
    }

    void deeplBody()
    {
        const Json body = parse(tr::buildRequest(deepl("k"), "say \"hi\"\nnow 你好", "ja").body);
        QCOMPARE(body["text"].toArray().size(), std::size_t(2)); // a line a text: translated each on its own
        QCOMPARE(QString::fromStdString(body["text"].toArray()[0].toString()), QString::fromUtf8("say \"hi\""));
        QCOMPARE(QString::fromStdString(body["text"].toArray()[1].toString()), QString::fromUtf8("now 你好"));
        QCOMPARE(parse(tr::buildRequest(deepl("k"), " one line \n\n", "ja").body)["text"].toArray().size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(body["target_lang"].toString()), QString::fromUtf8("JA"));
        QCOMPARE(QString::fromStdString(parse(tr::buildRequest(deepl("k"), "x", "zh").body)["target_lang"].toString()), QString::fromUtf8("ZH"));
        QCOMPARE(QString::fromStdString(parse(tr::buildRequest(deepl("k"), "x", "en").body)["target_lang"].toString()), QString::fromUtf8("EN"));
        QCOMPARE(QString::fromStdString(parse(tr::buildRequest(deepl("k"), "x", "pt-br").body)["target_lang"].toString()),
            QString::fromUtf8("PT-BR"));
    }

    void openaiEndpoint()
    {
        tr::Settings s = openai();
        QCOMPARE(QString::fromStdString(tr::buildRequest(s, "x", "en").url), QString::fromUtf8("https://api.openai.com/v1/chat/completions"));
        s.endpoint = "https://api.siliconflow.cn/v1";
        QCOMPARE(QString::fromStdString(tr::buildRequest(s, "x", "en").url), QString::fromUtf8("https://api.siliconflow.cn/v1/chat/completions"));
        s.endpoint = "http://127.0.0.1:8080/v1///";
        QCOMPARE(QString::fromStdString(tr::buildRequest(s, "x", "en").url), QString::fromUtf8("http://127.0.0.1:8080/v1/chat/completions"));
        QCOMPARE(QString::fromStdString(tr::buildRequest(s, "x", "en").authorization), QString::fromUtf8("Bearer sk-test"));
    }

    void openaiBody()
    {
        tr::Settings s = openai();
        Json body = parse(tr::buildRequest(s, "He said \"go\"\\n\tok 你好 😀", "en").body);
        QCOMPARE(QString::fromStdString(body["model"].toString()), QString::fromUtf8("gpt-4o-mini"));
        QCOMPARE(body["temperature"].toNumber(-1), 0.0);
        const Json::Array& messages = body["messages"].toArray();
        QCOMPARE(messages.size(), std::size_t(2));
        QCOMPARE(QString::fromStdString(messages[0]["role"].toString()), QString::fromUtf8("system"));
        QCOMPARE(QString::fromStdString(messages[0]["content"].toString()),
            QString::fromUtf8("Translate the following text into English. Reply with the translation only, no explanations."));
        QCOMPARE(QString::fromStdString(messages[1]["role"].toString()), QString::fromUtf8("user"));
        QCOMPARE(QString::fromStdString(messages[1]["content"].toString()), QString::fromUtf8("He said \"go\"\\n\tok 你好 😀"));

        s.model = "deepseek-chat";
        body = parse(tr::buildRequest(s, "x", "zh").body);
        QCOMPARE(QString::fromStdString(body["model"].toString()), QString::fromUtf8("deepseek-chat"));
        QVERIFY(QString::fromStdString(body["messages"].toArray()[0]["content"].toString()).contains(QString::fromUtf8("into Chinese.")));
        body = parse(tr::buildRequest(s, "x", "ja").body);
        QVERIFY(QString::fromStdString(body["messages"].toArray()[0]["content"].toString()).contains(QString::fromUtf8("into Japanese.")));
        body = parse(tr::buildRequest(s, "x", "fr").body);
        QVERIFY(QString::fromStdString(body["messages"].toArray()[0]["content"].toString()).contains(QString::fromUtf8("into fr.")));
    }

    void answersAreRead()
    {
        const tr::Answer d = tr::parseAnswer(deepl("k"),
            "{\"translations\":[{\"detected_source_language\":\"EN\",\"text\":\"你好\"}]}", true);
        QVERIFY(d.ok);
        QCOMPARE(QString::fromStdString(d.text), QString::fromUtf8("你好"));
        QCOMPARE(QString::fromStdString(d.detected), QString::fromUtf8("EN"));

        const tr::Answer o = tr::parseAnswer(openai(),
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"  Hello\\n\"}}]}", true);
        QVERIFY(o.ok);
        QCOMPARE(QString::fromStdString(o.text), QString::fromUtf8("Hello"));
        QVERIFY(o.detected.empty());
    }

    // Several lines (a screenshot's blocks): each translated on its own, the answer a line each.
    void severalLinesEachOnItsOwn()
    {
        QCOMPARE(tr::linesOf("  a \n\n\tb\r\n \n").size(), std::size_t(2));
        QCOMPARE(QString::fromStdString(tr::linesOf("  a \n\n\tb\r\n")[1]), QString::fromUtf8("b"));
        QVERIFY(tr::linesOf(" \n ").empty());

        // A chat model: a JSON array out, one of the same length asked for.
        const Json body = parse(tr::buildRequest(openai(), "Save \"all\"\nCancel\n\n就绪", "zh").body);
        const QString system = QString::fromStdString(body["messages"].toArray()[0]["content"].toString());
        QVERIFY2(system.contains(QString::fromUtf8("JSON array of 3 strings")) && system.contains(QString::fromUtf8("into Chinese"))
                && system.contains(QString::fromUtf8("exactly 3 strings")), qPrintable(system));
        const Json sent = parse(body["messages"].toArray()[1]["content"].toString());
        QCOMPARE(sent.toArray().size(), std::size_t(3));
        QCOMPARE(QString::fromStdString(sent.toArray()[0].toString()), QString::fromUtf8("Save \"all\""));
        QCOMPARE(QString::fromStdString(sent.toArray()[2].toString()), QString::fromUtf8("就绪"));
        const auto chat = [](const std::string& content) {
            return Json(Json::Object {{"choices", Json::Array {Json::Object {{"message", Json::Object {{"content", content}}}}}}}).dump();
        };
        tr::Answer a = tr::parseAnswer(openai(), chat("[\"保存“全部”\", \"取消\", \"Ready\"]"), true);
        QVERIFY(a.ok);
        QCOMPARE(QString::fromStdString(a.text), QString::fromUtf8("保存“全部”\n取消\nReady"));
        // In a code fence; a line break inside one translation does not make two.
        a = tr::parseAnswer(openai(), chat("```json\n[\"第一\\n行\", \"第二\"]\n```"), true);
        QCOMPARE(QString::fromStdString(a.text), QString::fromUtf8("第一 行\n第二"));
        // Not such an array: the text as it came (the caller counts the lines).
        a = tr::parseAnswer(openai(), chat("[1] 保存\n[2] 取消"), true);
        QCOMPARE(QString::fromStdString(a.text), QString::fromUtf8("[1] 保存\n[2] 取消"));
        a = tr::parseAnswer(openai(), chat("[\"a\", 2]"), true);
        QCOMPARE(QString::fromStdString(a.text), QString::fromUtf8("[\"a\", 2]"));

        // Microsoft and DeepL: a text each, a translation each.
        tr::Settings microsoft = openai();
        microsoft.provider = "microsoft";
        const Json ms = parse(tr::buildRequest(microsoft, "Save\nCancel", "zh").body);
        QCOMPARE(ms.toArray().size(), std::size_t(2));
        QCOMPARE(QString::fromStdString(ms.toArray()[1]["Text"].toString()), QString::fromUtf8("Cancel"));
        a = tr::parseAnswer(microsoft,
            R"([{"detectedLanguage":{"language":"en"},"translations":[{"text":"保存"}]},{"translations":[{"text":"取\n消"}]}])", true);
        QVERIFY(a.ok);
        QCOMPARE(QString::fromStdString(a.text), QString::fromUtf8("保存\n取 消"));
        QCOMPARE(QString::fromStdString(a.detected), QString::fromUtf8("en"));
        a = tr::parseAnswer(deepl("k"), R"({"translations":[{"detected_source_language":"EN","text":"保存"},{"text":"取消"}]})", true);
        QVERIFY(a.ok);
        QCOMPARE(QString::fromStdString(a.text), QString::fromUtf8("保存\n取消"));
        // One text keeps its own line breaks, as before.
        a = tr::parseAnswer(deepl("k"), R"({"translations":[{"text":"第一行\n第二行"}]})", true);
        QCOMPARE(QString::fromStdString(a.text), QString::fromUtf8("第一行\n第二行"));
    }

    void answersWithMissingParts()
    {
        // No translations / choices at all.
        QVERIFY(!tr::parseAnswer(deepl("k"), "{\"translations\":[]}", true).ok);
        QVERIFY(!tr::parseAnswer(deepl("k"), "{}", false).ok);
        QVERIFY(!tr::parseAnswer(openai(), "{\"choices\":[]}", true).ok);
        QVERIFY(!tr::parseAnswer(openai(), "{\"data\":1}", false).error.empty());
        // A row without text is an answer, an empty one.
        tr::Answer a = tr::parseAnswer(openai(), "{\"choices\":[{\"message\":{\"content\":null}}]}", true);
        QVERIFY(a.ok);
        QVERIFY(a.text.empty());
        a = tr::parseAnswer(openai(), "{\"choices\":[{}]}", true);
        QVERIFY(a.ok && a.text.empty());
        a = tr::parseAnswer(deepl("k"), "{\"translations\":[{\"detected_source_language\":\"DE\"}]}", true);
        QVERIFY(a.ok && a.text.empty());
        QCOMPARE(QString::fromStdString(a.detected), QString::fromUtf8("DE"));
    }

    void badJson()
    {
        for (const char* body : {"", "<html>502 Bad Gateway</html>", "{\"choices\":", "[1,2]", "null"}) {
            const tr::Answer a = tr::parseAnswer(openai(), body, false);
            QVERIFY2(!a.ok, body);
            QVERIFY2(!a.error.empty(), body);
        }
        QVERIFY(tr::parseAnswer(openai(), "oops", true).error != tr::parseAnswer(openai(), "oops", false).error);
    }

    void serviceMessages()
    {
        QCOMPARE(QString::fromStdString(tr::serviceMessage("{\"error\":\"bad key\"}")), QString::fromUtf8("bad key"));
        QCOMPARE(QString::fromStdString(tr::serviceMessage("{\"error\":{\"message\":\"Incorrect API key\",\"type\":\"x\"}}")),
            QString::fromUtf8("Incorrect API key"));
        QCOMPARE(QString::fromStdString(tr::serviceMessage("{\"message\":\"Wrong auth\"}")), QString::fromUtf8("Wrong auth"));
        // JSON without any of them, and no JSON at all: the body itself.
        QCOMPARE(QString::fromStdString(tr::serviceMessage("{\"code\":5}")), QString::fromUtf8("{\"code\":5}"));
        QCOMPARE(QString::fromStdString(tr::serviceMessage("Forbidden")), QString::fromUtf8("Forbidden"));
        // "error" an object without a message: fall through to the top-level message.
        QCOMPARE(QString::fromStdString(tr::serviceMessage("{\"error\":{\"code\":1},\"message\":\"m\"}")), QString::fromUtf8("m"));
    }

    void cutsOnCharacters()
    {
        QCOMPARE(QString::fromStdString(tr::cut("abcdef", 3)), QString::fromUtf8("abc"));
        QCOMPARE(QString::fromStdString(tr::cut("abc", 3)), QString::fromUtf8("abc"));
        QCOMPARE(QString::fromStdString(tr::cut("", 3)), QString());
        // 3-byte and 4-byte characters are not split.
        const std::string han = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96"; // 你好世
        QCOMPARE(QString::fromStdString(tr::cut(han, 2)), QString::fromUtf8("你好"));
        const std::string emoji = "a\xF0\x9F\x98\x80" "b\xF0\x9F\x98\x80" "c"; // a😀b😀c
        QCOMPARE(QString::fromStdString(tr::cut(emoji, 3)), QString::fromUtf8("a😀b"));
        QCOMPARE(QString::fromStdString(tr::cut(emoji, 2)), QString::fromUtf8("a😀"));

        // The default is 200 code points; a byte cut at 200 would split a character.
        std::string long200;
        for (int i = 0; i < 300; ++i)
            long200 += "\xE4\xBD\xA0";
        const std::string cutText = tr::cut(long200);
        QCOMPARE(cutText.size(), std::size_t(600));
        QVERIFY(validUtf8(cutText));
        std::string odd = "x";
        for (int i = 0; i < 300; ++i)
            odd += "\xE4\xBD\xA0";
        const std::string oddCut = tr::cut(odd);
        QCOMPARE(oddCut.size(), std::size_t(1 + 199 * 3));
        QVERIFY(validUtf8(oddCut));
    }

    void statusRowCutsTheServicesWords()
    {
        std::string said(500, 'x');
        const winshun::Item a = tr::statusRow(404, "{\"error\":{\"message\":\"" + said + "\"}}", false);
        QCOMPARE(QString::fromStdString(a.title), QString::fromUtf8("The service answered 404"));
        QCOMPARE(a.subtitle.size(), std::size_t(200));
        QVERIFY(!a.valid);
        std::string han;
        for (int i = 0; i < 250; ++i)
            han += "\xE6\xB5\x8B";
        const winshun::Item b = tr::statusRow(500, han, true);
        QCOMPARE(QString::fromStdString(b.title), QString::fromUtf8("翻译服务暂时出错（接口返回 500），稍后再试"));
        QVERIFY(validUtf8(b.subtitle));
        QCOMPARE(b.subtitle.size(), std::size_t(600));
        // The body of a failing answer that is not JSON.
        QCOMPARE(QString::fromStdString(tr::statusRow(502, "Bad Gateway", true).subtitle), QString::fromUtf8("Bad Gateway"));
    }

    // GitHub #444: a key the service refused is a setting to fix, not a
    // failure: a "setup" row (translate_text tries the next translate plugin
    // and the screenshot's Translate offers the settings), Enter opens them.
    void statusRowSaysWhatToDo()
    {
        const std::string body = "{\"error\":{\"message\":\"Incorrect API key provided: sk-u***. You can find your API key at https://console.cloud.tencent.com/hunyuan/start.\"}}";
        for (const long status : {401L, 403L}) {
            const winshun::Item key = tr::statusRow(status, body, true);
            QCOMPARE(QString::fromStdString(key.id), QString::fromUtf8("setup"));
            QCOMPARE(QString::fromStdString(key.title), QString::fromUtf8("翻译服务的密钥不对或没有权限（接口返回 %1）").arg(status));
            QVERIFY(QString::fromStdString(key.subtitle).startsWith(QString::fromUtf8("Enter 打开设置 → 插件 → 翻译，重新填密钥 · Incorrect API key provided")));
            QCOMPARE(key.open["url"].toString(), std::string("winshun:settings"));
            QVERIFY(key.valid);
        }
        const winshun::Item english = tr::statusRow(401, "", false);
        QCOMPARE(QString::fromStdString(english.title), QString::fromUtf8("The translation service refused the key (401)"));
        QCOMPARE(QString::fromStdString(english.subtitle), QString::fromUtf8("Enter opens Settings → Plugins → Translate to enter the key again"));

        QCOMPARE(QString::fromStdString(tr::statusRow(402, "", true).title), QString::fromUtf8("翻译服务的余额不足（接口返回 402）"));
        QCOMPARE(QString::fromStdString(tr::statusRow(429, "", true).title), QString::fromUtf8("请求太多或额度用完了（接口返回 429），稍后再试"));
        QCOMPARE(QString::fromStdString(tr::statusRow(503, "", false).title), QString::fromUtf8("The translation service failed (503): try again later"));
        for (const long status : {402L, 429L, 503L}) {
            QCOMPARE(QString::fromStdString(tr::statusRow(status, "", true).id), QString::fromUtf8("error"));
            QVERIFY(!tr::statusRow(status, "", true).valid);
        }
    }

    void subtitlesByLocale()
    {
        winshun::Items zh = tr::resultRows("hello", "你好", "zh", "EN", true);
        QCOMPARE(zh.size(), std::size_t(2));
        QCOMPARE(QString::fromStdString(zh[0].id), QString::fromUtf8("t"));
        QCOMPARE(QString::fromStdString(zh[0].title), QString::fromUtf8("你好"));
        QCOMPARE(QString::fromStdString(zh[0].subtitle), QString::fromUtf8("→ 中文 · EN · Enter 复制"));
        QCOMPARE(zh[0].score, 90);
        QCOMPARE(QString::fromStdString(zh[0].copy), QString::fromUtf8("你好"));
        QCOMPARE(QString::fromStdString(zh[1].id), QString::fromUtf8("source"));
        QCOMPARE(QString::fromStdString(zh[1].title), QString::fromUtf8("hello"));
        QCOMPARE(QString::fromStdString(zh[1].subtitle), QString::fromUtf8("原文"));
        QCOMPARE(QString::fromStdString(zh[1].copy), QString::fromUtf8("hello"));

        winshun::Items en = tr::resultRows("你好", "Hello", "en", "", false);
        QCOMPARE(QString::fromStdString(en[0].subtitle), QString::fromUtf8("→ English · Enter copies"));
        QCOMPARE(QString::fromStdString(en[1].subtitle), QString::fromUtf8("source"));
        QCOMPARE(QString::fromStdString(tr::resultRows("a", "b", "ja", "", true)[0].subtitle), QString::fromUtf8("→ 日文 · Enter 复制"));
        QCOMPARE(QString::fromStdString(tr::resultRows("a", "b", "ja", "", false)[0].subtitle), QString::fromUtf8("→ Japanese · Enter copies"));
        QCOMPARE(QString::fromStdString(tr::resultRows("a", "b", "en", "ZH", true)[0].subtitle), QString::fromUtf8("→ 英文 · ZH · Enter 复制"));
        QCOMPARE(QString::fromStdString(tr::resultRows("a", "b", "fr", "", false)[0].subtitle), QString::fromUtf8("→ fr · Enter copies"));
    }

    void otherRows()
    {
        const winshun::Item setup = tr::setupRow(true);
        QCOMPARE(QString::fromStdString(setup.id), QString::fromUtf8("setup"));
        QCOMPARE(setup.open["url"].toString(), std::string("winshun:settings"));
        QVERIFY(setup.valid);
        QCOMPARE(QString::fromStdString(tr::setupRow(false).title), QString::fromUtf8("No translation service set up"));
        QCOMPARE(QString::fromStdString(tr::hintRow(true).title), QString::fromUtf8("输入要翻译的句子"));
        QCOMPARE(QString::fromStdString(tr::hintRow(false).title), QString::fromUtf8("Type a sentence to translate"));
        QVERIFY(!tr::hintRow(true).valid);
        QCOMPARE(QString::fromStdString(tr::workingRow("hi", true).title), QString::fromUtf8("翻译中…"));
        QCOMPARE(QString::fromStdString(tr::workingRow("hi", false).title), QString::fromUtf8("Translating…"));
        QCOMPARE(QString::fromStdString(tr::workingRow("hi", false).subtitle), QString::fromUtf8("hi"));
        QVERIFY(!tr::workingRow("hi", true).valid);
        QCOMPARE(QString::fromStdString(tr::failedRow("why", true).title), QString::fromUtf8("翻译失败"));
        QCOMPARE(QString::fromStdString(tr::failedRow("why", false).title), QString::fromUtf8("Translation failed"));
        QCOMPARE(QString::fromStdString(tr::emptyRow("hi", true).title), QString::fromUtf8("没有译文"));
        QCOMPARE(QString::fromStdString(tr::emptyRow("hi", false).title), QString::fromUtf8("No translation came back"));
    }
};

QTEST_GUILESS_MAIN(TestTranslate)
#include "tst_translate.moc"
