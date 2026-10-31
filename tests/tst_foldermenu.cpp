// Uses an owned window and temporary commands. No global keystrokes, user
// files or shell configuration are changed; foreground is restored afterward.
#include "FolderMenu.h"
#include "Favorites.h"
#include "platform/WindowEffects.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>
#include <QScopeGuard>

using namespace ws;
using namespace Qt::StringLiterals;

class FolderMenuTest : public QObject {
    Q_OBJECT

    static int entry(const FolderMenu& menu, const QString& title)
    {
        const auto entries = menu.entries();
        for (int i = 0; i < entries.size(); ++i) {
            if (entries[i].toMap().value(u"text"_s).toString() == title)
                return i;
        }
        return -1;
    }

private slots:
    void commandsAndSettings()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const HWND previous = ::GetForegroundWindow();
        auto restore = qScopeGuard([previous] {
            if (previous && ::IsWindow(previous))
                ::SetForegroundWindow(previous);
        });
        QWindow source;
        source.setTitle(u"WinShun folder menu test"_s);
        source.resize(320, 160);
        source.show();
        QVERIFY(QTest::qWaitForWindowExposed(&source));
        win::bringToFront(&source);
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), reinterpret_cast<HWND>(source.winId()), 3000);

        Favorites favorites(temporary.filePath(u"favorites.json"_s));
        const QString output = temporary.filePath(u"command-ran.txt"_s);
        QString quotedOutput = QDir::toNativeSeparators(output);
        quotedOutput.replace(u'\'', u"''"_s);
        Favorite command {Favorite::Kind::Command, u"winshun-command:menu-test"_s, u"Test command"_s, u"test"_s,
            u"powershell.exe -NoProfile -NonInteractive -Command \"[IO.File]::WriteAllText('"_s + quotedOutput
                + u"',(Get-Location).Path)\""_s,
            temporary.path()};
        command.silent = true;
        QVERIFY(favorites.add(command));
        QWindow menuWindow;
        FolderMenu menu(nullptr, &favorites);
        menu.setWindow(&menuWindow);
        menu.setRecordHistory(false);
        menu.setGroups(false, false, false);
        QSignalSpy changed(&menu, &FolderMenu::entriesChanged);
        QSignalSpy settings(&menu, &FolderMenu::settingsRequested);
        QSignalSpy notice(&menu, &FolderMenu::notice);

        const auto open = [&] {
            changed.clear();
            menu.hotkeyPressed();
            return changed.wait(5000);
        };
        QVERIFY(open());
        const int settingsIndex = entry(menu, u"Win顺 设置…"_s);
        const int commandIndex = entry(menu, command.name);
        QVERIFY(settingsIndex >= 0);
        QVERIFY(commandIndex >= 0);
        menu.trigger(settingsIndex);
        QCOMPARE(settings.size(), 1);

        QVERIFY(open());
        menu.trigger(entry(menu, command.name));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 10000);
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QDir::cleanPath(QString::fromUtf8(file.readAll())), QDir::cleanPath(temporary.path()));
        QCOMPARE(notice.size(), 0);

        // The visible entry keeps the command's identity, not its old row.
        QVERIFY(open());
        const int removedIndex = entry(menu, command.name);
        QVERIFY(removedIndex >= 0);
        QVERIFY(favorites.remove(command.path));
        menu.trigger(removedIndex);
        QCOMPARE(notice.size(), 1);
        QVERIFY(notice.first().first().toString().contains(u"已移除"));
    }
};

QTEST_MAIN(FolderMenuTest)
#include "tst_foldermenu.moc"
