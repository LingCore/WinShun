// Explorer's 贴到屏幕（Win顺）on a picture file (src/app/platform/ShellMenu):
// where it is written, and what an update or turning it off leaves behind.
// HKCU of this process is a key under the real one: nothing of the user's
// verbs is read or changed.
#include "PictureFile.h"
#include "platform/ShellMenu.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <QtTest>

#include <windows.h>

#include <string>

using namespace Qt::StringLiterals;

namespace {

constexpr const wchar_t* kClasses = L"Software\\Classes\\";

bool exists(const std::wstring& key)
{
    HKEY handle = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, (kClasses + key).c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS)
        return false;
    ::RegCloseKey(handle);
    return true;
}

QString value(const std::wstring& key, const wchar_t* name)
{
    wchar_t text[1024] {};
    DWORD bytes = sizeof text;
    if (::RegGetValueW(HKEY_CURRENT_USER, (kClasses + key).c_str(), name, RRF_RT_REG_SZ, nullptr, text, &bytes) != ERROR_SUCCESS)
        return QString();
    return QString::fromWCharArray(text);
}

bool write(const std::wstring& key, const wchar_t* name, const QString& text)
{
    const std::wstring wide = text.toStdWString();
    return ::RegSetKeyValueW(HKEY_CURRENT_USER, (kClasses + key).c_str(), name, REG_SZ, wide.c_str(),
               static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)))
        == ERROR_SUCCESS;
}

std::wstring verbOf(QStringView extension)
{
    return L"SystemFileAssociations\\." + extension.toString().toStdWString() + L"\\shell\\WinShun.PinPicture";
}

const std::wstring kAllFiles = L"*\\shell\\WinShun.PinPicture"; // 0.45.0
const std::wstring kImages = L"SystemFileAssociations\\image\\shell\\WinShun.PinPicture"; // up to 0.44.0

} // namespace

class ShellMenuTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_stub = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/WinShunSearch.exe"_s);
        if (!QFileInfo::exists(m_stub))
            QSKIP("No WinShunSearch.exe beside the test: the verbs are only written for a copy that has it");
        m_scratch = L"Software\\WinShunTest\\" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString();
        QVERIFY(::RegCreateKeyExW(HKEY_CURRENT_USER, m_scratch.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &m_root, nullptr)
            == ERROR_SUCCESS);
        QCOMPARE(::RegOverridePredefKey(HKEY_CURRENT_USER, m_root), static_cast<LONG>(ERROR_SUCCESS));
        m_redirected = true;
        QVERIFY(!exists(L"")); // the scratch key, not the user's classes
    }

    void cleanupTestCase()
    {
        if (m_redirected)
            ::RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (m_root)
            ::RegCloseKey(m_root);
        if (!m_scratch.empty()) {
            ::RegDeleteTreeW(HKEY_CURRENT_USER, m_scratch.c_str());
            ::RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\WinShunTest"); // only if no other run is in it
        }
    }

    // On: a verb for each picture extension, beside the file type's own verbs
    // (the top of Explorer's menu), run once a file; the ones this copy wrote
    // where earlier versions had them are gone, so that the menu shows one.
    void onEachPictureExtension()
    {
        const QString command = u"\"%1\" --pin-picture \"%2\""_s.arg(m_stub, u"%1"_s);
        for (const std::wstring& legacy : {kAllFiles, kImages}) {
            QVERIFY(write(legacy, L"MUIVerb", u"贴到屏幕（Win顺）"_s));
            QVERIFY(write(legacy + L"\\command", nullptr, command));
        }
        // Another program's, under the same keys: they stay.
        QVERIFY(write(L"*\\shell\\Other.Program\\command", nullptr, u"other.exe \"%1\""_s));
        QVERIFY(write(L"SystemFileAssociations\\.png\\ShellEx\\ContextMenuHandlers\\Other", nullptr, u"{00000000-0000-0000-0000-000000000000}"_s));

        ws::shellmenu::applyPinPicture(true);

        for (const QStringView extension : ws::kPinnablePictureExtensions) {
            const std::wstring verb = verbOf(extension);
            QVERIFY2(!value(verb, L"MUIVerb").isEmpty(), qPrintable(extension.toString()));
            QCOMPARE(value(verb, L"MultiSelectModel"), u"Document"_s);
            QCOMPARE(value(verb + L"\\command", nullptr), command);
            QVERIFY(value(verb, L"Icon").endsWith(u"\\WinShun.exe,0"));
            QVERIFY(value(verb, L"AppliesTo").isEmpty()); // on its own types already
        }
        QVERIFY(!exists(kAllFiles));
        QVERIFY(!exists(kImages));
        QVERIFY2(!exists(L"SystemFileAssociations\\image"), "an empty key left where the verb was");
        QVERIFY(exists(L"*\\shell\\Other.Program"));
    }

    // Off: this copy's verbs go, and the keys that held nothing else; what
    // another program has there stays.
    void offLeavesNothingOfOurs()
    {
        ws::shellmenu::applyPinPicture(false);
        for (const QStringView extension : ws::kPinnablePictureExtensions)
            QVERIFY2(!exists(verbOf(extension)), qPrintable(extension.toString()));
        QVERIFY2(!exists(L"SystemFileAssociations\\.webp"), "an empty key left where the verb was");
        QVERIFY(!exists(L"SystemFileAssociations\\.png\\shell"));
        QVERIFY(exists(L"SystemFileAssociations\\.png\\ShellEx\\ContextMenuHandlers\\Other"));
        QVERIFY(exists(L"*\\shell\\Other.Program"));
    }

    // Another copy's verb (a portable one elsewhere, still there) is not this
    // copy's to remove, where it is now or where it was.
    void anotherCopyStays()
    {
        wchar_t system[MAX_PATH] {};
        QVERIFY(::GetSystemDirectoryW(system, MAX_PATH) > 0);
        const QString other = u"\"%1\\cmd.exe\" --pin-picture \"%2\""_s.arg(QString::fromWCharArray(system), u"%1"_s);
        QVERIFY(write(verbOf(u"png") + L"\\command", nullptr, other));
        QVERIFY(write(kAllFiles + L"\\command", nullptr, other));
        ws::shellmenu::applyPinPicture(false);
        QCOMPARE(value(verbOf(u"png") + L"\\command", nullptr), other);
        QCOMPARE(value(kAllFiles + L"\\command", nullptr), other);
    }

private:
    QString m_stub;
    std::wstring m_scratch;
    HKEY m_root = nullptr;
    bool m_redirected = false;
};

QTEST_GUILESS_MAIN(ShellMenuTest)
#include "tst_shellmenu.moc"
