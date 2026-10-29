#include "ProxyAddress.h"

#include <QStringList>

#include <algorithm>

namespace ws::proxy {

namespace {

using namespace Qt::StringLiterals;

// Letters, digits and hyphens in dot-separated parts of 1 to 63 (an
// internationalised name is its xn-- form), 253 in all.
bool validHostName(const QString& host)
{
    if (host.isEmpty() || host.size() > 253)
        return false;
    for (const QString& label : host.split(u'.')) {
        if (label.isEmpty() || label.size() > 63 || label.startsWith(u'-') || label.endsWith(u'-'))
            return false;
        for (const QChar c : label) {
            const bool letter = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
            if (!letter && !(c >= u'0' && c <= u'9') && c != u'-' && c != u'_')
                return false;
        }
    }
    return true;
}

// Hex digits, colons and an IPv4 tail: at least two colons, no more than seven.
bool validIpv6(const QString& host)
{
    const qsizetype colons = host.count(u':');
    if (colons < 2 || colons > 7)
        return false;
    return std::all_of(host.begin(), host.end(), [](QChar c) {
        return c == u':' || c == u'.' || (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
    });
}

} // namespace

Address parse(const QString& text)
{
    QString rest = text.trimmed();
    if (rest.isEmpty())
        return {Problem::Empty, {}};
    if (const qsizetype scheme = rest.indexOf(u"://"_s); scheme >= 0) {
        const QString name = rest.left(scheme).toLower();
        if (name.startsWith(u"socks"))
            return {Problem::Socks, {}};
        if (name != u"http")
            return {Problem::OtherScheme, {}};
        rest = rest.mid(scheme + 3);
    }
    while (rest.endsWith(u'/'))
        rest.chop(1);
    if (rest.contains(u'@'))
        return {Problem::Credentials, {}};
    if (rest.isEmpty() || rest.contains(u'/') || rest.contains(u' ') || rest.contains(u'?') || rest.contains(u'#'))
        return {Problem::BadAddress, {}};

    QString host;
    QString port;
    bool v6 = false;
    if (rest.startsWith(u'[')) { // [::1]:7890
        const qsizetype close = rest.indexOf(u']');
        if (close < 0)
            return {Problem::BadAddress, {}};
        host = rest.mid(1, close - 1);
        v6 = true;
        const QString after = rest.mid(close + 1);
        if (after.isEmpty())
            return {Problem::NoPort, {}};
        if (!after.startsWith(u':'))
            return {Problem::BadAddress, {}};
        port = after.mid(1);
    } else {
        const qsizetype colon = rest.indexOf(u':');
        if (colon < 0) {
            host = rest;
        } else {
            if (rest.indexOf(u':', colon + 1) >= 0) // more than one: a bare IPv6 address, which needs its brackets
                return {Problem::BadAddress, {}};
            host = rest.left(colon);
            port = rest.mid(colon + 1);
        }
    }
    if (v6 ? !validIpv6(host) : !validHostName(host))
        return {Problem::BadAddress, {}};
    if (port.isEmpty())
        return {Problem::NoPort, {}};
    bool number = false;
    const int portNumber = port.size() <= 5 ? port.toInt(&number) : 0;
    if (!number || portNumber < 1 || portNumber > 65535 || port.startsWith(u'+') || port.startsWith(u'-'))
        return {Problem::BadPort, {}};
    return {Problem::None, (v6 ? u'[' + host + u']' : host.toLower()) + u':' + QString::number(portNumber)};
}

} // namespace ws::proxy
