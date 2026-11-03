// WinShunTranslate.exe: the winshun.translate plugin ("翻译") in the plugin
// library. "tr sentence" translates Chinese to English and anything else to
// Chinese (or into the language the settings name) with the user's own
// DeepL or OpenAI-compatible chat service. The key is the one Win顺 keeps in
// the Credential Manager and hands over in the settings; without a service
// and a key nothing goes online and one row says what to set.
//
// The decisions and the wording are in Translate.cpp, the WinHTTP POST in
// Http.cpp. Win顺 drops a query after 10 s and restarts a plugin after three
// of those in a row, so the whole request gets 9 s (see kBudgetMs).

#include "winshun.h"

#include "Http.h"
#include "Translate.h"

#include <atomic>
#include <chrono>
#include <thread>

using winshun::Item;
using winshun::Items;
using winshun::Json;

namespace {

// Less than the 10 s the host waits: the answer (an error row, at worst) must
// reach it in time.
constexpr int kBudgetMs = 9000;

struct Outcome {
    Items items;
    bool cacheable = false; // only a translation is worth keeping
};

class Translate : public winshun::Plugin {
public:
    ~Translate() override { stop(); }

protected:
    std::string name() const override { return "winshun.translate"; }

    Items query(const winshun::Query& q) override { return run(q, tr::settingsFrom(settings()), chinese()).items; }

    void queryAsync(const winshun::Query& q, std::function<void(Json)> reply) override
    {
        stop();
        m_queryId = q.queryId;
        m_canceled = false;
        const tr::Settings snapshot = tr::settingsFrom(settings());
        const bool zh = chinese();
        m_worker = std::thread([this, q, snapshot, zh, reply = std::move(reply)] {
            Outcome o;
            try {
                o = run(q, snapshot, zh);
            } catch (const std::exception&) {
                o.items = {tr::failedRow(zh ? "无法完成请求" : "Could not complete the request", zh)};
            }
            Json::Object answer {{"items", winshun::itemsJson(o.items)}, {"cache", o.cacheable ? 300 : 0}};
            reply(std::move(answer));
        });
    }

    void cancel(long long queryId) override
    {
        if (queryId == m_queryId)
            m_canceled = true;
    }

    void settingsChanged(const Json&) override { stop(); }
    void shuttingDown() override { stop(); }

private:
    void stop()
    {
        m_canceled = true;
        if (m_worker.joinable())
            m_worker.join();
    }

    Outcome run(const winshun::Query& q, const tr::Settings& settings, bool zh)
    {
        const std::string text = winshun::trim(q.text);
        const bool dictionary = q.command == "dict";
        if (text.empty())
            return {{dictionary ? Item("hint", zh ? "输入要查的单词" : "Type a word to look up").invalid() : tr::hintRow(zh)}};
        if (settings.provider == "none" || settings.key.empty())
            return {{tr::setupRow(zh)}};
        if (dictionary && (settings.provider == "deepl" || settings.provider == "microsoft"))
            return {{Item("setup", zh ? "AI 词典需要 OpenAI 兼容的模型服务" : "AI dictionary needs an OpenAI-compatible model",
                zh ? "Enter 打开翻译设置，选择模型服务" : "Enter opens the translation settings").withUrl("winshun:settings")}};
        if (settings.provider == "doubao" && settings.model.empty())
            return {{Item("setup", zh ? "请填写方舟模型或推理接入点 ID" : "Enter the Ark model or endpoint ID").withUrl("winshun:settings")}};

        const std::string target = tr::targetOf(text, settings.target);
        partial(q.queryId, {tr::workingRow(text, zh)});

        const tr::Request request = tr::buildRequest(settings, text, target, dictionary);
        const auto started = std::chrono::steady_clock::now();
        const tr::Response response = tr::post(request.url, request.body, request.authorization, kBudgetMs, &m_canceled, request.headers);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        log("info", "request provider=" + settings.provider + " durationMs=" + std::to_string(elapsed)
            + " status=" + std::to_string(response.status) + " error=" + std::to_string(response.error));
        if (m_canceled)
            return {};
        if (response.error)
            return {{tr::failedRow(tr::networkError(response, zh), zh)}};
        if (response.status < 200 || response.status > 299)
            return {{tr::statusRow(static_cast<long>(response.status), response.body, zh)}};
        const tr::Answer answer = tr::parseAnswer(settings, response.body, zh);
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

    std::thread m_worker;
    std::atomic_bool m_canceled = false;
    long long m_queryId = 0;
};

} // namespace

int main()
{
    return Translate().serve();
}
