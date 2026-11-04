#include "Details.h"

using namespace Qt::StringLiterals;

namespace ws::plugin {

namespace {

// CON, PRN, AUX, NUL, COM1–9, LPT1–9 (and the superscript digits Windows
// also takes), CONIN$, CONOUT$: a device under any folder and with any
// extension on Windows 10.
bool reservedName(QStringView component)
{
    QStringView base = component.left(component.indexOf(u'.') < 0 ? component.size() : component.indexOf(u'.'));
    while (base.endsWith(u' '))
        base.chop(1);
    for (const QStringView name : {u"CON", u"PRN", u"AUX", u"NUL", u"CONIN$", u"CONOUT$"}) {
        if (base.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    if (base.size() != 4)
        return false;
    const QStringView stem = base.left(3);
    if (stem.compare(u"COM", Qt::CaseInsensitive) != 0 && stem.compare(u"LPT", Qt::CaseInsensitive) != 0)
        return false;
    const char16_t digit = base[3].unicode();
    return (digit >= u'1' && digit <= u'9') || digit == u'\u00B9' || digit == u'\u00B2' || digit == u'\u00B3';
}

} // namespace

bool plainFilePath(QStringView text)
{
    if (text.size() < 4 || text.size() > 32767)
        return false;
    QString path = text.toString();
    path.replace(u'/', u'\\');
    qsizetype rest = 0; // where the steps after the root begin
    if (path[0].unicode() < 128 && path[0].isLetter() && path[1] == u':' && path[2] == u'\\') {
        rest = 3;
    } else if (path.startsWith(u"\\\\"_s)) {
        const qsizetype server = path.indexOf(u'\\', 2);
        if (server <= 2)
            return false;
        const QStringView host = QStringView(path).mid(2, server - 2);
        if (host == u"?" || host == u"." || host.contains(u':'))
            return false; // \\?\…, \\.\… (devices, raw volumes)
        const qsizetype share = path.indexOf(u'\\', server + 1);
        if (share <= server + 1)
            return false;
        rest = share + 1;
    } else {
        return false;
    }
    if (rest >= path.size() || path.endsWith(u'\\'))
        return false;
    for (qsizetype i = 0; i < path.size(); ++i) {
        const char16_t c = path[i].unicode();
        if (c < 32 || c == u'*' || c == u'?' || c == u'"' || c == u'<' || c == u'>' || c == u'|' || (c == u':' && i != 1))
            return false;
    }
    for (const QStringView step : QStringView(path).mid(rest).split(u'\\')) {
        // Windows drops a step's trailing dots and spaces: "..." is "..",
        // "a. " is "a"; such a step means what it does not say.
        if (step.isEmpty() || step.endsWith(u'.') || step.endsWith(u' ') || reservedName(step))
            return false;
    }
    return true;
}

QString rowFile(const PluginRow& row)
{
    if (row.openKind != PluginRow::OpenKind::Path || !row.openArgs.isEmpty() || !row.openCwd.isEmpty()
        || !plainFilePath(row.openTarget))
        return {};
    return QString(row.openTarget).replace(u'/', u'\\');
}

} // namespace ws::plugin
