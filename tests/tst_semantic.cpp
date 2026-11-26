// The winshun.semantic plugin's pieces that need no Win顺 and no model
// (src/semantic-plugin): passages, the vector store, the settings of the
// model services, and how the 本地模型 plugin is asked which model it runs
// (a local server plays it).

#include "Chunker.h"
#include "Embedder.h"
#include "Store.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <cmath>
#include <future>
#include <random>

using namespace Qt::StringLiterals;
using namespace semantic;

namespace {

std::vector<float> unit(std::size_t dims, unsigned seed)
{
    std::mt19937 random(seed);
    std::normal_distribution<float> normal;
    std::vector<float> v(dims);
    double sum = 0;
    for (float& x : v) {
        x = normal(random);
        sum += static_cast<double>(x) * x;
    }
    for (float& x : v)
        x = static_cast<float>(x / std::sqrt(sum));
    return v;
}

// `base` turned a little: close to it, not the same.
std::vector<float> nudged(const std::vector<float>& base, unsigned seed, float amount)
{
    std::vector<float> noise = unit(base.size(), seed);
    double sum = 0;
    for (std::size_t i = 0; i < base.size(); ++i) {
        noise[i] = base[i] + amount * noise[i];
        sum += static_cast<double>(noise[i]) * noise[i];
    }
    for (float& x : noise)
        x = static_cast<float>(x / std::sqrt(sum));
    return noise;
}

NewFile file(const std::string& path, std::uint32_t stamp, std::vector<std::vector<float>> vectors)
{
    NewFile f;
    f.path = path;
    f.stamp = stamp;
    int n = 0;
    for (auto& v : vectors)
        f.passages.push_back({"[page " + std::to_string(++n) + "]", "passage " + std::to_string(n) + " of " + path, std::move(v)});
    return f;
}

} // namespace

class TestSemantic : public QObject {
    Q_OBJECT

private slots:
    void passagesFollowLinesAndMarks()
    {
        std::string text = "[page 1]\n";
        for (int i = 0; i < 30; ++i)
            text += "这是第一页的一行文字，二十个字左右。\n"; // 18 characters + newline each
        text += "\n[page 2]\nshort\n";
        const std::vector<Passage> parts = passages(text);
        QVERIFY(parts.size() >= 2);
        QCOMPARE(parts.front().where, std::string("[page 1]"));
        for (const Passage& p : parts) {
            QVERIFY(codePoints(p.text) <= kMost);
            QVERIFY(p.text.find("[page") == std::string::npos); // marks are not text
        }
        QCOMPARE(parts.back().where, std::string("[page 2]"));
        QCOMPARE(parts.back().text, std::string("short"));
        // Whole lines: none is cut in the middle.
        for (const Passage& p : parts) {
            if (p.where == "[page 1]")
                QVERIFY(p.text.ends_with("。"));
        }
    }

    void passagesCutLongLinesAndStop()
    {
        const std::string line(5000, 'x'); // one line of 5000 characters
        const std::vector<Passage> parts = passages(line + "\n");
        QCOMPARE(parts.size(), std::size_t {5});
        for (const Passage& p : parts)
            QCOMPARE(codePoints(p.text), kMost);
        // Never half a character, however long the line.
        std::string chinese;
        for (int i = 0; i < 2500; ++i)
            chinese += "字";
        for (const Passage& p : passages(chinese))
            QCOMPARE(p.text.size() % 3, std::size_t {0});
        // A huge file gives at most kMostPassages.
        std::string many;
        for (int i = 0; i < 2000; ++i)
            many += std::string(700, 'a') + "\n";
        QCOMPARE(passages(many).size(), kMostPassages);
        QVERIFY(passages("").empty());
        QVERIFY(passages("\n\n  \n").empty());
    }

    void snippetFoldsSpace()
    {
        QCOMPARE(snippet("a  b\n\nc", 10), std::string("a b c"));
        QCOMPARE(snippet("一二三四五", 3), std::string("一二三…"));
        QCOMPARE(snippet("  lead", 10), std::string("lead"));
    }

    void storeFindsReplacesRemoves()
    {
        QTemporaryDir dir;
        const std::wstring folder = dir.path().toStdWString() + L"\\semantic";
        const auto a = unit(64, 1);
        const auto b = unit(64, 2);
        const auto c = unit(64, 3);
        {
            Store store(folder);
            QVERIFY(store.open("model-1"));
            store.add(file("C:\\docs\\a.docx", 1, {a, unit(64, 10)}));
            store.add(file("C:\\docs\\b.pdf", 1, {b}));
            store.add(file("C:\\docs\\empty.pdf", 7, {})); // no text: kept, not searched
            QVERIFY(store.flush());
            QCOMPARE(store.files(), std::size_t {3});
            QCOMPARE(store.passages(), std::size_t {3});

            std::vector<Hit> hits = store.search(nudged(a, 5, 0.3f), 10);
            QCOMPARE(hits.size(), std::size_t {2});
            QCOMPARE(hits.front().path, std::string("C:\\docs\\a.docx"));
            QCOMPARE(hits.front().where, std::string("[page 1]"));
            QVERIFY(hits.front().score > 0.8f && hits.front().score > hits.back().score);
            QCOMPARE(store.search(nudged(b, 6, 0.3f), 1).front().path, std::string("C:\\docs\\b.pdf"));

            // The file changed: its new passages replace the old ones (a later segment).
            store.add(file("C:\\docs\\a.docx", 2, {c}));
            store.remove("C:\\docs\\b.pdf");
            QVERIFY(store.flush());
            QCOMPARE(store.files(), std::size_t {2});
            QCOMPARE(store.stamps().at("C:\\docs\\a.docx"), 2u);
            hits = store.search(a, 10);
            QCOMPARE(hits.size(), std::size_t {1});
            QCOMPARE(hits.front().path, std::string("C:\\docs\\a.docx"));
            QVERIFY(hits.front().score < 0.5f); // a's old passages are gone
            QVERIFY(store.search(c, 1).front().score > 0.98f); // int8: close to exact
        }
        // Opened again: the same.
        {
            Store store(folder);
            QVERIFY(store.open("model-1"));
            QCOMPARE(store.files(), std::size_t {2});
            QCOMPARE(store.stamps().at("C:\\docs\\a.docx"), 2u);
            QVERIFY(!store.stamps().contains("C:\\docs\\b.pdf"));
            QCOMPARE(store.search(c, 1).front().path, std::string("C:\\docs\\a.docx"));
        }
        // Another model: nothing of the old one is kept.
        {
            Store store(folder);
            QVERIFY(store.open("model-2"));
            QCOMPARE(store.files(), std::size_t {0});
            QVERIFY(store.search(c, 5).empty());
        }
    }

    void storeCompacts()
    {
        QTemporaryDir dir;
        const std::wstring folder = dir.path().toStdWString() + L"\\semantic";
        Store store(folder);
        QVERIFY(store.open("m"));
        // Many small segments of the same files, replaced again and again.
        for (unsigned round = 0; round < 30; ++round) {
            for (unsigned f = 0; f < 50; ++f)
                store.add(file("C:\\f" + std::to_string(f) + ".docx", round, {unit(32, round * 100 + f)}));
            QVERIFY(store.flush());
        }
        QCOMPARE(store.files(), std::size_t {50});
        QVERIFY(store.compactIfWorth());
        int segments = 0;
        WIN32_FIND_DATAW found;
        const HANDLE find = ::FindFirstFileW((folder + L"\\seg-*").c_str(), &found);
        for (BOOL more = find != INVALID_HANDLE_VALUE; more; more = ::FindNextFileW(find, &found))
            ++segments;
        ::FindClose(find);
        QCOMPARE(segments, 1);
        QCOMPARE(store.files(), std::size_t {50});
        QCOMPARE(store.passages(), std::size_t {50});
        QCOMPARE(store.stamps().at("C:\\f7.docx"), 29u);
        QCOMPARE(store.search(unit(32, 29 * 100 + 7), 1).front().path, std::string("C:\\f7.docx"));
    }

    void providers()
    {
        using winshun::Json;
        Provider p = providerFrom(Json::Object {{"provider", "ollama"}}, true);
        QVERIFY(p.configured()); // a local one needs no key
        QVERIFY(p.local);
        QCOMPARE(p.model, std::string("qwen3-embedding:0.6b"));
        QVERIFY(p.queryPrefix.starts_with("Instruct:"));
        QVERIFY(p.docPrefix.empty());

        p = providerFrom(Json::Object {{"provider", "bailian"}}, true);
        QVERIFY(!p.configured()); // no key yet
        p = providerFrom(Json::Object {{"provider", "bailian"}, {"apiKey", " sk-1 "}}, true);
        QVERIFY(p.configured());
        QCOMPARE(p.key, std::string("sk-1"));
        QCOMPARE(p.batch, 10); // its limit

        // Another endpoint, written with /embeddings or a trailing slash.
        p = providerFrom(Json::Object {{"provider", "custom"}, {"endpoint", "http://localhost:8080/v1/embeddings"}, {"model", "intfloat/multilingual-e5-small"}}, true);
        QCOMPARE(p.endpoint, std::string("http://localhost:8080/v1"));
        QVERIFY(p.local && p.configured());
        QCOMPARE(p.queryPrefix, std::string("query: "));
        QCOMPARE(p.docPrefix, std::string("passage: "));
        p = providerFrom(Json::Object {{"provider", "custom"}, {"endpoint", "https://api.example.com/v1/"}, {"model", "m"}}, true);
        QCOMPARE(p.endpoint, std::string("https://api.example.com/v1"));
        QVERIFY(!p.local && !p.configured());

        // The signature changes with what the vectors depend on.
        const Provider q = providerFrom(Json::Object {{"provider", "ollama"}, {"model", "bge-m3"}}, true);
        QVERIFY(q.signature() != providerFrom(Json::Object {{"provider", "ollama"}}, true).signature());
        QCOMPARE(providerFrom(Json::Object {{"provider", "nonsense"}}, true).id, std::string("none"));
        QVERIFY(!providerFrom(Json::Object {}, true).configured());
    }

    // The 本地模型 plugin: no key, its own port, and the model it says it runs.
    void localProvider()
    {
        using winshun::Json;
        Provider p = providerFrom(Json::Object {{"provider", "local"}}, true);
        QVERIFY(p.local && p.modelFromService);
        QVERIFY(!p.configured()); // until the model is known
        QCOMPARE(p.endpoint, std::string("http://127.0.0.1:39281/v1"));
        setModel(p, "qwen3-embedding-0.6b");
        QVERIFY(p.configured());
        QVERIFY(p.queryPrefix.starts_with("Instruct:")); // its family's prefix
        Provider other = p;
        setModel(other, "bge-m3-q4_k_m");
        QVERIFY(other.queryPrefix.empty());
        QVERIFY(other.signature() != p.signature()); // another model: another index
        // A model named in the settings is taken as said.
        p = providerFrom(Json::Object {{"provider", "local"}, {"model", "my-model"}, {"endpoint", "http://127.0.0.1:40000/v1"}}, true);
        QVERIFY(!p.modelFromService && p.configured());
    }

    void localModelIsAsked()
    {
        using winshun::Json;
        // The plugin's relay, played: /v1/models as told, /v1/embeddings refused as it refuses.
        // (No raw strings: moc loses the class after one with a quote before its parenthesis.)
        const auto json = [](const Json& value) { return QByteArray::fromStdString(value.dump()); };
        const Json chat = Json::Object {{"id", "qwen3.5-2b"}, {"kind", "chat"}};
        const Json embedding = Json::Object {{"id", "bge-m3"}, {"kind", "embedding"}};
        QByteArray models = json(Json::Object {{"object", "list"}, {"data", Json::Array {chat, embedding}}});
        const QByteArray refusal = json(Json::Object {{"error",
            Json::Object {{"message", "还没下载嵌入模型"}, {"type", "winshun_local_model"}, {"code", "no_model"}}}});
        int probes = 0;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    const QByteArray request = socket->peek(1 << 16);
                    const qsizetype head = request.indexOf("\r\n\r\n");
                    if (head < 0)
                        return;
                    const bool post = request.startsWith("POST ");
                    if (post && !request.mid(head + 4).contains(']')) // its body is still to come
                        return;
                    socket->readAll();
                    probes += post;
                    const QByteArray body = post ? refusal : models;
                    socket->write((post ? "HTTP/1.1 503 Service Unavailable\r\n" : "HTTP/1.1 200 OK\r\n")
                        + QByteArray("Content-Type: application/json\r\nConnection: close\r\nContent-Length: ") + QByteArray::number(body.size())
                        + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        const Provider p = providerFrom(
            Json::Object {{"provider", "local"}, {"endpoint", "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/v1"}}, true);
        const auto ask = [&](std::string& model) {
            auto answer = std::async(std::launch::async, [&] { return localModel(p, true, model); });
            while (answer.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            return answer.get();
        };
        std::string model;
        QCOMPARE(ask(model), std::string());
        QCOMPARE(model, std::string("bge-m3")); // the one that embeds, not the one that chats
        QCOMPARE(probes, 0);

        // None yet: it is asked for one, and its own words come back.
        models = json(Json::Object {{"object", "list"}, {"data", Json::Array {chat}}});
        model.clear();
        QCOMPARE(ask(model), std::string("还没下载嵌入模型"));
        QVERIFY(model.empty());
        QCOMPARE(probes, 1);

        // Not running: said so, with where to turn it on.
        server.close();
        const std::string away = ask(model);
        QVERIFY2(away.find("没在运行") != std::string::npos, away.c_str());
    }
};

QTEST_GUILESS_MAIN(TestSemantic)
#include "tst_semantic.moc"
