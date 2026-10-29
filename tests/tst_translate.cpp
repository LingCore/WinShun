// The logic of the translate plugin (src/translate-plugin/Translate): which
// language to translate into, what is sent to DeepL and to an OpenAI-
// compatible endpoint, how the answers and the services' errors are read,
// and the rows in both languages. No network.

#include "Translate.h"

#include <QTest>

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

} // namespace

class TestTranslate : public QObject {
    Q_OBJECT

private slots:
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
        QCOMPARE(body["text"].toArray().size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(body["text"].toArray()[0].toString()), QString::fromUtf8("say \"hi\"\nnow 你好"));
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
        Json body = parse(tr::buildRequest(s, "He said \"go\"\\n\n\tok 你好 😀", "en").body);
        QCOMPARE(QString::fromStdString(body["model"].toString()), QString::fromUtf8("gpt-4o-mini"));
        QCOMPARE(body["temperature"].toNumber(-1), 0.0);
        const Json::Array& messages = body["messages"].toArray();
        QCOMPARE(messages.size(), std::size_t(2));
        QCOMPARE(QString::fromStdString(messages[0]["role"].toString()), QString::fromUtf8("system"));
        QCOMPARE(QString::fromStdString(messages[0]["content"].toString()),
            QString::fromUtf8("Translate the following text into English. Reply with the translation only, no explanations."));
        QCOMPARE(QString::fromStdString(messages[1]["role"].toString()), QString::fromUtf8("user"));
        QCOMPARE(QString::fromStdString(messages[1]["content"].toString()), QString::fromUtf8("He said \"go\"\\n\n\tok 你好 😀"));

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
        const winshun::Item a = tr::statusRow(401, "{\"error\":{\"message\":\"" + said + "\"}}", false);
        QCOMPARE(QString::fromStdString(a.title), QString::fromUtf8("The service answered 401"));
        QCOMPARE(a.subtitle.size(), std::size_t(200));
        QVERIFY(!a.valid);
        std::string han;
        for (int i = 0; i < 250; ++i)
            han += "\xE6\xB5\x8B";
        const winshun::Item b = tr::statusRow(500, han, true);
        QCOMPARE(QString::fromStdString(b.title), QString::fromUtf8("接口返回 500"));
        QVERIFY(validUtf8(b.subtitle));
        QCOMPARE(b.subtitle.size(), std::size_t(600));
        // The body of a failing answer that is not JSON.
        QCOMPARE(QString::fromStdString(tr::statusRow(502, "Bad Gateway", true).subtitle), QString::fromUtf8("Bad Gateway"));
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
