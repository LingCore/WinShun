#include "TranslateText.h"

#include "Host.h"

#include <QPointer>
#include <QTimer>

#include <memory>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::plugin {

QJsonObject translationAnswer(bool ok, const Items& items, const QString& error)
{
    if (!ok || items.isEmpty())
        return {{u"state"_s, u"failed"_s}, {u"message"_s, error}};
    const Item& first = items.first();
    if (first.id == u"setup") // not set up, or a key the service refused: what to set, in its words
        return {{u"state"_s, u"service"_s}, {u"message"_s, first.title}};
    if (first.id == u"t" || !first.copy.trimmed().isEmpty())
        return {{u"state"_s, u"ok"_s}, {u"text"_s, first.copy.isEmpty() ? first.title : first.copy}};
    return {{u"state"_s, u"failed"_s},
        {u"message"_s, first.subtitle.isEmpty() || first.id == u"empty" ? first.title : first.title + u": "_s + first.subtitle}};
}

namespace {

// One text on its way through the translators, until one translates.
struct Asking {
    QPointer<Host> host;
    QString text;
    std::vector<Catalog::Translator> translators;
    std::function<void(QJsonObject)> done;
    QJsonObject first; // the first one's answer: what stands when none translates

    static void next(const std::shared_ptr<Asking>& self, std::size_t i)
    {
        if (!self->host) {
            self->done(self->first);
            return;
        }
        const Catalog::Translator& t = self->translators[i];
        self->host->callCommand(t.pluginId, t.commandId, self->text, 4,
            [self, i](bool ok, const Items& items, const QString& error) {
                const QJsonObject answer = translationAnswer(ok, items, error);
                if (i == 0)
                    self->first = answer;
                if (answer.value(u"state"_s) == u"ok"_s)
                    self->done(answer);
                else if (i + 1 < self->translators.size())
                    next(self, i + 1);
                else
                    self->done(self->first);
            });
    }
};

} // namespace

void translateText(Host& host, const QString& text, std::function<void(QJsonObject)> done)
{
    const std::vector<Catalog::Translator> installed = host.catalog().translators();
    auto asking = std::make_shared<Asking>(Asking {&host, text, {}, std::move(done), {}});
    for (const Catalog::Translator& t : installed) {
        if (host.catalog().enabled(t.pluginId))
            asking->translators.push_back(t);
    }
    if (asking->translators.empty()) {
        QJsonObject answer {{u"state"_s, u"plugin"_s}};
        if (!installed.empty())
            answer.insert(u"off"_s, true);
        QTimer::singleShot(0, &host, [asking, answer] { asking->done(answer); });
        return;
    }
    Asking::next(asking, 0);
}

} // namespace ws::plugin
