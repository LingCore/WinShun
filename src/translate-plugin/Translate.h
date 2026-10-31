// What WinShunTranslate.exe decides and says, without touching the network:
// which language to translate into, what to send to DeepL or to an
// OpenAI-compatible chat endpoint, how to read the answer, and the rows the
// launcher shows. Plain C++ on the plugin SDK so that tests/tst_translate.cpp
// can build it in again; the WinHTTP part is in Http.cpp.
#pragma once

#include "winshun.h"

#include <string>

namespace tr {

struct Settings {
    std::string provider = "none"; // none | deepl | openai (anything else is treated as openai)
    std::string key;
    std::string endpoint; // OpenAI-compatible base, e.g. https://api.openai.com/v1
    std::string model;
    std::string target = "auto"; // auto | zh | en | ja (or any code the service knows)
};

// From the settings Win顺 hands over at initialize (the key comes from the
// Credential Manager). Missing or blank values get their defaults.
Settings settingsFrom(const winshun::Json& json);

// Whether the text has a Chinese, Japanese or Korean character (kana and
// Hangul count: those are not to be translated "into" Chinese either).
bool hasCjk(const std::string& utf8);

// "auto" turns into en for CJK text and zh for the rest; anything else is
// kept (lower-cased).
std::string targetOf(const std::string& text, const std::string& setting);

// "中文" / "Chinese" for zh, and so on; an unknown code is returned as given.
std::string languageName(const std::string& target, bool chinese);

// One POST: where, what, and the Authorization header's value.
struct Request {
    std::string url;
    std::string body;
    std::string authorization;
};
Request buildRequest(const Settings& settings, const std::string& text, const std::string& target);

// What the service said, read.
struct Answer {
    bool ok = false;
    std::string text; // the translation (trimmed); may be empty
    std::string detected; // DeepL only: the language it found
    std::string error; // when !ok: why, in the user's language
};
Answer parseAnswer(const Settings& settings, const std::string& body, bool chinese);

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
winshun::Item statusRow(long status, const std::string& body, bool chinese); // a non-2xx answer
winshun::Item failedRow(const std::string& reason, bool chinese); // no answer, or one that cannot be used
winshun::Item emptyRow(const std::string& text, bool chinese);
winshun::Items resultRows(const std::string& text, const std::string& translated, const std::string& target,
    const std::string& detected, bool chinese);

} // namespace tr
