#include "PinGroups.h"
#include "PinManager.h"

#include <QImage>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace ws::capture;

namespace ws::shell {
int popupMenu(HWND, const std::vector<MenuItem>&, POINT, std::function<void()>) { return 0; }
QString pickSaveFile(HWND, const QString&, const QString&, const QString&, const QString&) { return {}; }
} // namespace ws::shell

namespace {

QImage sampleImage(QColor color)
{
    QImage img(48, 48, QImage::Format_ARGB32_Premultiplied);
    img.fill(color);
    return img;
}

} // namespace

class PinGroupsTest : public QObject {
    Q_OBJECT

private slots:
    void storeCrudAndValidation()
    {
        PinGroupStore store;
        QCOMPARE(store.groups().size(), std::size_t {1});
        QCOMPARE(store.groups().front().id, QString());
        QVERIFY(!store.hasCustomGroups());
        QCOMPARE(store.activeGroupId(), QString());
        QVERIFY(!store.showingAll());
        QCOMPARE(store.targetGroupForNewPin(), QString());

        // Reject empty, built-in names, and overly long names.
        QString err;
        QVERIFY(!store.createGroup(u"   "_s, &err).has_value());
        QVERIFY(!err.isEmpty());
        QVERIFY(!store.createGroup(u"默认"_s, &err).has_value());
        QVERIFY(!store.createGroup(u"Default"_s, &err).has_value());
        QVERIFY(!store.createGroup(QString(PinGroupStore::kMaxNameLength + 1, u'a'), &err).has_value());

        // Create and trim custom groups, deduplicating case-insensitively.
        const auto work = store.createGroup(u"  工作  "_s, &err);
        QVERIFY(work.has_value());
        QCOMPARE(work->name, u"工作"_s);
        QVERIFY(!work->id.isEmpty());
        QVERIFY(store.hasCustomGroups());

        const auto design = store.createGroup(u"Design"_s, &err);
        QVERIFY(design.has_value());
        QVERIFY(design->id != work->id);
        QVERIFY(!store.createGroup(u"design"_s, &err).has_value());

        // Rename validation and update.
        QVERIFY(!store.renameGroup(QString(), u"New"_s, &err));
        QVERIFY(!store.renameGroup(design->id, u"工作"_s, &err));
        QVERIFY(store.renameGroup(design->id, u"设计稿"_s, &err));
        QCOMPARE(store.nameOf(design->id), u"设计稿"_s);

        // Suggested names avoid collisions.
        const auto g1 = store.createGroup(store.suggestedNewName(), &err);
        QVERIFY(g1.has_value());
        QVERIFY(store.suggestedNewName() != g1->name);

        // Deleting active group resets active back to default.
        store.setActiveGroup(work->id);
        QCOMPARE(store.activeGroupId(), work->id);
        QVERIFY(store.deleteGroup(work->id));
        QCOMPARE(store.activeGroupId(), QString());
        QVERIFY(!store.contains(work->id));
        QVERIFY(!store.deleteGroup(QString()));

        // Enforce maximum custom groups limit.
        while (static_cast<int>(store.customGroups().size()) < PinGroupStore::kMaxCustomGroups)
            QVERIFY(store.createGroup(u"组%1"_s.arg(store.customGroups().size() + 100)).has_value());
        QVERIFY(!store.createGroup(u"超出上限"_s, &err).has_value());
        QVERIFY(!err.isEmpty());
    }

    void storeCyclingAndSerialization()
    {
        PinGroupStore store;
        QVERIFY(!store.nextGroupId().has_value()); // no custom groups yet

        const auto a = store.createGroup(u"工作"_s);
        const auto b = store.createGroup(u"参考"_s);
        QVERIFY(a && b);

        // Cycle: default -> a -> b -> default.
        QCOMPARE(store.nextGroupId(), std::optional<QString>(a->id));
        store.setActiveGroup(a->id);
        QCOMPARE(store.nextGroupId(), std::optional<QString>(b->id));
        store.setActiveGroup(b->id);
        QCOMPARE(store.nextGroupId(), std::optional<QString>(QString()));

        // From "show all" view, cycling lands on the first custom group.
        store.setShowingAll(true);
        QCOMPARE(store.activeStateToken(), u"*"_s);
        QCOMPARE(store.targetGroupForNewPin(), QString());
        QCOMPARE(store.nextGroupId(), std::optional<QString>(a->id));

        // JSON round-trip preserves order, ids, names, and advances id allocator.
        const QString json = store.serializeGroups();
        PinGroupStore reloaded;
        reloaded.loadGroups(json);
        reloaded.setActiveStateToken(b->id);
        QCOMPARE(reloaded.customGroups().size(), std::size_t {2});
        QCOMPARE(reloaded.nameOf(a->id), u"工作"_s);
        QCOMPARE(reloaded.nameOf(b->id), u"参考"_s);
        QCOMPARE(reloaded.activeGroupId(), b->id);
        const auto c = reloaded.createGroup(u"第三个"_s);
        QVERIFY(c.has_value());
        QVERIFY(c->id != a->id && c->id != b->id);

        // Unknown/malformed active token and malformed JSON degrade cleanly.
        reloaded.loadGroups(u"not-json"_s);
        QVERIFY(!reloaded.hasCustomGroups());
        QCOMPARE(reloaded.activeGroupId(), QString());
        reloaded.setActiveStateToken(u"missing-id"_s);
        QCOMPARE(reloaded.activeGroupId(), QString());
    }

    void pinManagerGroupVisibilityAndMove()
    {
        int groupsChangedCount = 0;
        QStringList notices;
        PinManager::Callbacks cb;
        cb.groupsChanged = [&] { ++groupsChangedCount; };
        cb.groupNotice = [&](const QString& text) { notices.append(text); };

        PinManager manager(std::move(cb));
        PinWindow* pDefault = manager.pin(sampleImage(Qt::red), {40, 40});
        QVERIFY(pDefault != nullptr);
        QCOMPARE(pDefault->groupId(), QString());
        QVERIFY(!pDefault->groupHidden());
        QVERIFY(pDefault->visibleNow());

        // Create two custom groups and switch to "工作".
        const auto work = manager.createGroup(u"工作"_s);
        const auto ref = manager.createGroup(u"参考"_s);
        QVERIFY(work && ref);
        QVERIFY(groupsChangedCount >= 2);

        manager.switchGroup(work->id);
        QVERIFY(pDefault->groupHidden());
        QVERIFY(!pDefault->visibleNow());

        // New pins created while "工作" is active belong to "工作".
        PinWindow* pWork1 = manager.pin(sampleImage(Qt::green), {120, 40});
        PinWindow* pWork2 = manager.pin(sampleImage(Qt::blue), {200, 40});
        QVERIFY(pWork1 && pWork2);
        QCOMPARE(pWork1->groupId(), work->id);
        QCOMPARE(pWork2->groupId(), work->id);
        QVERIFY(!pWork1->groupHidden());
        QCOMPARE(manager.pinCountInGroup(QString()), 1);
        QCOMPARE(manager.pinCountInGroup(work->id), 2);
        QCOMPARE(manager.visiblePinCount(), 2);

        // Moving a pin out of the active group hides it and notifies the user.
        manager.movePinToGroup(pWork2, ref->id);
        QCOMPARE(pWork2->groupId(), ref->id);
        QVERIFY(pWork2->groupHidden());
        QCOMPARE(manager.pinCountInGroup(ref->id), 1);
        QCOMPARE(manager.visiblePinCount(), 1);
        QVERIFY(!notices.isEmpty() && notices.last().contains(u"参考"_s));

        // closeOthers only closes other pins in the currently visible group, never hidden groups!
        PinWindow* pWorkExtra = manager.pin(sampleImage(Qt::yellow), {280, 40});
        QVERIFY(pWorkExtra != nullptr);
        manager.closeOthers(pWork1);
        QCoreApplication::processEvents();
        QVERIFY(manager.contains(pWork1));
        QVERIFY(!manager.contains(pWorkExtra));
        QVERIFY(manager.contains(pDefault)); // hidden default group preserved!
        QVERIFY(manager.contains(pWork2)); // hidden ref group preserved!

        // showAllGroups makes every group's pins visible simultaneously.
        manager.showAllGroups();
        QVERIFY(!pDefault->groupHidden());
        QVERIFY(!pWork1->groupHidden());
        QVERIFY(!pWork2->groupHidden());
        QCOMPARE(manager.visiblePinCount(), 3);

        // cycleGroup from showAll jumps to the first custom group ("工作") and un-hides global hide.
        manager.setAllHidden(true);
        QVERIFY(manager.allHidden());
        const auto cycled = manager.cycleGroup();
        QCOMPARE(cycled, std::optional<QString>(work->id));
        QVERIFY(!manager.allHidden());
        QVERIFY(!pWork1->groupHidden());
        QVERIFY(pDefault->groupHidden());

        // deleteGroup rescues pins back to default non-destructively.
        QVERIFY(manager.deleteGroup(work->id));
        QCOMPARE(manager.groups().activeGroupId(), QString());
        QCOMPARE(pWork1->groupId(), QString());
        QVERIFY(!pWork1->groupHidden());
        QVERIFY(!pDefault->groupHidden());
        QVERIFY(pWork2->groupHidden()); // still safely in "参考"

        manager.closeAll();
        QCoreApplication::processEvents();
        QCOMPARE(manager.count(), 0);
    }
};

QTEST_MAIN(PinGroupsTest)
#include "tst_pingroups.moc"
