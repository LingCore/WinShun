// WinShunDelta: delta update packages (src/app/UpdateDelta.h) for
// scripts/release.ps1. Not shipped.
//
//   WinShunDelta make  --old <dir> --new <dir> --from <ver> --to <ver> [--setup <delta setup.exe>] --out <zip>
//   WinShunDelta apply --package <zip> --base <dir> --out <dir> [--setup <exe>] [--skip <path>]...
//       (Win顺's own code: what the update will do on a user's computer)
//   WinShunDelta line  <zip>...
//       the hidden line for the release notes, from the packages' names, sizes and SHA-256

#include "UpdateDelta.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include <cstdio>

using namespace Qt::StringLiterals;
namespace delta = ws::update::delta;

namespace {

int usage()
{
    std::fputs("usage: WinShunDelta make|apply|line ... (see tools/delta/main.cpp)\n", stderr);
    return 2;
}

QString option(const QStringList& args, const QString& name)
{
    const qsizetype i = args.indexOf(name);
    return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString();
}

QStringList options(const QStringList& args, const QString& name)
{
    QStringList list;
    for (qsizetype i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name)
            list.append(args[i + 1]);
    return list;
}

int fail(const QString& why)
{
    std::fprintf(stderr, "WinShunDelta: %s\n", why.toUtf8().constData());
    return 1;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (args.isEmpty())
        return usage();
    const QString command = args.first();
    QTextStream out(stdout);

    if (command == u"make"_s) {
        const QString oldDir = option(args, u"--old"_s), newDir = option(args, u"--new"_s), from = option(args, u"--from"_s),
                      to = option(args, u"--to"_s), zip = option(args, u"--out"_s);
        if (oldDir.isEmpty() || newDir.isEmpty() || from.isEmpty() || to.isEmpty() || zip.isEmpty())
            return usage();
        delta::MadeStats stats;
        if (const QString why = delta::make(oldDir, newDir, from, to, option(args, u"--setup"_s), zip, &stats); !why.isEmpty())
            return fail(why);
        out << u"same %1, patched %2, new %3, %4 bytes\n"_s.arg(stats.same).arg(stats.patched).arg(stats.added).arg(stats.bytes);
        return 0;
    }

    if (command == u"apply"_s) {
        const QString package = option(args, u"--package"_s), base = option(args, u"--base"_s), target = option(args, u"--out"_s);
        if (package.isEmpty() || base.isEmpty() || target.isEmpty())
            return usage();
        QFile file(package);
        if (!file.open(QIODevice::ReadOnly))
            return fail(u"cannot read "_s + package);
        delta::ApplyOptions opts;
        opts.setupTarget = option(args, u"--setup"_s);
        opts.skip = options(args, u"--skip"_s);
        if (const QString why = delta::apply(file.readAll(), base, target, opts); !why.isEmpty())
            return fail(why);
        out << "applied\n";
        return 0;
    }

    if (command == u"line"_s) {
        static const QRegularExpression name(u"^WinShun-(\\d+\\.\\d+\\.\\d+)-x64-delta-from-(\\d+\\.\\d+\\.\\d+)\\.zip$"_s);
        QList<delta::Offered> offered;
        for (const QString& path : args.mid(1)) {
            const QFileInfo info(path);
            const QRegularExpressionMatch match = name.match(info.fileName());
            if (!match.hasMatch())
                return fail(u"not a delta package name: "_s + info.fileName());
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return fail(u"cannot read "_s + path);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(&file);
            offered.append({match.captured(2), info.fileName(), info.size(), QString::fromLatin1(hash.result().toHex())});
        }
        if (offered.isEmpty())
            return usage();
        out << delta::notesLine(offered) << '\n';
        return 0;
    }
    return usage();
}
