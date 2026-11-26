// The pieces of the local models plugin (src/localmodel-plugin, wslocalmodel)
// without a model: what a .gguf file says about itself, the recommended
// models against the manifest, how llama-server is started and what its
// log says, the relay on 127.0.0.1 (passed on, refused, answered itself),
// the engine's answer when no model can be used, and a download that breaks
// off and goes on. Every server here is a small one of the test's own.

#include "Catalog.h"
#include "Download.h"
#include "Engine.h"
#include "LlamaServer.h"
#include "Relay.h"

#include "winshun.h"

#include <QTemporaryDir>
#include <QTest>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

using namespace Qt::StringLiterals;
using winshun::Json;

namespace {

// ---- a GGUF file of the test's own --------------------------------------------

class Gguf {
public:
    Gguf& string(const std::string& key, const std::string& value)
    {
        head(key, 8);
        text(value);
        ++m_keys;
        return *this;
    }
    Gguf& u32(const std::string& key, std::uint32_t value)
    {
        head(key, 4);
        raw(value);
        ++m_keys;
        return *this;
    }
    Gguf& f32(const std::string& key, float value)
    {
        head(key, 6);
        raw(value);
        ++m_keys;
        return *this;
    }
    Gguf& strings(const std::string& key, const std::vector<std::string>& values)
    {
        head(key, 9);
        raw(std::uint32_t(8));
        raw(std::uint64_t(values.size()));
        for (const std::string& v : values)
            text(v);
        ++m_keys;
        return *this;
    }
    std::string bytes() const
    {
        std::string out = "GGUF";
        const auto put = [&](auto v) { out.append(reinterpret_cast<const char*>(&v), sizeof v); };
        put(std::uint32_t(3));
        put(std::uint64_t(0)); // tensors
        put(std::uint64_t(m_keys));
        return out + m_body;
    }
    void write(const std::filesystem::path& file, std::size_t cut = 0) const
    {
        std::string b = bytes();
        if (cut)
            b.resize(b.size() - cut);
        std::ofstream(file, std::ios::binary).write(b.data(), static_cast<std::streamsize>(b.size()));
    }

private:
    template <class T>
    void raw(T v) { m_body.append(reinterpret_cast<const char*>(&v), sizeof v); }
    void text(const std::string& s)
    {
        raw(std::uint64_t(s.size()));
        m_body += s;
    }
    void head(const std::string& key, std::uint32_t type)
    {
        text(key);
        raw(type);
    }
    std::string m_body;
    std::uint64_t m_keys = 0;
};

// ---- sockets ----------------------------------------------------------------------

struct Sock {
    SOCKET s = INVALID_SOCKET;
    explicit Sock(SOCKET socket = INVALID_SOCKET) : s(socket) {}
    Sock(Sock&& o) noexcept : s(std::exchange(o.s, INVALID_SOCKET)) {}
    ~Sock()
    {
        if (s != INVALID_SOCKET)
            ::closesocket(s);
    }
};

int portOf(SOCKET s)
{
    sockaddr_in a {};
    int size = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &size);
    return ntohs(a.sin_port);
}

Sock listener()
{
    Sock l(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(l.s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(l.s, 8);
    return l;
}

bool sendText(SOCKET s, const std::string& text)
{
    return ::send(s, text.data(), static_cast<int>(text.size()), 0) == static_cast<int>(text.size());
}

// One request: its head, then as much body as Content-Length says.
std::string readRequest(SOCKET s)
{
    std::string in;
    char buffer[8192];
    std::size_t end = std::string::npos;
    while ((end = in.find("\r\n\r\n")) == std::string::npos) {
        const int n = ::recv(s, buffer, sizeof buffer, 0);
        if (n <= 0)
            return in;
        in.append(buffer, static_cast<std::size_t>(n));
    }
    std::size_t length = 0;
    std::string lower = in.substr(0, end);
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (const std::size_t at = lower.find("content-length:"); at != std::string::npos)
        length = std::stoul(lower.substr(at + 15));
    while (in.size() < end + 4 + length) {
        const int n = ::recv(s, buffer, sizeof buffer, 0);
        if (n <= 0)
            break;
        in.append(buffer, static_cast<std::size_t>(n));
    }
    return in;
}

std::string readAll(SOCKET s)
{
    std::string in;
    char buffer[8192];
    for (int n; (n = ::recv(s, buffer, sizeof buffer, 0)) > 0;)
        in.append(buffer, static_cast<std::size_t>(n));
    return in;
}

// Sends `request` to 127.0.0.1:port and reads until the other end closes.
std::string roundTrip(int port, const std::string& request)
{
    Sock c(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const DWORD timeout = 10000;
    ::setsockopt(c.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    if (::connect(c.s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || !sendText(c.s, request))
        return {};
    return readAll(c.s);
}

// A server answering each connection with `handler` on a thread of its own.
class Server {
public:
    using Handler = std::function<void(SOCKET client, const std::string& request)>;
    explicit Server(Handler handler) : m_listener(listener()), m_handler(std::move(handler))
    {
        m_port = portOf(m_listener.s);
        m_thread = std::thread([this] {
            for (;;) {
                Sock client(::accept(m_listener.s, nullptr, nullptr));
                if (client.s == INVALID_SOCKET || m_stop)
                    return;
                const std::string request = readRequest(client.s);
                {
                    const std::lock_guard lock(m_mutex);
                    m_requests.push_back(request);
                }
                m_handler(client.s, request);
                ::shutdown(client.s, SD_SEND);
            }
        });
    }
    ~Server()
    {
        m_stop = true;
        ::closesocket(std::exchange(m_listener.s, INVALID_SOCKET));
        m_thread.join();
    }
    int port() const { return m_port; }
    std::vector<std::string> requests()
    {
        const std::lock_guard lock(m_mutex);
        return m_requests;
    }

private:
    Sock m_listener;
    Handler m_handler;
    int m_port = 0;
    std::atomic_bool m_stop = false;
    std::thread m_thread;
    std::mutex m_mutex;
    std::vector<std::string> m_requests;
};

int statusOf(const std::string& response)
{
    return response.size() > 12 ? std::atoi(response.c_str() + 9) : -1;
}

std::string bodyOf(const std::string& response)
{
    const std::size_t at = response.find("\r\n\r\n");
    return at == std::string::npos ? std::string() : response.substr(at + 4);
}

std::string sha256Hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    unsigned char digest[32] {};
    ::BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), digest, 32);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    std::string hex;
    for (unsigned char b : digest) {
        static constexpr char kDigits[] = "0123456789abcdef";
        hex += kDigits[b >> 4];
        hex += kDigits[b & 15];
    }
    return hex;
}

std::filesystem::path pathOf(const QString& s) { return std::filesystem::path(s.toStdWString()); }

std::string readFile(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string pattern(std::size_t size)
{
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 2654435761u) >> 13);
    return data;
}

// A file served in ranges as ModelScope's CDN serves it (206 with
// Content-Range), with ways to misbehave.
struct Pieces {
    static constexpr std::uint64_t kPiece = 256 << 10;
    static constexpr int kCount = 6; // the file is five and a half pieces
    std::string data = pattern(kPiece * 5 + kPiece / 2);
    lm::DownloadTuning tuning {kPiece, 3};
    std::atomic_int served = 0;
    int breakRequest = 0; // that request (1-based) sends half its bytes and breaks off
    std::atomic_int stallAfter = 0; // the requests after this many wait 800 ms first
    bool ranges = true; // false: the whole file whatever is asked
    Server server {[this](SOCKET client, const std::string& request) { serve(client, request); }}; // last: the rest is ready first

    std::string url() const { return "http://127.0.0.1:" + std::to_string(server.port()) + "/m.gguf"; }

    void serve(SOCKET client, const std::string& request)
    {
        const int n = ++served;
        std::size_t from = 0;
        std::size_t to = data.size() - 1;
        const std::size_t at = request.find("Range: bytes=");
        const bool ranged = ranges && at != std::string::npos;
        if (ranged) {
            from = std::stoul(request.substr(at + 13));
            to = std::min(to, static_cast<std::size_t>(std::stoul(request.substr(request.find('-', at + 13) + 1))));
        }
        if (stallAfter && n > stallAfter)
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
        const std::string body = data.substr(from, to - from + 1);
        sendText(client, (ranged ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/"
                                     + std::to_string(data.size()) + "\r\n"
                                 : std::string("HTTP/1.1 200 OK\r\n"))
                + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n");
        if (n == breakRequest) {
            sendText(client, body.substr(0, body.size() / 2));
            ::shutdown(client, SD_BOTH);
            return;
        }
        sendText(client, body);
    }
};

} // namespace

class TestLocalModel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        WSADATA wsa {};
        QCOMPARE(::WSAStartup(MAKEWORD(2, 2), &wsa), 0);
    }
    void cleanupTestCase() { ::WSACleanup(); }

    void chatModel()
    {
        QTemporaryDir dir;
        const auto file = pathOf(dir.filePath(u"Qwen3-4B-Instruct-Q4_K_M.gguf"_s));
        Gguf()
            .string("general.architecture", "qwen3")
            .string("general.name", "Qwen3 4B Instruct")
            .u32("general.file_type", 15)
            .u32("qwen3.context_length", 40960)
            .f32("qwen3.rope.freq_base", 1e6f)
            .strings("tokenizer.ggml.tokens", {"a", "b", "c"})
            .write(file);
        const lm::ModelInfo info = lm::readModel(file, true);
        QVERIFY2(info.error.empty(), info.error.c_str());
        QCOMPARE(info.architecture, std::string("qwen3"));
        QCOMPARE(info.name, std::string("Qwen3 4B Instruct"));
        QCOMPARE(info.quantization, std::string("Q4_K_M"));
        QCOMPARE(info.contextLength, std::uint64_t(40960));
        QVERIFY(!info.embedding);
        QVERIFY(lm::describe(info).starts_with("Qwen3 4B Instruct · Q4_K_M · "));
    }

    void nameFromTheFile()
    {
        // No general.name: the file's own name, in its Unicode (not the ANSI code page).
        QTemporaryDir dir;
        const auto file = pathOf(dir.filePath(u"翻译模型-Q8.gguf"_s));
        Gguf().string("general.architecture", "llama").u32("general.file_type", 7).write(file);
        const lm::ModelInfo info = lm::readModel(file, true);
        QVERIFY2(info.error.empty(), info.error.c_str());
        QCOMPARE(QString::fromStdString(info.name), u"翻译模型-Q8"_s);
        QCOMPARE(info.quantization, std::string("Q8_0"));
    }

    void notAChatModel()
    {
        QTemporaryDir dir;
        const auto embedding = pathOf(dir.filePath(u"e.gguf"_s));
        Gguf().string("general.architecture", "qwen3").u32("qwen3.pooling_type", 3).write(embedding);
        QVERIFY(lm::readModel(embedding, true).embedding);
        const auto bert = pathOf(dir.filePath(u"b.gguf"_s));
        Gguf().string("general.architecture", "bert").write(bert);
        QVERIFY(lm::readModel(bert, true).embedding);

        const auto mmproj = pathOf(dir.filePath(u"mmproj.gguf"_s));
        Gguf().string("general.architecture", "clip").string("general.type", "mmproj").write(mmproj);
        QVERIFY(lm::readModel(mmproj, true).error.find("mmproj") != std::string::npos);
        const auto lora = pathOf(dir.filePath(u"lora.gguf"_s));
        Gguf().string("general.architecture", "llama").string("general.type", "adapter").write(lora);
        QVERIFY(lm::readModel(lora, false).error.find("LoRA") != std::string::npos);
    }

    void brokenFiles()
    {
        QTemporaryDir dir;
        QVERIFY(lm::readModel(pathOf(dir.filePath(u"missing.gguf"_s)), true).error == "找不到模型文件");
        const auto text = pathOf(dir.filePath(u"a.gguf"_s));
        std::ofstream(text) << "not a model at all";
        QVERIFY(lm::readModel(text, true).error.starts_with("这不是 GGUF"));
        const auto cut = pathOf(dir.filePath(u"cut.gguf"_s));
        Gguf().string("general.architecture", "qwen3").string("general.name", "Cut short").write(cut, 4);
        QCOMPARE(lm::readModel(cut, false).error, std::string("The model file is incomplete"));
        const auto none = pathOf(dir.filePath(u"none.gguf"_s));
        Gguf().string("general.name", "No architecture").write(none);
        QVERIFY(lm::readModel(none, true).error.find("general.architecture") != std::string::npos);
    }

    void sizesAndQuantizations()
    {
        QCOMPARE(lm::sizeText(1280835840), std::string("1.2 GB"));
        QCOMPARE(lm::sizeText(512ull << 20), std::string("512 MB"));
        QCOMPARE(lm::quantizationName(1), std::string("F16"));
        QCOMPARE(lm::quantizationName(2), std::string("Q4_0"));
        QCOMPARE(lm::quantizationName(17), std::string("Q5_K_M"));
        QCOMPARE(lm::quantizationName(18), std::string("Q6_K"));
        QVERIFY(lm::quantizationName(9999).empty());
    }

    // The manifest's choices are the catalog's models and "file"; every
    // download is pinned (a revision, a size and a SHA-256) on ModelScope.
    void catalogAndManifest()
    {
        std::ifstream in(pathOf(QString::fromUtf8(WINSHUN_LOCALMODEL_MANIFEST))); // UTF-8: a path with Chinese in it
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto manifest = Json::parse(text);
        QVERIFY(manifest);
        // [0] the chat models ("model"), [1] the embedding models ("embedModel").
        std::vector<std::string> choices[2];
        std::string fallback[2];
        for (const Json& s : (*manifest)["settings"].toArray()) {
            const std::string key = s["key"].toString();
            if (key == "model" || key == "embedModel") {
                const int kind = key == "embedModel";
                fallback[kind] = s["default"].toString();
                for (const Json& o : s["options"].toArray())
                    choices[kind].push_back(o["value"].toString());
            }
            if (key == "port")
                QCOMPARE(s["default"].toInt(), static_cast<long long>(lm::kDefaultPort));
        }
        for (const int kind : {0, 1}) {
            const lm::Recommended* r = lm::findRecommended(fallback[kind]);
            QVERIFY(r);
            QCOMPARE(r->embedding, kind == 1);
        }
        std::vector<std::string> expected[2];
        for (const lm::Recommended& r : lm::recommended()) {
            expected[r.embedding].emplace_back(r.id);
            QVERIFY(!(r.embedding && r.translationOnly));
            QVERIFY(r.url.starts_with("https://www.modelscope.cn/models/"));
            QVERIFY2(r.url.find("/resolve/") != std::string_view::npos && r.url.find("/master/") == std::string_view::npos,
                "pinned to a revision, not master");
            QVERIFY(r.url.ends_with(r.file));
            QVERIFY(r.file.ends_with(".gguf"));
            QCOMPARE(r.sha256.size(), std::size_t(64));
            QVERIFY(r.size > (500ull << 20));
        }
        for (const int kind : {0, 1}) {
            expected[kind].emplace_back("file");
            QCOMPARE(choices[kind], expected[kind]);
        }
        QVERIFY(!lm::findRecommended("file"));
    }

    void launchArguments()
    {
        lm::Launch launch {L"C:\\Program Files\\x\\llama-server.exe", L"D:\\模型 文件\\m.gguf", "qwen3.5-2b", true, 8192, 40123};
        const std::vector<std::wstring> args = lm::arguments(launch);
        const auto after = [&](const wchar_t* name) {
            const auto it = std::ranges::find(args, std::wstring(name));
            return it == args.end() || it + 1 == args.end() ? std::wstring() : *(it + 1);
        };
        QCOMPARE(after(L"--host"), std::wstring(L"127.0.0.1"));
        QCOMPARE(after(L"--port"), std::wstring(L"40123"));
        QCOMPARE(after(L"--model"), std::wstring(L"D:\\模型 文件\\m.gguf"));
        QCOMPARE(after(L"--alias"), std::wstring(L"qwen3.5-2b"));
        QCOMPARE(after(L"--ctx-size"), std::wstring(L"8192"));
        QVERIFY(std::ranges::find(args, std::wstring(L"--offline")) != args.end());
        QCOMPARE(after(L"--cache-ram"), std::wstring(L"0")); // no prompt cache of up to 8 GB
        QCOMPARE(after(L"--ctx-checkpoints"), std::wstring(L"2"));
        QCOMPARE(after(L"-lv"), std::wstring(L"4")); // the device and offload lines come at 4
        QVERIFY(std::ranges::find(args, std::wstring(L"--device")) == args.end());
        QVERIFY(std::ranges::find(args, std::wstring(L"--embedding")) == args.end());
        {
            // An embedding model: a text goes in one batch, a slot's context long.
            lm::Launch embedding = launch;
            embedding.embedding = true;
            const std::vector<std::wstring> with = lm::arguments(embedding);
            const auto value = [&](const wchar_t* name) { return *(std::ranges::find(with, std::wstring(name)) + 1); };
            QVERIFY(std::ranges::find(with, std::wstring(L"--embedding")) != with.end());
            QCOMPARE(value(L"--batch-size"), std::wstring(L"4096"));
            QCOMPARE(value(L"--ubatch-size"), std::wstring(L"4096"));
        }
        launch.gpu = false;
        QCOMPARE(after(L"--device"), std::wstring());
        QCOMPARE(lm::arguments(launch).back(), std::wstring(L"none"));
        const std::wstring line = lm::commandLine(launch);
        QVERIFY(line.starts_with(L"\"C:\\Program Files\\x\\llama-server.exe\" "));
        QVERIFY(line.find(L"\"D:\\模型 文件\\m.gguf\"") != std::wstring::npos);
    }

    void logLines()
    {
        lm::LogFacts facts;
        lm::readLogLine("llama_model_load_from_file_impl: using device Vulkan0 (NVIDIA GeForce RTX 4060 Laptop GPU) (0000:01:00.0) - 7069 MiB free", facts);
        lm::readLogLine("load_tensors: offloaded 25/25 layers to GPU", facts);
        QCOMPARE(facts.device, std::string("NVIDIA GeForce RTX 4060 Laptop GPU"));
        QCOMPARE(facts.offloaded, 25);
        QCOMPARE(facts.layers, 25);
        // As b12359 writes them at -lv 4: the time and the level first.
        lm::LogFacts timed;
        lm::readLogLine("0.00.548.074 I llama_prepare_model_devices: using device Vulkan0 (NVIDIA GeForce RTX 4060 Laptop GPU) (0000:01:00.0) - 7186 MiB free", timed);
        lm::readLogLine("0.01.293.893 I load_tensors: offloaded 25/25 layers to GPU", timed);
        QCOMPARE(timed.device, std::string("NVIDIA GeForce RTX 4060 Laptop GPU"));
        QCOMPARE(timed.offloaded, 25);

        const auto unknown = lm::failureText({"llama_model_load: error loading model: error loading model architecture: unknown model architecture: 'futurenet'"}, true, true);
        QVERIFY2(unknown.find("futurenet") != std::string::npos && unknown.find("不认识") != std::string::npos, unknown.c_str());
        const auto memory = lm::failureText({"ggml_vulkan: Device memory allocation of size 2147483648 failed.", "ErrorOutOfDeviceMemory"}, true, true);
        QVERIFY2(memory.find("显存") != std::string::npos, memory.c_str());
        QCOMPARE(lm::failureText({"main: something else went wrong"}, false, false),
            std::string("The model did not load (main: something else went wrong)"));
        QCOMPARE(lm::failureText({}, false, true), std::string("模型没能加载"));

        // Ended by itself after loading: the job's memory limit (STATUS_NO_MEMORY), a crash.
        const auto noMemory = lm::stoppedText({"0.05.008.092 I slot launch_slot_: id  0 | task 0 | processing task"}, 0xC0000017ul, true, true);
        QVERIFY2(noMemory.find("显存或内存不够") != std::string::npos, noMemory.c_str());
        QVERIFY2(lm::stoppedText({"std::bad_alloc"}, 0xE06D7363ul, false, false).find("not enough memory") != std::string::npos, "bad_alloc");
        QCOMPARE(lm::stoppedText({}, 0xC0000005ul, true, true), std::string("模型意外停了（0xC0000005），再用到时会重新加载"));
    }

    void whoMayAsk()
    {
        QVERIFY(lm::loopbackHost("127.0.0.1:39281", 39281));
        QVERIFY(lm::loopbackHost("LOCALHOST:39281", 39281));
        QVERIFY(lm::loopbackHost("localhost", 39281));
        QVERIFY(lm::loopbackHost("[::1]:39281", 39281));
        QVERIFY(!lm::loopbackHost("127.0.0.1:80", 39281));
        QVERIFY(!lm::loopbackHost("evil.example:39281", 39281)); // DNS rebinding
        QVERIFY(!lm::loopbackHost("127.0.0.1.evil.example:39281", 39281));
        QVERIFY(!lm::loopbackHost("", 39281));
        QVERIFY(lm::allowedOrigin("http://127.0.0.1:39281", 39281));
        QVERIFY(lm::allowedOrigin("http://localhost:39281", 39281));
        QVERIFY(!lm::allowedOrigin("https://evil.example", 39281));
        QVERIFY(!lm::allowedOrigin("http://127.0.0.1:8080", 39281));
        QVERIFY(!lm::allowedOrigin("null", 39281));
    }

    // A request goes to the server acquire names: the body whole, the
    // relay's Host, no Origin; the answer comes back as it is (streamed).
    void relayPassesOn()
    {
        Server upstream([](SOCKET client, const std::string&) {
            sendText(client, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n");
            sendText(client, "data: {\"n\":1}\n\n");
            sendText(client, "data: [DONE]\n\n");
        });
        std::atomic_int acquired = 0;
        std::atomic_int busy = 0;
        std::atomic_int busyMost = 0;
        lm::Relay relay(
            [&](const std::string&, const std::string&) {
                ++acquired;
                return lm::Target {upstream.port()};
            },
            [&](int delta, const std::string&) {
                busy += delta;
                busyMost = std::max(busyMost.load(), busy.load());
            },
            [](const std::string& method, const std::string& path) -> std::optional<std::pair<int, std::string>> {
                if (method == "GET" && path == "/health")
                    return std::pair {200, std::string(R"({"status":"ok"})")};
                return std::nullopt;
            });
        std::string error;
        QVERIFY2(relay.listen(0, error, true), error.c_str());
        const int port = relay.port();
        const std::string body = R"({"model":"x","messages":[{"role":"user","content":"你好"}],"stream":true})";
        const std::string response = roundTrip(port,
            "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port)
                + "\r\nOrigin: http://127.0.0.1:" + std::to_string(port) + "\r\nContent-Type: application/json\r\nContent-Length: "
                + std::to_string(body.size()) + "\r\n\r\n" + body);
        QCOMPARE(statusOf(response), 200);
        QCOMPARE(bodyOf(response), std::string("data: {\"n\":1}\n\ndata: [DONE]\n\n"));
        QCOMPARE(acquired.load(), 1);
        QCOMPARE(busyMost.load(), 1);
        QTRY_COMPARE(busy.load(), 0);
        const auto seen = upstream.requests();
        QCOMPARE(seen.size(), std::size_t(1));
        const std::string& request = seen.front();
        QVERIFY(request.starts_with("POST /v1/chat/completions HTTP/1.1\r\n"));
        QVERIFY(request.find("host: 127.0.0.1:" + std::to_string(upstream.port())) != std::string::npos
            || request.find("Host: 127.0.0.1:" + std::to_string(upstream.port())) != std::string::npos);
        QVERIFY(request.find("rigin:") == std::string::npos);
        QVERIFY(request.ends_with(body));

        // Answered by the relay itself: no server needed.
        const std::string health = roundTrip(port, "GET /health HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\n\r\n");
        QCOMPARE(statusOf(health), 200);
        QCOMPARE(acquired.load(), 1);
        relay.close();
    }

    // The server ended before it answered (its memory, a crash): an answer
    // that says so, not a connection that just closes.
    void relaySaysTheServerStopped()
    {
        Server upstream([](SOCKET, const std::string&) {}); // reads the request, closes
        lm::Relay relay([&](const std::string&, const std::string&) { return lm::Target {upstream.port()}; }, [](int, const std::string&) {},
            [](const std::string&, const std::string&) { return std::optional<std::pair<int, std::string>>(); });
        std::string error;
        QVERIFY2(relay.listen(0, error, true), error.c_str());
        const std::string port = std::to_string(relay.port());
        const std::string response = roundTrip(relay.port(),
            "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:" + port + "\r\nContent-Length: 2\r\n\r\n{}");
        QCOMPARE(statusOf(response), 502);
        const auto answer = Json::parse(bodyOf(response));
        QVERIFY(answer);
        QCOMPARE((*answer)["error"]["type"].toString(), std::string("winshun_local_model"));
        QCOMPARE((*answer)["error"]["code"].toString(), std::string("stopped"));
        QVERIFY((*answer)["error"]["message"].toString().find("卡片") != std::string::npos);
        relay.close();
    }

    void relayRefusesAndExplains()
    {
        lm::Relay relay([](const std::string&, const std::string&) { return lm::Target {0, 503, "no_model", "还没下载模型"}; },
            [](int, const std::string&) {}, [](const std::string&, const std::string&) { return std::optional<std::pair<int, std::string>>(); });
        std::string error;
        QVERIFY2(relay.listen(0, error, true), error.c_str());
        const std::string port = std::to_string(relay.port());

        const std::string none = roundTrip(relay.port(), "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:" + port + "\r\nContent-Length: 2\r\n\r\n{}");
        QCOMPARE(statusOf(none), 503);
        const auto answer = Json::parse(bodyOf(none));
        QVERIFY(answer);
        QCOMPARE((*answer)["error"]["code"].toString(), std::string("no_model"));
        QCOMPARE((*answer)["error"]["message"].toString(), std::string("还没下载模型"));

        // Another name for this address (DNS rebinding), another site's page.
        QCOMPARE(statusOf(roundTrip(relay.port(), "GET /v1/models HTTP/1.1\r\nHost: evil.example:" + port + "\r\n\r\n")), 403);
        QCOMPARE(statusOf(roundTrip(relay.port(),
                     "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:" + port + "\r\nOrigin: https://evil.example\r\nContent-Length: 2\r\n\r\n{}")),
            403);
        relay.close();
    }

    void portTaken()
    {
        // Held by "another program" (exclusively, as the relay holds its own).
        Sock other(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        const BOOL exclusive = TRUE;
        ::setsockopt(other.s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        QCOMPARE(::bind(other.s, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        QCOMPARE(::listen(other.s, 1), 0);
        const int port = portOf(other.s);
        lm::Relay relay([](const std::string&, const std::string&) { return lm::Target {}; }, [](int, const std::string&) {},
            [](const std::string&, const std::string&) { return std::optional<std::pair<int, std::string>>(); });
        std::string error;
        QVERIFY(!relay.listen(port, error, true));
        QVERIFY2(error.find(std::to_string(port)) != std::string::npos && error.find("占") != std::string::npos, error.c_str());
        QCOMPARE(relay.port(), 0);
    }

    void engineWithoutModel()
    {
        int changed = 0;
        lm::Engine engine([&] { ++changed; });
        lm::EngineConfig config;
        config.unusable = "模型还在下载（42%），下好就能用";
        config.unusableCode = "downloading";
        engine.configure(config, true);
        QVERIFY(!engine.usable());
        QVERIFY(changed > 0);
        const lm::Target t = engine.acquire();
        QCOMPARE(t.port, 0);
        QCOMPARE(t.status, 503);
        QCOMPARE(t.code, std::string("downloading"));
        QCOMPARE(t.message, config.unusable);
        QCOMPARE(engine.state().phase, lm::Engine::Phase::Unloaded);
        // A program that is not there: a failed load that says why.
        config = {};
        config.program = L"C:\\nowhere\\llama-server.exe";
        config.model = L"C:\\nowhere\\m.gguf";
        config.alias = "m";
        engine.configure(config, false);
        QVERIFY(engine.usable());
        const lm::Target failed = engine.acquire();
        QCOMPARE(failed.code, std::string("load_failed"));
        QVERIFY(!failed.message.empty());
        QCOMPARE(engine.state().phase, lm::Engine::Phase::Failed);
    }

    // Pieces over three connections; the second request breaks off halfway
    // and that piece is asked for again. The file is whole, checked,
    // renamed, its .part and map gone.
    void downloadInPieces()
    {
        Pieces pieces;
        pieces.breakRequest = 2;
        QTemporaryDir dir;
        const auto target = pathOf(dir.filePath(u"模型.gguf"_s));
        std::atomic_bool cancel = false;
        std::uint64_t lastDone = 0;
        bool checked = false;
        const lm::DownloadResult ok = lm::download(pieces.url(), target, pieces.data.size(), sha256Hex(pieces.data), cancel,
            [&](std::uint64_t done, std::uint64_t, bool checking) {
                lastDone = done;
                checked = checked || checking;
            },
            true, pieces.tuning);
        QVERIFY2(ok.ok, ok.error.c_str());
        QVERIFY(checked);
        QCOMPARE(lastDone, std::uint64_t(pieces.data.size()));
        QVERIFY(readFile(target) == pieces.data);
        QVERIFY(!std::filesystem::exists(std::filesystem::path(target).concat(L".part")));
        QVERIFY(!std::filesystem::exists(std::filesystem::path(target).concat(L".part.map")));
        QCOMPARE(pieces.served.load(), Pieces::kCount + 1); // each piece once, the broken one twice
    }

    // Stopped after some pieces: the next download asks only for the rest.
    void downloadGoesOnFromTheMap()
    {
        Pieces pieces;
        pieces.stallAfter = 2; // the third request on waits a little: the cancel comes first
        QTemporaryDir dir;
        const auto target = pathOf(dir.filePath(u"m.gguf"_s));
        std::atomic_bool cancel = false;
        const lm::DownloadResult stopped = lm::download(pieces.url(), target, pieces.data.size(), sha256Hex(pieces.data), cancel,
            [&](std::uint64_t done, std::uint64_t, bool) {
                if (done >= 2 * Pieces::kPiece)
                    cancel = true;
            },
            true, pieces.tuning);
        QVERIFY(stopped.canceled);
        QVERIFY(std::filesystem::exists(std::filesystem::path(target).concat(L".part.map")));
        const int first = pieces.served.load();
        QVERIFY(first >= 2);

        pieces.stallAfter = 0;
        cancel = false;
        const int before = pieces.served.load();
        const lm::DownloadResult ok = lm::download(pieces.url(), target, pieces.data.size(), sha256Hex(pieces.data), cancel, {}, true, pieces.tuning);
        QVERIFY2(ok.ok, ok.error.c_str());
        QVERIFY(readFile(target) == pieces.data);
        QVERIFY2(pieces.served.load() - before <= Pieces::kCount - 2, "the pieces that were in are not asked for again");
    }

    // A .part of one stream from the start (plugin 1.0's first builds): its
    // whole pieces are kept.
    void downloadKeepsAnOldPart()
    {
        Pieces pieces;
        QTemporaryDir dir;
        const auto target = pathOf(dir.filePath(u"m.gguf"_s));
        const std::string first = pieces.data.substr(0, 2 * Pieces::kPiece + 1000);
        std::ofstream(std::filesystem::path(target).concat(L".part"), std::ios::binary).write(first.data(), static_cast<std::streamsize>(first.size()));
        std::atomic_bool cancel = false;
        const lm::DownloadResult ok = lm::download(pieces.url(), target, pieces.data.size(), sha256Hex(pieces.data), cancel, {}, true, pieces.tuning);
        QVERIFY2(ok.ok, ok.error.c_str());
        QVERIFY(readFile(target) == pieces.data);
        QCOMPARE(pieces.served.load(), Pieces::kCount - 2);
    }

    // A server that sends the whole file for a Range: one stream instead.
    void downloadWithoutRanges()
    {
        Pieces pieces;
        pieces.ranges = false;
        QTemporaryDir dir;
        const auto target = pathOf(dir.filePath(u"m.gguf"_s));
        std::atomic_bool cancel = false;
        const lm::DownloadResult ok = lm::download(pieces.url(), target, pieces.data.size(), sha256Hex(pieces.data), cancel, {}, false, pieces.tuning);
        QVERIFY2(ok.ok, ok.error.c_str());
        QVERIFY(readFile(target) == pieces.data);
    }

    // Bytes that are not what the SHA-256 says: not kept.
    void downloadFailsTheCheck()
    {
        Pieces pieces;
        QTemporaryDir dir;
        const auto target = pathOf(dir.filePath(u"m.gguf"_s));
        std::atomic_bool cancel = false;
        const lm::DownloadResult bad = lm::download(pieces.url(), target, pieces.data.size(), std::string(64, '0'), cancel, {}, false, pieces.tuning);
        QVERIFY(!bad.ok && !bad.canceled);
        QVERIFY(!bad.error.empty());
        QVERIFY(!std::filesystem::exists(target));
        QVERIFY(!std::filesystem::exists(std::filesystem::path(target).concat(L".part")));
        QVERIFY(!std::filesystem::exists(std::filesystem::path(target).concat(L".part.map")));
    }
};

QTEST_GUILESS_MAIN(TestLocalModel)
#include "tst_localmodel.moc"
