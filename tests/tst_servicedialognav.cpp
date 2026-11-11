// The request that takes an administrator's file dialog to a folder (GitHub
// #104), as Win顺's UI makes it: with no service, and, against an isolated
// SYSTEM service and a real administrator's dialog of the Windows XP style
// (GitHub #143), the navigation itself. The elevated harness sets the
// variables the live part waits for
// (docs/validation/issue143-20261111/harness); without them it is skipped.

#include "platform/ServiceDialogNav.h"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTest>

#include <windows.h>

#include <atomic>
#include <future>
#include <memory>
#include <optional>

using namespace ws;
using namespace std::chrono_literals;

namespace {
// navigate()'s answer, waited for: nothing when none came in `limit`.
std::optional<bool> navigated(ServiceDialogNav& nav, HWND dialog, const std::wstring& folder, std::chrono::seconds limit)
{
    auto answer = std::make_shared<std::promise<bool>>();
    std::future<bool> done = answer->get_future();
    nav.navigate(dialog, folder, [answer](bool ok) { answer->set_value(ok); });
    if (done.wait_for(limit) != std::future_status::ready)
        return std::nullopt;
    return done.get();
}
} // namespace

class ServiceDialogNavTest : public QObject {
    Q_OBJECT

private slots:
    // No service on the pipe: said so, and a request is answered "not done"
    // at once, so the bar keeps its copy-and-paste way.
    void noService()
    {
        const QByteArray before = qgetenv("WINSHUN_PIPE");
        qputenv("WINSHUN_PIPE", "WinShun.NoSuchPipe.tst_servicedialognav");
        const auto restore = qScopeGuard([&before] {
            if (before.isEmpty())
                qunsetenv("WINSHUN_PIPE");
            else
                qputenv("WINSHUN_PIPE", before);
        });
        ServiceDialogNav nav({});
        QElapsedTimer timer;
        timer.start();
        const std::optional<bool> went = navigated(nav, ::GetDesktopWindow(), L"C:\\", 5s);
        QVERIFY(went.has_value());
        QVERIFY(!*went);
        QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(nav.reach(), ServiceDialogNav::Reach::NoService);
        QVERIFY(!nav.available());
    }

    // Live: WINSHUN_PIPE names an isolated SYSTEM service that takes this
    // program for Win顺's UI; WINSHUN_NAV_DIALOG is an administrator's file
    // dialog, WINSHUN_NAV_FOLDER a folder to take it to. Where the dialog
    // then is, and what its file name box holds, the harness reads (it has
    // the dialog's rights; this program has not).
    void administratorDialogLive()
    {
        bool ok = false;
        const HWND dialog = reinterpret_cast<HWND>(static_cast<quintptr>(qEnvironmentVariable("WINSHUN_NAV_DIALOG").toULongLong(&ok)));
        const std::wstring folder = qEnvironmentVariable("WINSHUN_NAV_FOLDER").toStdWString();
        if (!ok || !dialog || folder.empty() || qEnvironmentVariableIsEmpty("WINSHUN_PIPE"))
            QSKIP("needs the elevated harness (an isolated service and an administrator's file dialog)");
        QVERIFY(::IsWindow(dialog));

        std::atomic<int> reach {-1};
        ServiceDialogNav nav([&reach](ServiceDialogNav::Reach now) { reach = static_cast<int>(now); });
        QTRY_COMPARE_WITH_TIMEOUT(reach.load(), static_cast<int>(ServiceDialogNav::Reach::Available), 10'000);

        // A folder that is not there: refused, and the dialog left as it is.
        std::optional<bool> went = navigated(nav, dialog, folder + L"\\not here", 20s);
        QVERIFY(went.has_value());
        QVERIFY(!*went);
        QVERIFY(::IsWindow(dialog));

        went = navigated(nav, dialog, folder, 20s);
        QVERIFY(went.has_value());
        QVERIFY(*went);
        QVERIFY(::IsWindow(dialog)); // gone there, not answered
    }
};

QTEST_APPLESS_MAIN(ServiceDialogNavTest)
#include "tst_servicedialognav.moc"
