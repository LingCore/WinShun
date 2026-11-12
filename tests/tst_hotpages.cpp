#include "platform/HotPages.h"

#include <QtTest>

#include <algorithm>
#include <random>

#include <windows.h>
#include <psapi.h>

using namespace ws;

namespace {

constexpr std::size_t kPage = PageSet::kPage;

PageSet pages(std::initializer_list<std::pair<std::size_t, std::size_t>> fromTo)
{
    std::vector<PageSet::Range> ranges;
    for (const auto& [from, to] : fromTo)
        ranges.push_back({from * kPage, (to - from) * kPage});
    return PageSet(std::move(ranges));
}

// Memory of this test's own, given back when it goes.
struct Block {
    explicit Block(std::size_t pageCount)
        : bytes(pageCount * kPage)
        , data(static_cast<char*>(::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
    {
    }
    ~Block() { ::VirtualFree(data, 0, MEM_RELEASE); }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    void write(char value) const
    {
        for (std::size_t at = 0; at < bytes; at += kPage)
            data[at] = value;
    }
    PageSet all() const { return part(0, bytes / kPage); }
    PageSet part(std::size_t from, std::size_t to) const
    {
        return PageSet({{reinterpret_cast<std::uintptr_t>(data) + from * kPage, (to - from) * kPage}});
    }
    // How many of its pages [from, to) are in the working set.
    std::size_t inMemory(std::size_t from, std::size_t to) const
    {
        std::vector<PSAPI_WORKING_SET_EX_INFORMATION> asked(to - from);
        for (std::size_t i = 0; i < asked.size(); ++i)
            asked[i].VirtualAddress = data + (from + i) * kPage;
        if (!::QueryWorkingSetEx(::GetCurrentProcess(), asked.data(), static_cast<DWORD>(asked.size() * sizeof asked[0])))
            return 0;
        return static_cast<std::size_t>(std::ranges::count_if(asked, [](const auto& page) { return page.VirtualAttributes.Valid != 0; }));
    }

    const std::size_t bytes;
    char* const data;
};

} // namespace

class HotPagesTest : public QObject {
    Q_OBJECT

private slots:
    void rangesAreMergedAndSorted()
    {
        const PageSet set({{9 * kPage, kPage}, {kPage, 2 * kPage}, {2 * kPage, 2 * kPage}, {4 * kPage, kPage}, {20 * kPage, 0}});
        QCOMPARE(set, pages({{1, 5}, {9, 10}}));
        QCOMPARE(set.bytes(), 5 * kPage);
        QVERIFY(PageSet().empty());
        QCOMPARE(PageSet().bytes(), std::size_t {0});
    }

    void unitedAndWithout()
    {
        const PageSet a = pages({{0, 4}, {10, 20}, {30, 31}});
        const PageSet b = pages({{2, 12}, {15, 16}, {19, 25}, {40, 41}});
        QCOMPARE(a.united(b), pages({{0, 25}, {30, 31}, {40, 41}}));
        QCOMPARE(a.without(b), pages({{0, 2}, {12, 15}, {16, 19}, {30, 31}}));
        QCOMPARE(b.without(a), pages({{4, 10}, {20, 25}, {40, 41}}));
        QCOMPARE(a.intersected(b), pages({{2, 4}, {10, 12}, {15, 16}, {19, 20}}));
        QCOMPARE(b.intersected(a), a.intersected(b));
        QCOMPARE(a.intersected(PageSet()), PageSet());
        QCOMPARE(a.intersected(a), a);
        QCOMPARE(a.without(a), PageSet());
        QCOMPARE(a.without(PageSet()), a);
        QCOMPARE(PageSet().without(a), PageSet());
        // One range of the other over several of these, and the next of these beginning inside it.
        QCOMPARE(pages({{0, 2}, {3, 5}, {6, 9}}).without(pages({{1, 7}})), pages({{0, 1}, {7, 9}}));
    }

    // Against a set of page numbers, the plain way.
    void algebraAgreesWithPageNumbers()
    {
        std::mt19937 random(280);
        const auto some = [&random] {
            std::vector<bool> in(400);
            std::vector<PageSet::Range> ranges;
            for (int i = std::uniform_int_distribution(0, 40)(random); i > 0; --i) {
                const std::size_t from = std::uniform_int_distribution<std::size_t>(0, 390)(random);
                const std::size_t count = std::uniform_int_distribution<std::size_t>(0, 9)(random);
                ranges.push_back({from * kPage, count * kPage});
                std::fill_n(in.begin() + static_cast<std::ptrdiff_t>(from), count, true);
            }
            return std::pair(PageSet(std::move(ranges)), in);
        };
        const auto numbers = [](const PageSet& set) {
            std::vector<bool> in(400);
            std::uintptr_t last = 0;
            for (const PageSet::Range& r : set.ranges()) {
                if (r.bytes == 0 || r.start % kPage || r.bytes % kPage || (last != 0 && r.start <= last))
                    return std::vector<bool>(); // not as promised: empty, off a boundary, touching or out of order
                last = r.end();
                std::fill_n(in.begin() + static_cast<std::ptrdiff_t>(r.start / kPage), r.bytes / kPage, true);
            }
            return in;
        };
        for (int round = 0; round < 2'000; ++round) {
            const auto [a, inA] = some();
            const auto [b, inB] = some();
            std::vector<bool> both(400), left(400), common(400);
            for (std::size_t i = 0; i < 400; ++i) {
                both[i] = inA[i] || inB[i];
                left[i] = inA[i] && !inB[i];
                common[i] = inA[i] && inB[i];
            }
            QCOMPARE(numbers(a), inA);
            QCOMPARE(numbers(a.united(b)), both);
            QCOMPARE(numbers(a.without(b)), left);
            QCOMPARE(numbers(a.intersected(b)), common);
            QCOMPARE(a.intersected(b).bytes(), static_cast<std::size_t>(std::ranges::count(common, true)) * kPage);
            QCOMPARE(a.without(b).bytes(), static_cast<std::size_t>(std::ranges::count(left, true)) * kPage);
        }
    }

    void withinKeepsWholePagesOnly()
    {
        const auto at = [](std::size_t address) { return reinterpret_cast<const void*>(address); };
        const std::vector<std::pair<const void*, std::size_t>> blocks {
            {at(10 * kPage + 16), 3 * kPage}, // pages 11 and 12 lie wholly inside
            {at(20 * kPage), 2 * kPage},
            {at(30 * kPage + 1), kPage}, // none does
        };
        QCOMPARE(PageSet::within(blocks), pages({{11, 13}, {20, 22}}));
    }

    void smallestFirst()
    {
        const PageSet set = pages({{0, 8}, {10, 11}, {20, 24}, {30, 32}});
        QCOMPARE(set.smallest(7 * kPage), pages({{10, 11}, {20, 24}, {30, 32}}));
        QCOMPARE(set.smallest(3 * kPage), pages({{10, 11}, {30, 32}}));
        QCOMPARE(set.smallest(100 * kPage), set);
        QCOMPARE(set.smallest(0), PageSet());
    }

    // The point of it all: some pages go, the others stay where they are,
    // and those that went still hold what they held.
    void releaseTakesOnlyThesePagesOut()
    {
        const Block block(64);
        block.write('w');
        QCOMPARE(block.inMemory(0, 64), std::size_t {64});
        QCOMPARE(block.all().without(PageSet::resident()), PageSet()); // resident() sees them

        block.part(16, 48).release();
        QCOMPARE(block.inMemory(0, 16), std::size_t {16});
        QCOMPARE(block.inMemory(16, 48), std::size_t {0});
        QCOMPARE(block.inMemory(48, 64), std::size_t {16});
        QCOMPARE(block.part(16, 48).without(PageSet::resident()), block.part(16, 48));

        for (std::size_t page = 0; page < 64; ++page)
            QCOMPARE(block.data[page * kPage], 'w');
        QCOMPARE(block.inMemory(0, 64), std::size_t {64});
    }

    // What IdleTrim does: everything in memory but the pages to keep.
    void allButTheKeptPagesGo()
    {
        const Block kept(32), cold(256);
        kept.write('k');
        cold.write('c');
        PageSet::resident().without(kept.all()).release();
        QCOMPARE(kept.inMemory(0, 32), std::size_t {32});
        QVERIFY2(cold.inMemory(0, 256) == 0, qPrintable(QString::number(cold.inMemory(0, 256))));
        QCOMPARE(cold.data[100 * kPage], 'c'); // and back when used
        QCOMPARE(cold.inMemory(100, 101), std::size_t {1});
    }

    void releaseLeavesLockedAndGuardPagesAsTheyAre()
    {
        const Block block(8);
        block.write('w');
        QVERIFY(::VirtualLock(block.data + 2 * kPage, kPage));
        DWORD was = 0;
        QVERIFY(::VirtualProtect(block.data + 6 * kPage, kPage, PAGE_READWRITE | PAGE_GUARD, &was));
        block.part(0, 4).release(); // the pages around the locked one go; it stays, locked
        QCOMPARE(block.inMemory(0, 2) + block.inMemory(3, 4), std::size_t {0});
        QCOMPARE(block.inMemory(2, 3), std::size_t {1});
        QVERIFY(::VirtualUnlock(block.data + 2 * kPage, kPage)); // it was still locked
        block.part(4, 8).release();
        MEMORY_BASIC_INFORMATION guard {};
        QVERIFY(::VirtualQuery(block.data + 6 * kPage, &guard, sizeof guard));
        QVERIFY(guard.Protect & PAGE_GUARD);
    }

    void releaseAndPrefetchPassOverFreedMemory()
    {
        PageSet gone;
        {
            const Block block(16);
            block.write('w');
            ::VirtualFree(block.data + 4 * kPage, 4 * kPage, MEM_DECOMMIT);
            block.all().release(); // a hole in the middle
            QCOMPARE(block.inMemory(0, 4) + block.inMemory(8, 16), std::size_t {0});
            QCOMPARE(block.all().prefetch(), 12 * kPage); // asked for: the committed pages only
            gone = block.all();
        }
        gone.release(); // nothing there any more (unless something else has the place by now): no harm
        gone.prefetch();
        QCOMPARE(PageSet().prefetch(), std::size_t {0});
    }

    void firstOpeningIsTakenAsItIs()
    {
        HotPages hot;
        QCOMPARE(hot.bytes(), std::size_t {0});
        hot.learn(pages({{0, 100}, {200, 210}}));
        QCOMPARE(hot.pages(), pages({{0, 100}, {200, 210}}));

        const std::size_t most = HotPages::kMostBytes / kPage;
        HotPages large; // too much at once: the small ranges of it, and that is all
        large.learn(pages({{0, 10}, {100'000, 100'000 + most + 1}}));
        QCOMPARE(large.pages(), pages({{0, 10}}));
        large.learn(pages({{20, 30}, {100'000, 100'000 + most + 1}}));
        large.learn(pages({{20, 30}, {100'000, 100'000 + most + 1}}));
        QCOMPARE(large.pages(), pages({{0, 10}}));
    }

    // What an opening allocates and never uses again must not add up.
    void laterOnlyWhatTwoOpeningsInARowUsed()
    {
        HotPages hot;
        hot.learn(pages({{0, 100}}));
        hot.learn(pages({{300, 310}, {400, 405}})); // once: not yet
        QCOMPARE(hot.pages(), pages({{0, 100}}));
        hot.learn(pages({{300, 310}, {500, 505}})); // 300.. again: stays from now on; 400.. was that once
        QCOMPARE(hot.pages(), pages({{0, 100}, {300, 310}}));
        hot.learn(pages({{400, 405}, {500, 502}})); // 400.. not in a row; 500..502 twice
        QCOMPARE(hot.pages(), pages({{0, 100}, {300, 310}, {500, 502}}));
        hot.learn(PageSet()); // nothing else used
        hot.learn(pages({{400, 405}}));
        QCOMPARE(hot.pages(), pages({{0, 100}, {300, 310}, {500, 502}}));

        for (int opening = 0; opening < 50; ++opening) { // new memory each time, as an opening allocates
            const std::size_t at = 10'000 + static_cast<std::size_t>(opening) * 600;
            hot.learn(pages({{at, at + 500}}));
        }
        QCOMPARE(hot.pages(), pages({{0, 100}, {300, 310}, {500, 502}}));
    }

    void whatWasAddedGoesOnceGrownByHalf()
    {
        const std::size_t slack = HotPages::kSlackBytes / kPage;
        const auto twice = [](HotPages& hot, const PageSet& used) {
            hot.learn(used);
            hot.learn(used);
        };
        HotPages small; // 100 pages at first: the slack is more than half of that
        small.learn(pages({{0, 100}}));
        twice(small, pages({{1'000, 1'000 + slack}}));
        QCOMPARE(small.pages(), pages({{0, 100}, {1'000, 1'000 + slack}}));
        twice(small, pages({{50'000, 50'001}})); // one page more: back to the first ones
        QCOMPARE(small.pages(), pages({{0, 100}}));
        twice(small, pages({{50'000, 50'001}})); // still used: back after two openings
        QCOMPARE(small.pages(), pages({{0, 100}, {50'000, 50'001}}));

        HotPages large;
        const std::size_t first = 4 * slack;
        large.learn(pages({{0, first}}));
        twice(large, pages({{100'000, 100'000 + first / 2}}));
        QCOMPARE(large.pages(), pages({{0, first}, {100'000, 100'000 + first / 2}}));
        twice(large, pages({{200'000, 200'001}}));
        QCOMPARE(large.pages(), pages({{0, first}}));
    }
};

QTEST_APPLESS_MAIN(HotPagesTest)
#include "tst_hotpages.moc"
