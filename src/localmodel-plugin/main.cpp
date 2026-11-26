// WinShunLocalModel.exe: the winshun.localmodel plugin ("本地模型") of the
// plugin library. It runs a model on this computer with llama.cpp's
// llama-server (in the package's llama\ folder: the CPU, and the GPU through
// Vulkan when there is one) and offers it at http://127.0.0.1:39281/v1, an
// OpenAI-compatible API: the translation plugin's "本地模型" service uses
// it, and so can any other program on this computer. Nothing is uploaded.
//
// A second model, loaded the same way, turns text into vectors
// (/v1/embeddings) for the 按意思找 plugin.
//
// The model is one Win顺 recommends (downloaded once from ModelScope and
// checked) or a .gguf file of the user's own, used where it is. It is
// loaded when a request comes and let go of after some idle minutes; the
// plugin's own process stays small while it waits.
//
// Threads: serve() (requests from Win顺), the ticker (the card, the port,
// letting an idle model go), the relay's (one per connection), a download,
// a "Retry" load. The relay is opened and closed by the ticker alone.

#include "winshun.h"

#include "Catalog.h"
#include "Download.h"
#include "Engine.h"
#include "Relay.h"

#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

using winshun::Item;
using winshun::Items;
using winshun::Json;

namespace {

using namespace std::chrono_literals;

const char* const kIcon = "ph:cpu";
const char* const kOwnFile = "file"; // a model setting's value for a file of the user's own

// The two models it runs, each in a llama-server of its own: one that chats
// (translation, the chat page) and one that turns text into vectors
// (/v1/embeddings: 按意思找). Each is loaded when a request of its kind comes.
enum Role { Chat = 0, Embed = 1 };
constexpr Role kRoles[] = {Chat, Embed};
const char* const kDefaultModel[] = {"qwen3.5-2b", "qwen3-embedding-0.6b"};
const char* const kDownloadAction[] = {"download", "download-embed"};

struct Options {
    std::string model[2] = {kDefaultModel[Chat], kDefaultModel[Embed]};
    std::string file[2];
    bool gpu = true;
    int idleMinutes = 5;
    int port = lm::kDefaultPort;
};

Options optionsFrom(const Json& s)
{
    Options o;
    const auto chosen = [&](Role role, const char* modelKey, const char* fileKey) {
        const std::string model = winshun::trim(s[modelKey].toString());
        const lm::Recommended* r = lm::findRecommended(model);
        if (model == kOwnFile || (r && r->embedding == (role == Embed)))
            o.model[role] = model; // anything else (a model since taken off the list): the default
        o.file[role] = winshun::trim(s[fileKey].toString());
    };
    chosen(Chat, "model", "modelFile");
    chosen(Embed, "embedModel", "embedFile");
    o.gpu = s["gpu"].toString() != "off";
    o.idleMinutes = std::clamp(static_cast<int>(s["idleMinutes"].toNumber(5)), 1, 60);
    const long long port = s["port"].toInt(lm::kDefaultPort);
    o.port = port >= 1024 && port <= 65535 ? static_cast<int>(port) : lm::kDefaultPort;
    return o;
}

// The requests the embedding model answers; everything else is the chat
// model's (its API and its chat page).
Role roleOf(const std::string& path)
{
    return path.starts_with("/v1/embedding") || path.starts_with("/embedding") ? Embed : Chat;
}

std::string percent(std::uint64_t done, std::uint64_t total)
{
    return std::to_string(total ? done * 100 / total : 0) + "%";
}

// "1.6 GB" with one decimal, as Task Manager would round it.
std::string memoryText(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    return text;
}

std::filesystem::path pathOf(const std::string& utf8)
{
    return std::filesystem::path(winshun::wide(utf8));
}

int toneRank(const std::string& tone)
{
    return tone == "error" ? 3 : tone == "warn" ? 2 : tone == "busy" ? 1 : 0;
}

class LocalModel : public winshun::Plugin {
public:
    LocalModel()
        : m_chat([this] { wake(); })
        , m_embed([this] { wake(); })
        , m_relay(
              [this](const std::string&, const std::string& path) {
                  const Role role = roleOf(path);
                  if (role == Embed)
                      wantEmbedding();
                  return engine(role).acquire();
              },
              [this](int delta, const std::string& path) { engine(roleOf(path)).busy(delta); },
              [this](const std::string& method, const std::string& path) { return answerHere(method, path); })
    {
    }
    ~LocalModel() override { stopAll(); }

protected:
    std::string name() const override { return "winshun.localmodel"; }

    void initialized() override
    {
        std::error_code ec;
        m_embedWanted = std::filesystem::exists(wantedMark(), ec);
        applySettings();
        m_ticker = std::thread([this] { tickLoop(); });
    }

    void settingsChanged(const Json&) override { applySettings(); }

    void shuttingDown() override { stopAll(); }

    Items query(const winshun::Query&) override { return rows(snapshot()); }

    Json queryResult(const winshun::Query&) override
    {
        const Snapshot s = snapshot();
        Json::Object answer {{"items", winshun::itemsJson(rows(s))}};
        // While it changes by itself (a download, a load), the list follows.
        if (s.downloading || s.loading)
            answer["rerun"] = 1;
        return Json(std::move(answer));
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        const bool zh = chinese();
        const std::string what = a.trigger == "status" ? a.action : a.item["id"].toString();
        for (const Role role : kRoles) {
            if (what != kDownloadAction[role])
                continue;
            const bool started = startDownload(role);
            return {winshun::effect::notify(started ? (zh ? "开始下载，进度在“本地模型”的设置卡片上" : "Downloading: the progress is on the plugin's card")
                                                    : (zh ? "另一个模型正在下载，等它下完再下这个" : "Another model is downloading: this one after it")),
                winshun::effect::keep()};
        }
        if (what == "cancel") {
            m_cancelDownload = true;
            wake();
            return {winshun::effect::notify(zh ? "已取消下载，下好的部分留着" : "Download canceled; what came is kept"), winshun::effect::keep()};
        }
        if (what == "unload") {
            const bool chat = m_chat.unload();
            const bool embed = m_embed.unload();
            return {winshun::effect::notify(chat && embed ? (zh ? "已释放内存" : "Memory freed")
                                                          : (zh ? "模型正在加载或回答，稍后再试" : "The model is loading or answering: try again in a moment")),
                winshun::effect::keep()};
        }
        if (what == "retry") {
            loadInBackground();
            return {winshun::effect::keep()};
        }
        return winshun::Plugin::action(a);
    }

private:
    struct Line {
        std::string text;
        double progress = -1;
        std::string tone;
        std::vector<std::pair<std::string, std::string>> actions;
        bool operator==(const Line&) const = default;
    };
    struct Snapshot {
        Line line;
        bool usable[2] = {};
        bool canDownload[2] = {};
        std::string name[2]; // the recommended model chosen, when it is one
        std::string size[2];
        bool loaded = false; // one of the two is
        bool loading = false;
        bool downloading = false;
        int port = 0;
    };
    struct DownloadState {
        std::string id; // the recommended model it is for
        bool running = false;
        bool checking = false;
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        std::string error;
    };

    lm::Engine& engine(Role role) { return role == Embed ? m_embed : m_chat; }

    // The embedding model is spoken of (on the card, in the errors) once
    // something asked for it: who only translates is not told about a
    // second model to download. Remembered across restarts.
    std::filesystem::path wantedMark() const { return pathOf(dataDir()) / L"embed.wanted"; }
    void wantEmbedding()
    {
        if (m_embedWanted.exchange(true))
            return;
        if (const HANDLE mark = ::CreateFileW(wantedMark().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            mark != INVALID_HANDLE_VALUE)
            ::CloseHandle(mark);
        wake();
    }

    // What the relay answers itself, without loading a model: whether it is
    // there, and the models' names (clients ask before anything else).
    // "kind" is Win顺's own: which one chats and which one embeds.
    std::optional<std::pair<int, std::string>> answerHere(const std::string& method, const std::string& path)
    {
        if (method != "GET")
            return std::nullopt;
        if (path == "/health")
            return std::pair {200, std::string(R"({"status":"ok"})")};
        if (path == "/v1/models" || path == "/models") {
            Json::Array data;
            for (const Role role : kRoles) {
                if (const std::string alias = engine(role).alias(); !alias.empty() && engine(role).usable())
                    data.push_back(Json::Object {{"id", alias}, {"object", "model"}, {"owned_by", "winshun"},
                        {"kind", role == Embed ? "embedding" : "chat"}});
            }
            return std::pair {200, Json(Json::Object {{"object", "list"}, {"data", Json(std::move(data))}}).dump()};
        }
        return std::nullopt;
    }

    Items rows(const Snapshot& s) const
    {
        const bool zh = chinese();
        const bool links = capabilities()["settingsLinks"].toBool();
        Items rows;
        rows.push_back(Item("status", s.line.text, zh ? "本地模型 · Enter 打开设置" : "Local models · Enter opens the settings")
                           .withIcon(kIcon)
                           .withUrl("winshun:settings"));
        for (const Role role : kRoles) {
            if (!s.canDownload[role])
                continue;
            const std::string title = (zh ? "下载 " : "Download ") + s.name[role] + (zh ? "（" : " (") + s.size[role] + (zh ? "）" : ")");
            rows.push_back(Item(kDownloadAction[role], title,
                role == Embed ? (zh ? "嵌入模型，“按意思找”离线用；从 ModelScope 下载" : "The embedding model, for Search by meaning offline; from ModelScope")
                              : (zh ? "从 ModelScope 下载，下好就能离线用" : "From ModelScope; works offline once it is here"))
                               .withIcon("ph:download-simple"));
        }
        if (s.downloading)
            rows.push_back(Item("cancel", zh ? "取消下载" : "Cancel the download",
                zh ? "已下的部分留着，下次接着下" : "What came is kept: the next try goes on from there")
                               .withIcon("ph:x-circle"));
        if (s.usable[Chat]) {
            const std::string address = "http://127.0.0.1:" + std::to_string(s.port) + "/";
            rows.push_back(Item("chat", zh ? "在浏览器里和模型对话" : "Chat with the model in the browser",
                address + (zh ? " · llama.cpp 自带的对话页，只在本机" : " · llama.cpp's own chat page, on this computer only"))
                               .withIcon("ph:chat-circle-text")
                               .withUrl(address));
            if (links)
                rows.push_back(Item("translate", zh ? "让“翻译”用本地模型" : "Let Translate use the local model",
                    zh ? "打开“翻译”的设置，把翻译服务选成“本地模型（离线）”"
                       : "Opens Translate's settings: set its service to Local model (offline)")
                                   .withIcon("ph:translate")
                                   .withUrl("winshun:settings/plugin/winshun.translate"));
        }
        if (s.usable[Embed] && links)
            rows.push_back(Item("semantic", zh ? "让“按意思找”用本地模型" : "Let Search by meaning use the local model",
                zh ? "打开“按意思找”的设置，把模型服务选成“本地模型（离线）”"
                   : "Opens Search by meaning's settings: set its model service to Local model (offline)")
                               .withIcon("ph:magnifying-glass")
                               .withUrl("winshun:settings/plugin/winshun.semantic"));
        if (s.loaded)
            rows.push_back(Item("unload", zh ? "释放内存" : "Free the memory",
                zh ? "现在就把模型从内存里移走；再用到时会重新加载" : "Lets the model go now; it is loaded again when needed")
                               .withIcon("ph:eject"));
        return rows;
    }

    std::filesystem::path modelsFolder() const { return pathOf(dataDir()) / L"models"; }

    // What a file of the user's own is, read once per file (path, size, time).
    lm::ModelInfo infoOf(const std::filesystem::path& file, bool zh)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        const auto time = std::filesystem::last_write_time(file, ec);
        const std::string key = winshun::utf8(file.wstring()) + "|" + std::to_string(size) + "|"
            + std::to_string(time.time_since_epoch().count()) + (zh ? "|zh" : "|en");
        const std::lock_guard lock(m_infoMutex);
        auto it = m_infos.find(key);
        if (it == m_infos.end()) {
            if (m_infos.size() > 16)
                m_infos.clear();
            it = m_infos.emplace(key, lm::readModel(file, zh)).first;
        }
        return it->second;
    }

    // Settings in force: the engines told what to run; the ticker moves the
    // address when the port changed.
    void applySettings()
    {
        bool cancel = false;
        {
            const std::lock_guard lock(m_mutex);
            m_options = optionsFrom(settings());
            m_chinese = chinese();
            // A download for a model no longer chosen stops (what came is kept).
            cancel = m_download.running && m_download.id != m_options.model[Chat] && m_download.id != m_options.model[Embed];
        }
        if (cancel)
            m_cancelDownload = true;
        reconfigure();
        wake();
    }

    // What an engine is to run, from the settings and the download.
    lm::EngineConfig configFor(Role role, const Options& o, const DownloadState& d, bool zh)
    {
        const bool embed = role == Embed;
        lm::EngineConfig config;
        config.program = pathOf(pluginDir()) / L"llama" / L"llama-server.exe";
        config.log = pathOf(dataDir()) / (embed ? L"embed.log" : L"server.log");
        config.gpu = o.gpu;
        config.idleSeconds = o.idleMinutes * 60;
        config.embedding = embed;
        // Two texts at a time, 2048 tokens each (a passage of 按意思找 is 1000
        // characters at most): half the memory of the chat model's context.
        config.context = embed ? 4096 : 8192;
        const auto unusable = [&](const char* code, const std::string& chinese, const std::string& english) {
            config.unusable = zh ? chinese : english;
            config.unusableCode = code;
        };
        if (const lm::Recommended* r = lm::findRecommended(o.model[role])) {
            const std::filesystem::path file = modelsFolder() / std::wstring(r->file.begin(), r->file.end());
            std::error_code ec;
            if (const auto size = std::filesystem::file_size(file, ec); !ec && size == r->size) {
                config.model = file;
                config.alias = std::string(r->id);
            } else if (d.running && d.id == r->id) {
                unusable("downloading", std::string(embed ? "嵌入模型" : "模型") + "还在下载（" + percent(d.done, d.total) + "），下好就能用",
                    std::string(embed ? "The embedding model" : "The model") + " is still downloading (" + percent(d.done, d.total) + ")");
            } else if (embed) {
                unusable("no_model", "还没下载嵌入模型（“按意思找”要用）：在 设置 → 插件 → 本地模型 里点“下载嵌入模型”",
                    "No embedding model yet (Search by meaning needs one): Settings → Plugins → Local models → Download embedding model");
            } else {
                unusable("no_model", "还没下载模型：在 设置 → 插件 → 本地模型 里点“下载”",
                    "No model downloaded yet: Settings → Plugins → Local models → Download");
            }
        } else if (o.file[role].empty()) {
            unusable("no_model", std::string("还没选") + (embed ? "嵌入" : "") + "模型文件：在 设置 → 插件 → 本地模型 里选一个 .gguf 文件",
                std::string(embed ? "No embedding model file chosen" : "No model file chosen") + ": Settings → Plugins → Local models");
        } else {
            const std::filesystem::path file = pathOf(o.file[role]);
            const lm::ModelInfo info = infoOf(file, zh);
            if (!info.error.empty()) {
                config.unusable = info.error;
                config.unusableCode = "bad_model";
            } else if (info.embedding && !embed) {
                unusable("bad_model", "这是嵌入模型（把文字变成向量用的），不能对话和翻译：换一个对话模型，或把它选到“嵌入模型文件”里",
                    "This is an embedding model: it cannot chat or translate; pick a chat model, or set it as the embedding model file");
            } else if (!info.embedding && embed) {
                unusable("bad_model", "这是对话模型，不是嵌入模型：“按意思找”要用嵌入模型（比如 Qwen3-Embedding、bge-m3）",
                    "This is a chat model, not an embedding model: Search by meaning needs one (Qwen3-Embedding, bge-m3…)");
            } else {
                config.model = file;
                std::string alias = winshun::utf8(file.stem().wstring());
                std::ranges::transform(alias, alias.begin(), [](unsigned char c) {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
                });
                config.alias = alias;
            }
        }
        return config;
    }

    void reconfigure()
    {
        Options o;
        DownloadState d;
        bool zh = true;
        {
            const std::lock_guard lock(m_mutex);
            o = m_options;
            d = m_download;
            zh = m_chinese;
        }
        for (const Role role : kRoles) {
            lm::EngineConfig config = configFor(role, o, d, zh);
            {
                const std::lock_guard lock(m_mutex);
                m_unusable[role] = config.unusable;
                m_unusableCode[role] = config.unusableCode;
            }
            engine(role).configure(std::move(config), zh);
        }
    }

    // One model's part of the card's line. `alone`: the line is this model's
    // only (who only translates sees nothing of the embedding model).
    Line partOf(Role role, const Options& o, const DownloadState& d, const std::string& unusable, const std::string& unusableCode,
        bool alone, bool zh, Snapshot& s)
    {
        const bool embed = role == Embed;
        const lm::Recommended* r = lm::findRecommended(o.model[role]);
        const std::string& name = s.name[role];
        if (!s.usable[role]) {
            if (r && !d.error.empty() && d.id == r->id)
                return {(zh ? name + " 没下载好：" : name + ": download failed: ") + d.error, -1, "error",
                    {{kDownloadAction[role], embed ? (zh ? "重试嵌入模型" : "Retry embedding model") : (zh ? "重试" : "Retry")}}};
            if (r && embed)
                return {zh ? "“按意思找”要用的 " + name + " 还没下载（" + s.size[role] + "）"
                           : name + " (for Search by meaning) is not downloaded yet (" + s.size[role] + ")",
                    -1, "warn", {{kDownloadAction[role], zh ? "下载嵌入模型" : "Download embedding model"}}};
            if (r)
                return {zh ? "还没下载 " + name + "（" + s.size[role] + "，从 ModelScope 下载）"
                           : name + " is not downloaded yet (" + s.size[role] + ", from ModelScope)",
                    -1, "warn", {{kDownloadAction[role], zh ? "下载" : "Download"}}};
            if (o.file[role].empty())
                return {embed ? (zh ? "选一个嵌入模型文件（.gguf），或选推荐的嵌入模型" : "Pick an embedding model file (.gguf), or the recommended one")
                              : (zh ? "选一个模型文件（.gguf），或在上面选一个推荐的模型" : "Pick a model file (.gguf), or one of the recommended models above"),
                    -1, "warn", {}};
            return {unusable, -1, unusableCode == "no_model" ? "warn" : "error", {}};
        }
        const std::string model = r ? name : lm::describe(infoOf(pathOf(o.file[role]), zh));
        const lm::Engine::State e = engine(role).state();
        switch (e.phase) {
        case lm::Engine::Phase::Loading:
            s.loading = true;
            return {zh ? "正在加载 " + model + "…" : "Loading " + model + "…", -1, "busy", {}};
        case lm::Engine::Phase::Loaded: {
            s.loaded = true;
            const std::string where = !e.device.empty() ? (zh ? "显卡 " : "GPU ") + e.device
                : e.cpuFallback                         ? (zh ? "显卡用不上，改用 CPU" : "on the CPU: the GPU could not")
                                                        : "CPU";
            const std::string memory = e.memory ? (zh ? "，占内存 " : ", memory ") + memoryText(e.memory) : std::string();
            return {(zh ? "已加载 " : "Loaded ") + model + " · " + where + memory, -1, e.cpuFallback ? "warn" : "ok",
                {{"unload", zh ? "释放内存" : "Free memory"}}};
        }
        case lm::Engine::Phase::Failed:
            return {(e.stopped ? (alone ? std::string() : model + (zh ? "：" : ": ")) : model + (zh ? " 加载失败：" : " could not load: ")) + e.error,
                -1, "error", {{"retry", zh ? "重试" : "Retry"}}};
        case lm::Engine::Phase::Unloaded:
            break;
        }
        if (embed)
            return {zh ? "嵌入模型 " + model + " 就绪" : "embedding model " + model + " ready", -1, "ok", {}};
        if (!alone)
            return {zh ? "就绪：" + model + "，用到时才加载" : "Ready: " + model + ", loaded when needed", -1, "ok", {}};
        return {zh ? "就绪：" + model + "，用到时才加载。把“翻译”的服务选成“本地模型”即可离线翻译"
                   : "Ready: " + model + ", loaded when needed. Set Translate's service to Local model to translate offline",
            -1, "ok", {}};
    }

    Snapshot snapshot()
    {
        Options o;
        DownloadState d;
        std::string relayError, unusable[2], unusableCode[2];
        int port = 0;
        bool zh = true;
        {
            const std::lock_guard lock(m_mutex);
            o = m_options;
            d = m_download;
            relayError = m_relayError;
            for (const Role role : kRoles) {
                unusable[role] = m_unusable[role];
                unusableCode[role] = m_unusableCode[role];
            }
            port = m_listening ? m_listening : o.port;
            zh = m_chinese;
        }
        Snapshot s;
        s.port = port;
        s.downloading = d.running;
        for (const Role role : kRoles) {
            s.usable[role] = engine(role).usable();
            if (const lm::Recommended* r = lm::findRecommended(o.model[role])) {
                s.name[role] = std::string(r->name);
                s.size[role] = lm::sizeText(r->size);
                s.canDownload[role] = !s.usable[role] && !d.running; // one download at a time
            }
        }
        Line& line = s.line;
        if (!relayError.empty()) {
            line = {relayError, -1, "error", {}};
            return s;
        }
        if (d.running) {
            const lm::Recommended* r = lm::findRecommended(d.id);
            const std::string name = r ? std::string(r->name) : d.id;
            line.text = d.checking ? (zh ? "正在校验下好的 " + name + "…" : "Checking the downloaded " + name + "…")
                                   : (zh ? "正在下载 " + name + "：" + lm::sizeText(d.done) + " / " + lm::sizeText(d.total)
                                         : "Downloading " + name + ": " + lm::sizeText(d.done) + " / " + lm::sizeText(d.total));
            line.progress = d.total ? static_cast<double>(d.done) / static_cast<double>(d.total) : 0.0;
            line.tone = "busy";
            line.actions = {{"cancel", zh ? "取消" : "Cancel"}};
            return s;
        }
        // The embedding model has its say once it is in play: asked for,
        // there, chosen as a file, or its download failed.
        const bool embedShown = m_embedWanted || s.usable[Embed] || (o.model[Embed] == kOwnFile && !o.file[Embed].empty())
            || (!d.error.empty() && d.id == o.model[Embed]);
        line = partOf(Chat, o, d, unusable[Chat], unusableCode[Chat], !embedShown, zh, s);
        if (embedShown) {
            // Who uses it for 按意思找 alone is not warned about the chat model.
            if (s.usable[Embed] && !s.usable[Chat] && line.tone == "warn")
                line.tone = "ok";
            const Line embed = partOf(Embed, o, d, unusable[Embed], unusableCode[Embed], false, zh, s);
            line.text += (zh ? "；" : "; ") + embed.text;
            if (toneRank(embed.tone) > toneRank(line.tone))
                line.tone = embed.tone;
            for (const auto& action : embed.actions) {
                if (std::ranges::find(line.actions, action.first, &std::pair<std::string, std::string>::first) == line.actions.end())
                    line.actions.push_back(action);
            }
        }
        return s;
    }

    void wake()
    {
        {
            const std::lock_guard lock(m_tickMutex);
            m_dirty = true;
        }
        m_tickCv.notify_all();
    }

    // Waits for a change, a second at most; false when the plugin ends.
    bool waitFor(std::chrono::milliseconds most, bool anyChange)
    {
        std::unique_lock lock(m_tickMutex);
        m_tickCv.wait_for(lock, most, [&] { return (anyChange && m_dirty) || m_stopping; });
        if (m_stopping)
            return false;
        if (anyChange)
            m_dirty = false;
        return true;
    }

    // The address on the port the settings name; tried again now and then
    // while another program holds it.
    void keepListening(std::chrono::steady_clock::time_point& lastTry)
    {
        int wanted = 0;
        bool failed = false;
        bool zh = true;
        {
            const std::lock_guard lock(m_mutex);
            wanted = m_options.port;
            failed = !m_relayError.empty();
            zh = m_chinese;
        }
        if (m_relay.port() == wanted && !failed)
            return;
        if (failed && m_relayPort == wanted && std::chrono::steady_clock::now() - lastTry < 15s)
            return;
        lastTry = std::chrono::steady_clock::now();
        m_relayPort = wanted;
        std::string error;
        const bool ok = m_relay.listen(wanted, error, zh);
        if (!ok)
            log("error", "listen port=" + std::to_string(wanted) + ": " + error);
        const std::lock_guard lock(m_mutex);
        m_relayError = ok ? std::string() : error;
        m_listening = ok ? m_relay.port() : 0;
    }

    // The card follows the state (a second apart at most), the port follows
    // the settings, and the engine lets an idle model go.
    void tickLoop()
    {
        auto lastTick = std::chrono::steady_clock::now();
        auto lastTry = lastTick - 1h;
        auto lastSent = lastTick - 1h;
        while (waitFor(1s, true)) {
            keepListening(lastTry);
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTick >= 2s) {
                lastTick = now;
                m_chat.tick();
                m_embed.tick();
            }
            if (const auto since = std::chrono::steady_clock::now() - lastSent; since < 1s) {
                if (!waitFor(std::chrono::duration_cast<std::chrono::milliseconds>(1s - since), false))
                    return;
            }
            if (sendStatus()) {
                lastSent = std::chrono::steady_clock::now();
                refresh(); // a launcher list showing the rows asks again
            }
        }
    }

    // The card's line, when it changed.
    bool sendStatus()
    {
        Line line = snapshot().line;
        if (line.progress >= 0)
            line.progress = static_cast<int>(line.progress * 200) / 200.0; // half a percent apart at least
        if (m_sent && line == *m_sent)
            return false;
        if (line.tone == "error" && (!m_sent || m_sent->text != line.text))
            log("warn", line.text); // what went wrong stays in the log after the card moves on
        m_sent = line;
        status(line.text, line.progress, line.tone, line.actions);
        return true;
    }

    // "Retry" on the card: what failed is loaded as a request would load it;
    // the card follows.
    void loadInBackground()
    {
        if (m_loading.exchange(true))
            return;
        if (m_loader.joinable())
            m_loader.join(); // the last one has ended (m_loading was false)
        m_loader = std::thread([this] {
            for (const Role role : kRoles) {
                if (engine(role).state().phase == lm::Engine::Phase::Failed)
                    engine(role).acquire();
            }
            m_loading = false;
        });
    }

    // False when another download is under way (one at a time).
    bool startDownload(Role role)
    {
        const lm::Recommended* r = nullptr;
        bool zh = true;
        {
            const std::lock_guard lock(m_mutex);
            r = lm::findRecommended(m_options.model[role]);
            if (!r || m_download.running)
                return false;
            zh = m_chinese;
        }
        // The last download has ended (running is false): its thread may
        // still be finishing, outside the lock.
        if (m_downloader.joinable())
            m_downloader.join();
        {
            const std::lock_guard lock(m_mutex);
            m_cancelDownload = false;
            m_download = {std::string(r->id), true, false, 0, r->size, {}};
        }
        const std::filesystem::path target = modelsFolder() / std::wstring(r->file.begin(), r->file.end());
        log("info", "download " + std::string(r->id) + " from " + std::string(r->url));
        reconfigure(); // requests meanwhile are told it is downloading
        m_downloader = std::thread([this, r, target, zh] {
            const auto started = std::chrono::steady_clock::now();
            const lm::DownloadResult result = lm::download(std::string(r->url), target, r->size, r->sha256, m_cancelDownload,
                [this](std::uint64_t done, std::uint64_t total, bool checking) {
                    {
                        const std::lock_guard lock(m_mutex);
                        m_download.done = done;
                        m_download.total = total;
                        m_download.checking = checking;
                    }
                    wake();
                },
                zh);
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
            log(result.ok ? "info" : "warn",
                "download " + std::string(r->id) + (result.ok ? " done" : result.canceled ? " canceled" : " failed: " + result.error)
                    + " seconds=" + std::to_string(seconds));
            {
                const std::lock_guard lock(m_mutex);
                m_download.running = false;
                m_download.checking = false;
                m_download.error = result.ok || result.canceled ? std::string() : result.error;
            }
            reconfigure();
            wake();
        });
        wake();
        return true;
    }

    void stopAll()
    {
        if (m_stopped.exchange(true))
            return;
        {
            const std::lock_guard lock(m_tickMutex);
            m_stopping = true;
        }
        m_tickCv.notify_all();
        m_cancelDownload = true;
        // A load in progress gives up, and the requests waiting for it are
        // answered: the relay's threads end, and the ticker's close with them.
        m_chat.shutdown();
        m_embed.shutdown();
        if (m_ticker.joinable())
            m_ticker.join();
        m_relay.close();
        if (m_loader.joinable())
            m_loader.join();
        if (m_downloader.joinable())
            m_downloader.join();
    }

    lm::Engine m_chat;
    lm::Engine m_embed;
    std::atomic_bool m_embedWanted = false; // see wantEmbedding()
    lm::Relay m_relay;
    int m_relayPort = 0; // the ticker's: the port it last tried

    std::mutex m_mutex; // the fields below
    Options m_options;
    bool m_chinese = true;
    DownloadState m_download;
    std::string m_relayError;
    int m_listening = 0; // the relay's port, 0 when it has none
    std::string m_unusable[2]; // why a model cannot be used (its engine's config)
    std::string m_unusableCode[2];

    std::mutex m_infoMutex;
    std::map<std::string, lm::ModelInfo> m_infos;

    std::atomic_bool m_cancelDownload = false;
    std::thread m_downloader;
    std::atomic_bool m_loading = false;
    std::thread m_loader;

    std::optional<Line> m_sent; // the ticker's
    std::mutex m_tickMutex;
    std::condition_variable m_tickCv;
    bool m_dirty = false;
    bool m_stopping = false;
    std::thread m_ticker;
    std::atomic_bool m_stopped = false;
};

} // namespace

int main()
{
    WSADATA wsa {};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;
    int code = 0;
    {
        LocalModel plugin;
        code = plugin.serve();
    }
    ::WSACleanup();
    return code;
}
