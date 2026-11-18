#pragma once

// The rows winshun.devtools puts in Win顺's search box: for `json` (what was
// typed after it, else the clipboard) and for JSON pasted straight into the
// box. Plain data in, JSON out, so the tests read them without a host.

#include "Json.h"

#include <QJsonArray>
#include <QJsonObject>

namespace ws::devtools {

enum class From {
    Typed, // after the keyword
    Clipboard,
    Pasted, // the whole input looked like JSON (the regex trigger)
    File,
};

// The rows. `full`: the copy rows too; else only the one to look at it, and
// none when it is not JSON (a pasted line that only starts with a brace).
QJsonArray rows(const QString& text, From from, const Reading& reading, bool full, const QString& indent);

// What Enter on a copy row puts on the clipboard: "pretty", "compact",
// "escaped" (the compact JSON as it goes inside a string literal).
QString copied(const Reading& reading, const QString& form, const QString& indent);

QString sizeText(qsizetype bytes); // "3.2 KB"

} // namespace ws::devtools
