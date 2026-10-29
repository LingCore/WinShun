// Win+S / Win+V and Explorer's DisabledHotkeys (src/app/platform/WinV): which
// letters are ours to take out again. The decision is a pure function; the
// registry part runs with HKEY_CURRENT_USER redirected to a scratch key, so the
// real DisabledHotkeys is never read or written.

#include "WinV.h"

#include <QTest>
#include <QUuid>

#include <windows.h>

#include <string>

using namespace ws::winv;

namespace {

constexpr wchar_t kAdvanced[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr wchar_t kSentinel[] = L"WinShunTestSentinel"; // in the scratch key's root only

std::wstring readValue(const wchar_t* subKey, const wchar_t* name, bool* exists = nullptr)
{
    wchar_t value[128] {};
    DWORD size = sizeof value - sizeof(wchar_t);
    const bool ok = ::RegGetValueW(HKEY_CURRENT_USER, subKey, name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
    if (exists)
        *exists = ok;
    return ok ? value : std::wstring();
}

void writeValue(const wchar_t* subKey, const wchar_t* name, const std::wstring& text)
{
    HKEY key = nullptr;
    QVERIFY2(::RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS,
        "scratch key");
    ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
        static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
}

} // namespace

class WinKeysTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // HKCU of this process becomes a key under the real one, in which
        // everything below happens.
        m_scratch = L"Software\\WinShunTest\\" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString();
        QVERIFY(::RegCreateKeyExW(HKEY_CURRENT_USER, m_scratch.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &m_root, nullptr)
            == ERROR_SUCCESS);
        const std::wstring mark = m_scratch;
        QVERIFY(::RegSetValueExW(m_root, kSentinel, 0, REG_SZ, reinterpret_cast<const BYTE*>(mark.c_str()),
                    static_cast<DWORD>((mark.size() + 1) * sizeof(wchar_t)))
            == ERROR_SUCCESS);
        QCOMPARE(::RegOverridePredefKey(HKEY_CURRENT_USER, m_root), static_cast<LONG>(ERROR_SUCCESS));
        m_redirected = true;
        QVERIFY(redirected());
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

    void init()
    {
        // Proof, before anything is deleted, that HKCU is still the scratch key:
        // its marker reads back at the root.
        QVERIFY(redirected());
        ::RegDeleteKeyValueW(HKEY_CURRENT_USER, kAdvanced, L"DisabledHotkeys");
        ::RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\WinShun");
    }

    void plan_data()
    {
        QTest::addColumn<bool>("on");
        QTest::addColumn<bool>("present");
        QTest::addColumn<bool>("recorded");
        QTest::addColumn<bool>("add");
        QTest::addColumn<bool>("remove");
        QTest::addColumn<bool>("recordedAfter");
        QTest::addColumn<bool>("blocked");
        QTest::addColumn<int>("origin");
        const int toggle = int(Origin::Toggle), start = int(Origin::Startup), other = int(Origin::StartupOtherProfile);
        QTest::newRow("on, nothing yet") << true << false << false << true << false << true << false << toggle;
        QTest::newRow("on, foreign letter, toggled") << true << true << false << false << false << false << false << toggle;
        QTest::newRow("on, older version's letter, startup") << true << true << false << false << false << true << false << start;
        QTest::newRow("on, foreign letter, other profile") << true << true << false << false << false << false << false << other;
        QTest::newRow("on, nothing yet, other profile") << true << false << false << true << false << true << false << other;
        QTest::newRow("on, there and recorded") << true << true << true << false << false << true << false << toggle;
        QTest::newRow("on, recorded but gone") << true << false << true << true << false << true << false << toggle;
        QTest::newRow("off, ours") << false << true << true << false << true << false << false << toggle;
        QTest::newRow("off, ours, startup") << false << true << true << false << true << false << false << start;
        QTest::newRow("off, ours, other profile: kept") << false << true << true << false << false << true << false << other;
        QTest::newRow("off, recorded, gone") << false << false << true << false << false << false << false << toggle;
        QTest::newRow("off, recorded, gone, other profile: kept") << false << false << true << false << false << true << false << other;
        QTest::newRow("off, foreign (the reporter's)") << false << true << false << false << false << false << true << toggle;
        QTest::newRow("off, foreign, other profile") << false << true << false << false << false << false << false << other;
        QTest::newRow("off, nothing") << false << false << false << false << false << false << false << toggle;
    }

    void plan()
    {
        QFETCH(bool, on);
        QFETCH(bool, present);
        QFETCH(bool, recorded);
        QFETCH(bool, add);
        QFETCH(bool, remove);
        QFETCH(bool, recordedAfter);
        QFETCH(bool, blocked);
        QFETCH(int, origin);
        const Plan expected {.addLetter = add, .removeLetter = remove, .recorded = recordedAfter, .blocked = blocked};
        QVERIFY(planFor(on, present, recorded, Origin(origin)) == expected);
    }

    void switchOnWritesLetterAndRecord()
    {
        writeValue(kAdvanced, L"DisabledHotkeys", L"A");
        QVERIFY(reconcile(L'S', true));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"AS"));
        QVERIFY(recordedAsOurs(L'S'));
        QVERIFY(!recordedAsOurs(L'V'));
        QVERIFY(reconcile(L'S', true)); // idempotent
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"AS"));
    }

    void switchOffRemovesOurLetterKeepsOthers()
    {
        QVERIFY(reconcile(L'S', true));
        QVERIFY(reconcile(L'V', true));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"SV"));
        QVERIFY(reconcile(L'S', false));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"V"));
        QVERIFY(!recordedAsOurs(L'S'));
        QVERIFY(recordedAsOurs(L'V'));
    }

    void switchOffLeavesForeignLetter()
    {
        writeValue(kAdvanced, L"DisabledHotkeys", L"SV"); // the issue: set by someone else
        QVERIFY(reconcile(L'S', false));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"SV"));
        QVERIFY(!recordedAsOurs(L'S'));
        QVERIFY(planFor(false, releasedByExplorer(L'S'), recordedAsOurs(L'S')).blocked);
    }

    void olderVersionsLetterIsTakenOverAtStartup()
    {
        writeValue(kAdvanced, L"DisabledHotkeys", L"S");
        QVERIFY(reconcile(L'S', true, Origin::Startup));
        QVERIFY(recordedAsOurs(L'S'));
        QVERIFY(reconcile(L'S', false));
        bool exists = true;
        readValue(kAdvanced, L"DisabledHotkeys", &exists);
        QVERIFY(!exists); // the last letter: the value goes
    }

    void foreignLetterIsNotAdoptedByToggle()
    {
        writeValue(kAdvanced, L"DisabledHotkeys", L"S");
        QVERIFY(reconcile(L'S', true)); // switched on by hand
        QVERIFY(!recordedAsOurs(L'S'));
        QVERIFY(reconcile(L'S', false));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"S"));
    }

    void otherProfileStartupLeavesMainCopysState()
    {
        QVERIFY(reconcile(L'S', true)); // the main copy
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"S"));
        QVERIFY(reconcile(L'S', false, Origin::StartupOtherProfile)); // --profile, switch off
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"S"));
        QVERIFY(recordedAsOurs(L'S'));
        QVERIFY(reconcile(L'S', true, Origin::StartupOtherProfile)); // on: adds and records, never adopts
        QVERIFY(recordedAsOurs(L'S'));
        QVERIFY(reconcile(L'V', true, Origin::StartupOtherProfile));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"SV"));
        QVERIFY(recordedAsOurs(L'V'));
    }

    void lettersAreMatchedExactly()
    {
        writeValue(kAdvanced, L"DisabledHotkeys", L"s"); // VK_F4, not Win+S
        QVERIFY(!releasedByExplorer(L'S'));
        QVERIFY(reconcile(L'S', true));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"sS"));
        QVERIFY(reconcile(L'S', false));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"s"));
        QVERIFY(giveBack(L'S'));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"s"));
        QVERIFY(!planFor(false, releasedByExplorer(L'S'), false).blocked);
    }

    void giveBackTakesForeignLetterAndRecord()
    {
        writeValue(kAdvanced, L"DisabledHotkeys", L"SV");
        QVERIFY(giveBack(L'S'));
        QCOMPARE(readValue(kAdvanced, L"DisabledHotkeys"), std::wstring(L"V"));
        QVERIFY(giveBack(L'V'));
        bool exists = true;
        readValue(kAdvanced, L"DisabledHotkeys", &exists);
        QVERIFY(!exists);
        QVERIFY(giveBack(L'V')); // nothing left to give
    }

    void giveBackDropsRecordAndEmptyKey()
    {
        QVERIFY(reconcile(L'S', true));
        QVERIFY(giveBack(L'S'));
        QVERIFY(!recordedAsOurs(L'S'));
        QVERIFY(!releasedByExplorer(L'S'));
        HKEY key = nullptr;
        QVERIFY(::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\WinShun", 0, KEY_READ, &key) == ERROR_FILE_NOT_FOUND);
    }

    void recordKeyKeptWhileOtherValuesLive()
    {
        writeValue(L"Software\\WinShun", L"Other", L"x");
        QVERIFY(reconcile(L'S', true));
        QVERIFY(giveBack(L'S'));
        QCOMPARE(readValue(L"Software\\WinShun", L"Other"), std::wstring(L"x"));
    }

private:
    bool redirected() const
    {
        return m_redirected && readValue(L"", kSentinel) == m_scratch;
    }

    std::wstring m_scratch;
    HKEY m_root = nullptr;
    bool m_redirected = false;
};

QTEST_GUILESS_MAIN(WinKeysTest)
#include "tst_winkeys.moc"
