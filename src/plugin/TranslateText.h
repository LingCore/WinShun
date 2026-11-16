#pragma once

// A text translated by the plugins that say they translate (GitHub #191,
// #340): what translate_text answers the plugin that asked (the capture's
// OCR window and its pins).

#include "Items.h"

#include <QJsonObject>
#include <QString>

#include <functional>

namespace ws::plugin {

class Host;

// What a translate plugin's rows say: the row "t", or any first row with
// copy text, is the translation (its copy text whole, the title may be cut
// for showing); "setup" asks for a service; anything else says what went
// wrong. {"state": ok + "text" | service | failed + "message"}.
QJsonObject translationAnswer(bool ok, const Items& items, const QString& error);

// `text` to the translate plugins that are on (Catalog::translators), in
// turn until one translates: one not set up, or failing, hands over to the
// next, and when none can, the first one's answer stands. None on:
// {"state": "plugin"}, with "off": true when one is installed. `done` is
// called once.
void translateText(Host& host, const QString& text, std::function<void(QJsonObject)> done);

} // namespace ws::plugin
