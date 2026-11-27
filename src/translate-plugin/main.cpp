// WinShunTranslate.exe: the winshun.translate plugin ("翻译") in the plugin
// library. "tr sentence" translates Chinese to English and anything else to
// Chinese (or into the language the settings name) with the user's own
// DeepL, Microsoft or OpenAI-compatible chat service, or with the model on
// this computer (the 本地模型 plugin, winshun.localmodel). A service's key is
// the one Win顺 keeps in the Credential Manager and hands over in the
// settings; without a service (or a key, where one is needed) nothing goes
// online and one row says what to set.
//
// The decisions and the wording are in Translate.cpp, the WinHTTP POST in
// Http.cpp. Win顺 drops a launcher query after 10 s (and restarts a plugin
// after three of those in a row), a command call (translate_text, AI, ws)
// after 30 s. So in the launcher a chat service's answer runs as a job of
// its own and streams: each query answers at once with what has come so far
// and asks to be asked again (rerun) until it is all there, however long a
// local model takes to load. DeepL, Microsoft and command calls answer
// within their time in one request.

#include "winshun.h"

#include "Http.h"
#include "Translate.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using winshun::Item;
using winshun::Items;
using winshun::Json;

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Less than the 10 s the host waits: the answer (an error row, at worst) must
// reach it in time.
constexpr int kBudgetMs = 9000;
// A command call: the host waits 30 s.
constexpr int kCommandBudgetMs = 25000;
// A streamed answer in the launcher: a local model may load first.
constexpr int kJobBudgetMs = 60000;
constexpr int kLocalJobBudgetMs = 180000;
// How long a launcher query waits for its job before it answers with what
// has come: a quick service's translation comes in the first answer.
constexpr auto kFirstWait = 1500ms;
constexpr auto kUpdateWait = 600ms;

struct Outcome {
    Items items;
    bool cacheable = false; // only a translation is worth keeping
};

// A chat service's answer on its way, for the launcher's queries about the
// same text: the reply so far, then the outcome.
struct Job {
    std::string key; // the command, the text and the settings it is for
    std::string text;
    std::string target;
    bool local = false;
    Clock::time_point started = Clock::now();
    std::atomic_bool canceled = false;
    std::mutex mutex; // the fields below
    std::condition_variable changed;
    std::string sofar;
    int version = 0; // up by one at each change
    bool done = false;
    Clock::time_point finished;
    Outcome outcome;
    std::thread thread;

    // Done a while ago: whoever wanted it has gone, and a later query is
    // asked anew (the model may run by now).
    bool stale()
    {
        const std::lock_guard lock(mutex);
        return done && Clock::now() - finished > 10s;
    }
};

class Translate : public winshun::Plugin {
public:
    ~Translate() override
    {
        stopWorker();
        stopJob();
        stopTest();
    }

protected:
    std::string name() const override { return "winshun.translate"; }

    Items query(const winshun::Query& q) override
    {
        const tr::Settings s = tr::settingsFrom(settings());
        if (auto early = gate(q, s, chinese()))
            return *early;
        return once(q, s, chinese(), q.source == "command" ? kCommandBudgetMs : kBudgetMs, links()).items;
    }

    void queryAsync(const winshun::Query& q, std::function<void(Json)> reply) override
    {
        stopWorker();
        m_queryId = q.queryId;
        m_canceled = false;
        const tr::Settings snapshot = tr::settingsFrom(settings());
        const bool zh = chinese();
        const bool settingsLinks = links();
        m_worker = std::thread([this, q, snapshot, zh, settingsLinks, reply = std::move(reply)] {
            Json answer;
            try {
                answer = respond(q, snapshot, zh, settingsLinks);
            } catch (const std::exception&) {
                answer = itemsAnswer({{tr::failedRow(zh ? "无法完成请求" : "Could not complete the request", zh)}});
            }
            reply(std::move(answer));
        });
    }

    void cancel(long long queryId) override
    {
        if (queryId == m_queryId) {
            m_canceled = true;
            wakeWaiter();
        }
    }

    void initialized() override { publishUntried(); }

    void settingsChanged(const Json&) override
    {
        stopWorker();
        stopJob();
        stopTest();
        publishUntried(); // another service, key or model: what the last test said is not about it
    }
    void shuttingDown() override
    {
        stopWorker();
        stopJob();
        stopTest();
    }

    // 测试连接 on the plugin's card (winshun/status's button, GitHub #495).
    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        if (a.trigger == "status" && a.action == "test")
            startTest();
        return {};
    }

private:
    bool links() const { return capabilities()["settingsLinks"].toBool(); }

    static Json itemsAnswer(const Outcome& o)
    {
        return Json::Object {{"items", winshun::itemsJson(o.items)}, {"cache", o.cacheable ? 300 : 0}};
    }

    void stopWorker()
    {
        m_canceled = true;
        wakeWaiter();
        if (m_worker.joinable())
            m_worker.join();
    }

    // A query waiting for its job (fromJob) hears m_canceled now. The job's
    // lock is taken first, so the flag is seen before the wait or wakes it.
    void wakeWaiter()
    {
        const std::lock_guard jobs(m_jobMutex);
        if (!m_job)
            return;
        { const std::lock_guard lock(m_job->mutex); }
        m_job->changed.notify_all();
    }

    void stopJob()
    {
        std::shared_ptr<Job> job;
        {
            const std::lock_guard lock(m_jobMutex);
            job = std::move(m_job);
        }
        if (job) {
            job->canceled = true;
            if (job->thread.joinable())
                job->thread.join();
        }
    }

    // What is answered before anything is sent: a hint, what to set up.
    std::optional<Items> gate(const winshun::Query& q, const tr::Settings& settings, bool zh) const
    {
        const std::string text = winshun::trim(q.text);
        const bool dictionary = q.command == "dict";
        if (text.empty())
            return Items {dictionary ? Item("hint", zh ? "输入要查的单词" : "Type a word to look up").invalid() : tr::hintRow(zh)};
        if (settings.provider == "none" || (tr::needsKey(settings) && settings.key.empty()))
            return Items {tr::setupRow(zh)};
        if (dictionary && !tr::chatService(settings))
            return Items {Item("setup", zh ? "AI 词典需要 OpenAI 兼容的模型服务" : "AI dictionary needs an OpenAI-compatible model",
                zh ? "Enter 打开翻译设置，选择模型服务" : "Enter opens the translation settings")
                    .withUrl("winshun:settings")};
        if (settings.provider == "doubao" && settings.model.empty())
            return Items {Item("setup", zh ? "请填写方舟模型或推理接入点 ID" : "Enter the Ark model or endpoint ID").withUrl("winshun:settings")};
        return std::nullopt;
    }

    Json respond(const winshun::Query& q, const tr::Settings& settings, bool zh, bool settingsLinks)
    {
        if (auto early = gate(q, settings, zh))
            return itemsAnswer({std::move(*early)});
        // Typed in the launcher, to a chat service: a job that streams.
        if (q.source != "command" && tr::chatService(settings))
            return fromJob(q, settings, zh, settingsLinks);
        return itemsAnswer(once(q, settings, zh, q.source == "command" ? kCommandBudgetMs : kBudgetMs, settingsLinks));
    }

    // The rows for what a service answered (or did not).
    static Outcome conclude(const winshun::Query& q, const tr::Settings& settings, const tr::Response& response, const tr::StreamReader* stream,
        const std::string& text, const std::string& target, bool zh, bool settingsLinks)
    {
        const bool dictionary = q.command == "dict";
        if (response.error) {
            if (tr::notConnected(response) && tr::loopback(settings.endpoint))
                return {{tr::notRunningRow(settings, response.host, zh, settingsLinks)}};
            return {{tr::failedRow(tr::networkError(response, zh), zh)}};
        }
        if (response.status < 200 || response.status > 299) {
            if (const std::string message = tr::localModelMessage(response.body); !message.empty())
                return {{tr::localModelRow(message, zh, settingsLinks)}};
            return {{tr::statusRow(static_cast<long>(response.status), response.body, zh)}};
        }
        const std::size_t lines = dictionary ? 1 : tr::linesOf(text).size();
        tr::Answer answer;
        if (stream && stream->events()) {
            if (!stream->error().empty())
                return {{tr::failedRow(stream->error(), zh)}};
            answer = tr::answerFromText(settings, stream->text(), lines);
        } else {
            answer = tr::parseAnswer(settings, response.body, zh, lines);
        }
        if (!answer.ok)
            return {{tr::failedRow(answer.error, zh)}};
        if (answer.text.empty())
            return {{tr::emptyRow(text, zh)}};
        if (dictionary) {
            auto rows = tr::dictionaryRows(text, answer.text, zh);
            const bool valid = !rows.empty() && rows[0].id == "word";
            return {std::move(rows), valid};
        }
        return {tr::resultRows(text, answer.text, target, answer.detected, zh), true};
    }

    // One request, answered within `budgetMs`.
    Outcome once(const winshun::Query& q, const tr::Settings& settings, bool zh, int budgetMs, bool settingsLinks)
    {
        const std::string text = winshun::trim(q.text);
        const std::string target = tr::targetOf(text, settings.target);
        if (q.source != "command")
            partial(q.queryId, {tr::workingRow(text, zh)});
        const tr::Request request = tr::buildRequest(settings, text, target, q.command == "dict");
        const auto started = Clock::now();
        const tr::Response response = tr::post(request.url, request.body, request.authorization, budgetMs, &m_canceled, request.headers);
        logRequest(settings, started, response);
        if (m_canceled)
            return {};
        return conclude(q, settings, response, nullptr, text, target, zh, settingsLinks);
    }

    void logRequest(const tr::Settings& settings, Clock::time_point started, const tr::Response& response)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        log("info", "request provider=" + settings.provider + " durationMs=" + std::to_string(elapsed) + " status="
                + std::to_string(response.status) + " error=" + std::to_string(response.error));
    }

    // The launcher's query to a chat service: the job for this text (started
    // when there is none), its outcome when it has one within a moment, else
    // what has come so far and `rerun` (the job's refresh() asks sooner).
    Json fromJob(const winshun::Query& q, const tr::Settings& settings, bool zh, bool settingsLinks)
    {
        const std::string text = winshun::trim(q.text);
        const std::string key = q.command + '\n' + text + '\n' + settings.provider + '\n' + settings.endpoint + '\n' + settings.model + '\n'
            + settings.target + '\n' + (zh ? "zh" : "en");
        std::shared_ptr<Job> job;
        std::shared_ptr<Job> old;
        bool fresh = false;
        {
            const std::lock_guard lock(m_jobMutex);
            if (m_job && m_job->key == key && !m_job->canceled && !m_job->stale()) {
                job = m_job;
            } else {
                old = std::move(m_job);
                job = std::make_shared<Job>();
                job->key = key;
                job->text = text;
                job->target = tr::targetOf(text, settings.target);
                job->local = settings.provider == "local";
                m_job = job;
                fresh = true;
            }
        }
        if (old) { // another text now: that one is not wanted any more
            old->canceled = true;
            if (old->thread.joinable())
                old->thread.join();
        }
        if (fresh) {
            partial(q.queryId, {tr::streamingRow(text, {}, job->target, false, zh)});
            // Every owner joins the thread before it lets go of the job.
            job->thread = std::thread([this, raw = job.get(), q, settings, zh, settingsLinks] { runJob(*raw, q, settings, zh, settingsLinks); });
        }

        std::unique_lock lock(job->mutex);
        const int seen = job->version;
        const auto deadline = Clock::now() + (fresh ? kFirstWait : kUpdateWait);
        job->changed.wait_until(lock, deadline, [&] { return job->done || (!fresh && job->version != seen) || m_canceled; });
        if (job->done) {
            // Said once: the same text asked again later (the model started
            // since, say) is asked anew; a translation is in the host's cache.
            Json answer = itemsAnswer(job->outcome);
            lock.unlock();
            {
                const std::lock_guard jobs(m_jobMutex);
                if (m_job == job)
                    m_job.reset();
            }
            if (job->thread.joinable())
                job->thread.join(); // it has ended or is about to
            return answer;
        }
        const bool waiting = job->local && job->sofar.empty() && Clock::now() - job->started > 1500ms;
        Json::Object answer {{"items", winshun::itemsJson({tr::streamingRow(text, job->sofar, job->target, waiting, zh)})}, {"rerun", 2}};
        return Json(std::move(answer));
    }

    void runJob(Job& job, const winshun::Query& q, const tr::Settings& settings, bool zh, bool settingsLinks)
    {
        const bool dictionary = q.command == "dict";
        const tr::Request request = tr::buildRequest(settings, job.text, job.target, dictionary, !dictionary);
        tr::StreamReader stream;
        auto told = Clock::now();
        const auto started = Clock::now();
        const tr::Response response = tr::post(request.url, request.body, request.authorization,
            job.local ? kLocalJobBudgetMs : kJobBudgetMs, &job.canceled, request.headers, [&](std::string_view chunk) {
                stream.feed(chunk);
                bool tell = false;
                {
                    const std::lock_guard lock(job.mutex);
                    if (stream.text() != job.sofar) {
                        job.sofar = stream.text();
                        ++job.version;
                        tell = Clock::now() - told >= 250ms;
                    }
                }
                job.changed.notify_all();
                if (tell) {
                    told = Clock::now();
                    refresh(); // the launcher asks again now
                }
            });
        logRequest(settings, started, response);
        if (job.canceled)
            return;
        Outcome outcome = conclude(q, settings, response, &stream, job.text, job.target, zh, settingsLinks);
        {
            const std::lock_guard lock(job.mutex);
            job.outcome = std::move(outcome);
            job.done = true;
            job.finished = Clock::now();
            ++job.version;
        }
        job.changed.notify_all();
        refresh();
    }

    // ---- 测试连接 (GitHub #495) ----------------------------------------------------------
    // A key, an address and a model typed in, and nothing said whether they
    // work until something was translated: the card has a line and a button
    // (winshun/status). The test sends one word to the service chosen, as a
    // translation is sent, and the line says how it went.

    std::vector<std::pair<std::string, std::string>> testButton(bool zh) const
    {
        return {{"test", zh ? "测试连接" : "Test connection"}};
    }

    // Nothing tested yet for these settings; no line while there is nothing
    // to test (no service chosen, its key missing: the card's rows say so).
    void publishUntried()
    {
        const tr::Settings s = tr::settingsFrom(settings());
        const bool zh = chinese();
        winshun::Query q;
        q.text = kTestWord;
        if (gate(q, s, zh))
            status({});
        else
            status(zh ? "填好后可以测试一下能不能连上" : "Test whether the service can be reached", -1, "idle", testButton(zh));
    }

    void stopTest()
    {
        m_testCanceled = true;
        if (m_tester.joinable())
            m_tester.join();
    }

    void startTest()
    {
        stopTest();
        m_testCanceled = false;
        const tr::Settings s = tr::settingsFrom(settings());
        const bool zh = chinese();
        const bool settingsLinks = links();
        status(zh ? "正在测试连接…" : "Testing the connection…", -1, "busy");
        m_tester = std::thread([this, s, zh, settingsLinks] {
            winshun::Query q;
            q.text = kTestWord;
            q.source = "command";
            Outcome outcome;
            const auto started = Clock::now();
            if (auto early = gate(q, s, zh)) {
                outcome.items = std::move(*early);
            } else {
                const std::string target = tr::targetOf(q.text, s.target);
                const tr::Request request = tr::buildRequest(s, q.text, target);
                const tr::Response response = tr::post(request.url, request.body, request.authorization,
                    s.provider == "local" ? kLocalJobBudgetMs : kCommandBudgetMs, &m_testCanceled, request.headers);
                logRequest(s, started, response);
                if (m_testCanceled)
                    return;
                outcome = conclude(q, s, response, nullptr, q.text, target, zh, settingsLinks);
            }
            if (m_testCanceled)
                return;
            if (outcome.cacheable) { // a translation came
                char seconds[32];
                std::snprintf(seconds, sizeof seconds, "%.1f", std::chrono::duration<double>(Clock::now() - started).count());
                status(zh ? std::string("连接正常，用时 ") + seconds + " 秒" : std::string("Reached in ") + seconds + " s", -1, "ok", testButton(zh));
                return;
            }
            // The row a translation would have shown: what is wrong, and what to do.
            std::string why = outcome.items.empty() ? std::string(zh ? "没有回答" : "No answer") : outcome.items.front().title;
            if (!outcome.items.empty() && !outcome.items.front().subtitle.empty())
                why += (zh ? "：" : ": ") + outcome.items.front().subtitle;
            status((zh ? "没有连上 · " : "Not reached · ") + tr::cut(why, 160), -1, "error", testButton(zh));
        });
    }

    static constexpr const char* kTestWord = "hello";
    std::thread m_tester;
    std::atomic_bool m_testCanceled = false;
    std::thread m_worker;
    std::atomic_bool m_canceled = false;
    long long m_queryId = 0;
    std::mutex m_jobMutex;
    std::shared_ptr<Job> m_job;
};

} // namespace

int main()
{
    return Translate().serve();
}
