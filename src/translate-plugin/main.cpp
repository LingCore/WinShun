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
protected:
    std::string name() const override { return "winshun.translate"; }

    Items query(const winshun::Query& q) override { return run(q).items; }

    Json queryResult(const winshun::Query& q) override
    {
        Outcome o = run(q);
        Json::Object answer {{"items", winshun::itemsJson(o.items)}};
        if (o.cacheable)
            answer["cache"] = 300;
        return answer;
    }

private:
    Outcome run(const winshun::Query& q)
    {
        const bool zh = chinese();
        const std::string text = winshun::trim(q.text);
        const tr::Settings settings = tr::settingsFrom(this->settings());
        if (text.empty())
            return {{tr::hintRow(zh)}};
        if (settings.provider == "none" || settings.key.empty())
            return {{tr::setupRow(zh)}};

        const std::string target = tr::targetOf(text, settings.target);
        partial(q.queryId, {tr::workingRow(text, zh)});

        const tr::Request request = tr::buildRequest(settings, text, target);
        const tr::Response response = tr::post(request.url, request.body, request.authorization, kBudgetMs);
        if (response.error)
            return {{tr::failedRow(tr::networkError(response, zh), zh)}};
        if (response.status < 200 || response.status > 299)
            return {{tr::statusRow(static_cast<long>(response.status), response.body, zh)}};
        const tr::Answer answer = tr::parseAnswer(settings, response.body, zh);
        if (!answer.ok)
            return {{tr::failedRow(answer.error, zh)}};
        if (answer.text.empty())
            return {{tr::emptyRow(text, zh)}};
        return {tr::resultRows(text, answer.text, target, answer.detected, zh), true};
    }
};

} // namespace

int main()
{
    return Translate().serve();
}
