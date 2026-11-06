#include "Translate.h"

#include <algorithm>

using winshun::Item;
using winshun::Items;
using winshun::Json;

namespace tr {

namespace {

const char* const kIcon = "glyph:U+E8C1";

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    });
    return s;
}

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return c < 0x80 ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
    });
    return s;
}

bool isDeepl(const Settings& s) { return s.provider == "deepl"; }

// The English name of the language, for the prompt to a chat model.
std::string englishName(const std::string& target)
{
    if (target == "zh")
        return "Chinese";
    if (target == "en")
        return "English";
    if (target == "ja")
        return "Japanese";
    return target;
}

// DeepL's code for a target: ZH, EN, JA, else the upper-cased code.
std::string deeplTarget(const std::string& target) { return upper(target); }

// The next code point of well-formed or broken UTF-8, and how many bytes it
// took (at least 1, so a loop always moves on).
unsigned nextCodePoint(const std::string& s, std::size_t i, std::size_t& length)
{
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    length = 1;
    if (lead < 0x80)
        return lead;
    std::size_t n = 0;
    unsigned cp = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        n = 2;
        cp = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        n = 3;
        cp = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        n = 4;
        cp = lead & 0x07;
    } else
        return 0xFFFD;
    if (i + n > s.size())
        return 0xFFFD;
    for (std::size_t k = 1; k < n; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0) != 0x80)
            return 0xFFFD;
        cp = (cp << 6) | (c & 0x3F);
    }
    length = n;
    return cp;
}

} // namespace

std::string keyField(const std::string& provider)
{
    for (const char* known : {"deepl", "microsoft", "deepseek", "doubao", "hunyuan"})
        if (provider == known)
            return provider + "Key";
    return "openaiKey"; // openai, and anything else spoken to the same way
}

Settings settingsFrom(const Json& json)
{
    Settings s;
    const std::string provider = lower(winshun::trim(json["provider"].toString()));
    if (!provider.empty())
        s.provider = provider;
    // Each service keeps its own key (#10), so switching services and back
    // does not send one service's key to another. apiKey is the one shared
    // key of 2.1.1 and earlier: still used by a service with none of its own.
    s.key = winshun::trim(json[keyField(s.provider)].toString());
    if (s.key.empty())
        s.key = winshun::trim(json["apiKey"].toString());
    s.endpoint = winshun::trim(json["endpoint"].toString());
    s.model = winshun::trim(json["model"].toString());
    s.region = winshun::trim(json["region"].toString());
    if (s.provider == "deepseek") {
        if (s.endpoint.empty())
            s.endpoint = "https://api.deepseek.com";
        if (s.model.empty())
            s.model = "deepseek-flash";
    } else if (s.provider == "doubao") {
        if (s.endpoint.empty())
            s.endpoint = "https://ark.cn-beijing.volces.com/api/v3";
    } else if (s.provider == "hunyuan") {
        if (s.endpoint.empty())
            s.endpoint = "https://api.hunyuan.cloud.tencent.com/v1";
        if (s.model.empty())
            s.model = "hunyuan-turbos-latest";
    }
    const std::string target = lower(winshun::trim(json["target"].toString()));
    if (!target.empty())
        s.target = target;
    return s;
}

bool hasCjk(const std::string& utf8)
{
    for (std::size_t i = 0; i < utf8.size();) {
        std::size_t n = 1;
        const unsigned cp = nextCodePoint(utf8, i, n);
        i += n;
        // Kana, CJK extension A, the unified ideographs, Hangul syllables.
        if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF)
            || (cp >= 0xAC00 && cp <= 0xD7AF))
            return true;
    }
    return false;
}

std::string targetOf(const std::string& text, const std::string& setting)
{
    const std::string target = setting.empty() ? "auto" : lower(setting);
    if (target != "auto")
        return target;
    return hasCjk(text) ? "en" : "zh";
}

std::string languageName(const std::string& target, bool chinese)
{
    if (target == "zh")
        return chinese ? "中文" : "Chinese";
    if (target == "en")
        return chinese ? "英文" : "English";
    if (target == "ja")
        return chinese ? "日文" : "Japanese";
    return target;
}

Request buildRequest(const Settings& settings, const std::string& text, const std::string& target, bool dictionary)
{
    Request r;
    if (settings.provider == "microsoft") {
        std::string base = settings.endpoint.empty() ? "https://api.cognitive.microsofttranslator.com" : settings.endpoint;
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        r.url = base + "/translate?api-version=3.0&to=" + (target == "zh" ? "zh-Hans" : target);
        r.headers["Ocp-Apim-Subscription-Key"] = settings.key;
        if (!settings.region.empty())
            r.headers["Ocp-Apim-Subscription-Region"] = settings.region;
        r.body = Json(Json::Array {Json::Object {{"Text", text}}}).dump();
        return r;
    }
    if (isDeepl(settings)) {
        // The free plan's keys end in ":fx" and live on their own host.
        const bool free = settings.key.size() >= 3 && settings.key.compare(settings.key.size() - 3, 3, ":fx") == 0;
        r.url = std::string("https://") + (free ? "api-free.deepl.com" : "api.deepl.com") + "/v2/translate";
        r.body = Json(Json::Object {{"text", Json::Array {text}}, {"target_lang", deeplTarget(target)}}).dump();
        r.authorization = "DeepL-Auth-Key " + settings.key;
        return r;
    }
    std::string base = settings.endpoint.empty() ? "https://api.openai.com/v1" : settings.endpoint;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    r.url = base + "/chat/completions";
    // The language is named in English: the prompt is English whatever the UI says.
    const std::string prompt = dictionary
        ? "Act as a bilingual dictionary. Explain the user's word in " + englishName(target)
            + ". Return only a JSON object with term (string), phonetic (string, optional), "
              "definitions (up to 4 objects with partOfSpeech and meaning strings), "
              "examples (up to 2 objects with text and translation strings). "
              "For an unknown word return empty definitions. Do not invent a meaning."
        : "Translate the following text into " + englishName(target)
            + ". Reply with the translation only, no explanations.";
    r.body = Json(Json::Object {
                      {"model", settings.model.empty() ? "gpt-4o-mini" : settings.model},
                      {"temperature", 0},
                      {"messages",
                          Json::Array {Json::Object {{"role", "system"}, {"content", prompt}},
                              Json::Object {{"role", "user"}, {"content", text}}}},
                  })
                 .dump();
    r.authorization = "Bearer " + settings.key;
    return r;
}

Answer parseAnswer(const Settings& settings, const std::string& body, bool chinese)
{
    Answer a;
    std::string problem;
    const auto json = Json::parse(body, &problem);
    if (!json || (!json->isObject() && !(settings.provider == "microsoft" && json->isArray()))) {
        a.error = chinese ? "服务的回答看不懂（不是 JSON）" : "The service's answer is not readable (not JSON)";
        return a;
    }
    if (settings.provider == "microsoft") {
        const auto& results = json->toArray();
        if (results.empty() || results[0]["translations"].toArray().empty()) {
            a.error = chinese ? "回答里没有译文" : "No translation in the answer";
            return a;
        }
        a.text = winshun::trim(results[0]["translations"].toArray()[0]["text"].toString());
        a.detected = results[0]["detectedLanguage"]["language"].toString();
    } else if (isDeepl(settings)) {
        const Json::Array& translations = (*json)["translations"].toArray();
        if (translations.empty()) {
            a.error = chinese ? "回答里没有译文" : "No translation in the answer";
            return a;
        }
        a.text = winshun::trim(translations[0]["text"].toString());
        a.detected = translations[0]["detected_source_language"].toString();
    } else {
        const Json::Array& choices = (*json)["choices"].toArray();
        if (choices.empty()) {
            a.error = chinese ? "回答里没有译文（不是 OpenAI 兼容的 chat/completions？）"
                              : "No translation in the answer (not OpenAI-compatible chat/completions?)";
            return a;
        }
        a.text = winshun::trim(choices[0]["message"]["content"].toString());
    }
    a.ok = true;
    return a;
}

std::string serviceMessage(const std::string& body)
{
    const auto json = Json::parse(body);
    if (!json)
        return body;
    const Json& error = (*json)["error"];
    if (error.isString())
        return error.toString();
    if (error["message"].isString())
        return error["message"].toString();
    if ((*json)["message"].isString())
        return (*json)["message"].toString();
    return body;
}

std::string cut(const std::string& utf8, std::size_t limit)
{
    std::size_t i = 0;
    for (std::size_t count = 0; i < utf8.size(); ++count) {
        if (count == limit)
            return utf8.substr(0, i);
        std::size_t n = 1;
        nextCodePoint(utf8, i, n);
        i += n;
    }
    return utf8;
}

winshun::Item hintRow(bool chinese)
{
    return Item("hint", chinese ? "输入要翻译的句子" : "Type a sentence to translate",
        chinese ? "中文译成英文，其他译成中文；Enter 复制译文" : "Chinese to English, anything else to Chinese; Enter copies")
        .withIcon(kIcon)
        .invalid();
}

winshun::Item setupRow(bool chinese)
{
    // Enter opens Win顺's settings at this plugin's card ("winshun:settings"
    // is Win顺's own, never handed to the browser).
    return Item("setup", chinese ? "还没有设置翻译服务" : "No translation service set up",
        chinese ? "Enter 打开设置 → 插件 → 翻译：选服务，填这个服务的密钥；没填之前不联网"
                : "Enter opens Settings → Plugins → Translate: pick a service and enter its key; nothing is sent until then")
        .withIcon(kIcon)
        .withUrl("winshun:settings");
}

winshun::Item workingRow(const std::string& text, bool chinese)
{
    return Item("working", chinese ? "翻译中…" : "Translating…", text).withIcon(kIcon).invalid();
}

winshun::Item statusRow(long status, const std::string& body, bool chinese)
{
    return Item("error", (chinese ? "接口返回 " : "The service answered ") + std::to_string(status), cut(serviceMessage(body)))
        .withIcon(kIcon)
        .invalid();
}

winshun::Item failedRow(const std::string& reason, bool chinese)
{
    return Item("error", chinese ? "翻译失败" : "Translation failed", cut(reason)).withIcon(kIcon).invalid();
}

winshun::Item emptyRow(const std::string& text, bool chinese)
{
    return Item("empty", chinese ? "没有译文" : "No translation came back", text).withIcon(kIcon).invalid();
}

winshun::Items dictionaryRows(const std::string& text, const std::string& body, bool chinese)
{
    std::string content = winshun::trim(body);
    if (content.starts_with("```")) {
        const auto begin = content.find('\n');
        const auto end = content.rfind("```");
        if (begin != std::string::npos && end > begin)
            content = content.substr(begin + 1, end - begin - 1);
    }
    const auto json = Json::parse(content);
    if (!json || !json->isObject() || (*json)["definitions"].toArray().empty())
        return {failedRow(chinese ? "没有可用的词典释义" : "No usable dictionary definition", chinese)};
    std::string term = (*json)["term"].toString();
    if (term.empty())
        term = text;
    const std::string phonetic = (*json)["phonetic"].toString();
    winshun::Items rows {Item("word", cut(term + (phonetic.empty() ? "" : "  " + phonetic)),
        chinese ? "AI 词典 · Enter 复制单词" : "AI dictionary · Enter copies the word").withIcon(kIcon).withCopy(term)};
    for (const auto& definition : (*json)["definitions"].toArray()) {
        const std::string meaning = winshun::trim(definition["meaning"].toString());
        if (!meaning.empty())
            rows.push_back(Item("definition" + std::to_string(rows.size()), cut(meaning), cut(definition["partOfSpeech"].toString()))
                .withIcon(kIcon).withCopy(meaning));
        if (rows.size() >= 5)
            break;
    }
    if (rows.size() == 1)
        return {failedRow(chinese ? "没有可用的词典释义" : "No usable dictionary definition", chinese)};
    int examples = 0;
    for (const auto& example : (*json)["examples"].toArray()) {
        const std::string sentence = winshun::trim(example["text"].toString());
        if (!sentence.empty()) {
            rows.push_back(Item("example" + std::to_string(++examples), cut(sentence), cut(example["translation"].toString()))
                .withIcon(kIcon).withCopy(sentence));
            if (examples == 2)
                break;
        }
    }
    return rows;
}

winshun::Items resultRows(const std::string& text, const std::string& translated, const std::string& target,
    const std::string& detected, bool chinese)
{
    const std::string subtitle = "→ " + languageName(target, chinese) + (detected.empty() ? "" : " · " + detected) + " · "
        + (chinese ? "Enter 复制" : "Enter copies");
    return {Item("t", translated, subtitle).withIcon(kIcon).withCopy(translated).withScore(90),
        Item("source", text, chinese ? "原文" : "source").withIcon(kIcon).withCopy(text)};
}

} // namespace tr
