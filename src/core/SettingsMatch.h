#pragma once

#include "Pinyin.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <vector>

// Finding an option of the settings window by what the user types: the
// words of its title, other words people use for it, the labels on its
// controls, the page and section it is on, its explanation, and what the
// user filled into it (see SettingsSearch in the app). Not a section's
// note: shared by all its options, any word in it would bring them all up.
namespace ws::settingsmatch {

// One word of the query.
struct Term {
    QString text; // case folded
    pinyin::Matcher pinyin; // valid() when the word can be read as pinyin ("kjj" for 快捷键)
    bool single = false; // one ASCII letter or digit: found only where a word starts
};

struct Query {
    std::vector<Term> terms;

    bool isEmpty() const noexcept { return terms.empty(); }
};

// The query as typed, made comparable and split into words: full-width
// letters and digits as ASCII, punctuation ("，", "、", quotes, brackets) as
// spaces, and "Win + V" as "Win+V".
Query parse(const QString& text);

// What an option can be found by.
struct Fields {
    QString title;
    QStringList keywords; // other words for it ("热键" for 快捷键)
    QStringList options; // the labels on its controls ("浅色", "检查更新")
    QStringList context; // its page's title and keywords, its section's title; by pinyin from three letters
    // Word for word only, never by pinyin: in long sentences, pinyin and
    // initials fit almost anything.
    QString description;
    QStringList values; // what the user filled in ("node_modules", "KeePass.exe")
};

struct Span {
    qsizetype start = 0; // in UTF-16 units
    qsizetype length = 0;

    bool operator==(const Span&) const = default;
};

struct Match {
    int score = -1; // -1: a word is in none of the fields; higher is better
    QList<Span> title; // where the words are, in order, merged
    QList<Span> description;
    QList<int> values; // the values a word is in
};

// Every word must be found in some field. Each word scores by its best
// field and how well it fits there (the start of the title best, the
// description least), and the scores add up.
Match match(const Query& query, const Fields& fields);

// `text` for a Text item in StyledText, with `spans` drawn in `color` and
// everything else escaped; line breaks kept.
QString highlight(const QString& text, const QList<Span>& spans, const QString& color);

} // namespace ws::settingsmatch
