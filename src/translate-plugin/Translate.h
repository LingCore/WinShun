// What WinShunTranslate.exe decides and says, without touching the network:
// which language to translate into, what to send to DeepL or to an
// OpenAI-compatible chat endpoint, how to read the answer, and the rows the
// launcher shows. Plain C++ on the plugin SDK so that tests/tst_translate.cpp
// can build it in again; the WinHTTP part is in Http.cpp.
#pragma once

#include "winshun.h"

#include <string>
#include <string_view>
#include <vector>

namespace tr {

// The local models plugin's address (winshun.localmodel, port 39281 unless
// its settings say otherwise).
inline constexpr const char* kLocalEndpoint = "http://127.0.0.1:39281/v1";

struct Settings {
    // none | deepl | microsoft | local (Win顺's local models) | openai (and
    // anything else: spoken to as OpenAI-compatible, as are deepseek, doubao, hunyuan)
    std::string provider = "none";
    std::string key;
    std::string endpoint; // OpenAI-compatible base, e.g. https://api.openai.com/v1
    std::string model;
    std::string region; // Microsoft Translator's resource region, when required
    std::string target = "auto"; // auto | zh | en | ja (or any code the service knows)
};

// The setting that holds this service's own key: deeplKey, microsoftKey,
// deepseekKey, doubaoKey, hunyuanKey, or openaiKey for any other.
std::string keyField(const std::string& provider);

// From the settings Win顺 hands over at initialize (the keys come from the
// Credential Manager). Missing or blank values get their defaults; a service
// without a key of its own uses the shared apiKey of 2.1.1 and earlier.
Settings settingsFrom(const winshun::Json& json);

// Whether a URL names this computer (127.0.0.1, localhost, [::1]): Win顺's
// local models, Ollama, LM Studio. Such a service needs no key and is reached
// without the proxy.
bool loopback(const std::string& url);
// Whether nothing may be sent before a key is set.
bool needsKey(const Settings& settings);
// A chat-completions service (not DeepL or Microsoft): its answer can stream.
bool chatService(const Settings& settings);

// Whether the text has a Chinese, Japanese or Korean character (kana and
// Hangul count: those are not to be translated "into" Chinese either).
bool hasCjk(const std::string& utf8);

// "auto" turns into en for CJK text and zh for the rest; anything else is
// kept (lower-cased).
std::string targetOf(const std::string& text, const std::string& setting);

// "中文" / "Chinese" for zh, and so on; an unknown code is returned as given.
std::string languageName(const std::string& target, bool chinese);

// The text's lines, trimmed, the empty ones left out. More than one (the
// blocks of a screenshot, a paragraph each: Win顺's translate_text) are
// translated each on its own and come back a line each, in the same order:
// DeepL and Microsoft take them as an array, a chat model gets a JSON array
// and is asked for one of the same length (parseAnswer reads it; an answer
// that is not such an array is taken as the plain text it is).
std::vector<std::string> linesOf(const std::string& text);

// One POST: where, what, and the Authorization header's value.
struct Request {
    std::string url;
    std::string body;
    std::string authorization;
    std::map<std::string, std::string> headers;
};
// `stream`: the answer as server-sent events, to show it as it comes.
Request buildRequest(const Settings& settings, const std::string& text, const std::string& target, bool dictionary = false,
    bool stream = false);

// What the service said, read.
struct Answer {
    bool ok = false;
    std::string text; // the translation (trimmed); may be empty
    std::string detected; // DeepL only: the language it found
    std::string error; // when !ok: why, in the user's language
};
// `sentLines`: how many lines buildRequest sent each on its own (linesOf).
Answer parseAnswer(const Settings& settings, const std::string& body, bool chinese, std::size_t sentLines = 1);
// A chat model's reply, read as parseAnswer reads one: a JSON array (or, for
// the local models, numbered lines) back into a line each.
Answer answerFromText(const Settings& settings, const std::string& content, std::size_t sentLines = 1);

// A streamed answer (server-sent events of chat completions), fed as it
// comes: the reply so far. A body that is no stream gets no events (and is
// read whole with parseAnswer).
class StreamReader {
public:
    void feed(std::string_view chunk);
    const std::string& text() const { return m_text; }
    bool events() const { return m_events; } // at least one "data:" event came
    bool done() const { return m_done; } // "data: [DONE]" came
    const std::string& error() const { return m_error; } // an error event's message

private:
    void line(std::string_view line);
    std::string m_pending; // a line not ended yet
    std::string m_text;
    std::string m_error;
    bool m_events = false;
    bool m_done = false;
};

// The service's own words from an error body: "error" as a string,
// "error.message", or "message"; otherwise the body itself. Not cut.
std::string serviceMessage(const std::string& body);

// At most `limit` code points, cut between characters (a byte cut could put
// invalid UTF-8 into the JSON line, which the host drops whole).
std::string cut(const std::string& utf8, std::size_t limit = 200);

// ---- the rows ---------------------------------------------------------------

winshun::Item hintRow(bool chinese);
winshun::Item setupRow(bool chinese);
winshun::Item workingRow(const std::string& text, bool chinese);
// A non-2xx answer, said plainly with the status after it. 401 and 403 (a key
// wrong, expired or not allowed this model) are a "setup" row whose Enter
// opens the settings: translate_text then tells the screenshot's Translate
// to set it up, and tries the next translate plugin (GitHub #444).
winshun::Item statusRow(long status, const std::string& body, bool chinese);
winshun::Item failedRow(const std::string& reason, bool chinese); // no answer, or one that cannot be used
// The local models' own answer that they cannot (503 with type
// "winshun_local_model": no model yet, still downloading, failed to load) or
// nothing listening there: a "setup" row whose Enter opens the local models'
// card (`settingsLinks`: this Win顺 opens another plugin's card), else
// Translate's own.
winshun::Item localModelRow(const std::string& message, bool chinese, bool settingsLinks);
std::string localModelMessage(const std::string& body); // empty: not the local models' error
// Nothing answers at a loopback address (connection refused): which program to start.
winshun::Item notRunningRow(const Settings& settings, const std::string& host, bool chinese, bool settingsLinks);
// While a streamed answer comes: what came so far (or "Translating…").
// `waiting`: nothing has come for a while from a local model (it is loading).
winshun::Item streamingRow(const std::string& text, const std::string& sofar, const std::string& target, bool waiting, bool chinese);
winshun::Item emptyRow(const std::string& text, bool chinese);
winshun::Items resultRows(const std::string& text, const std::string& translated, const std::string& target,
    const std::string& detected, bool chinese);
winshun::Items dictionaryRows(const std::string& text, const std::string& body, bool chinese);

} // namespace tr
