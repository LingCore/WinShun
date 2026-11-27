#include "AppCatalog.h"
#include "EverythingIpc.h"
#include "FileIndex.h"
#include "FuzzyMatch.h"
#include "History.h"
#include "IndexService.h"
#include "NameSearch.h"
#include "Query.h"
#include "SearchEngine.h"

#include <QDateTime>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <string>
#include <vector>

using namespace ws;
using namespace Qt::StringLiterals;

class FuzzyMatchTest : public QObject {
    Q_OBJECT

private slots:
    void algorithmCoreAndRanking()
    {
        QVERIFY(fuzzy::isSubsequence("Report.pdf", "rpt"));
        QVERIFY(fuzzy::isSubsequence("NameSearch.cpp", "nsrch"));
        QVERIFY(fuzzy::isSubsequence("Visual Studio Code", "vcode"));
        QVERIFY(fuzzy::isSubsequence("SettingsWindow.qml", "swqml"));
        QVERIFY(fuzzy::isSubsequence("package.json", "pkgjson"));
        QVERIFY(!fuzzy::isSubsequence("Visual Studio Code", "vscodx"));
        QVERIFY(!fuzzy::isSubsequence("HiBit Startup Manager", "hms"));
        QVERIFY(!fuzzy::match("Report.pdf", "").has_value());
        QVERIFY(!fuzzy::match("", "a").has_value());

        // Word-start and tight matches must outscore scattered mid-word hits.
        const auto goodRpt = fuzzy::match("Report.pdf", "rpt");
        const auto badRpt = fuzzy::match("arbitrary_patch_test.txt", "rpt");
        QVERIFY(goodRpt.has_value());
        QVERIFY(badRpt.has_value());
        QVERIFY2(goodRpt->score > badRpt->score,
            qPrintable(u"expected Report.pdf (%1) > arbitrary_patch_test.txt (%2)"_s.arg(goodRpt->score).arg(badRpt->score)));
        QVERIFY(fuzzy::passesAutoThreshold(goodRpt->score, 3));

        // Heavily scattered mid-word garbage is rejected by automatic threshold.
        const auto garbage = fuzzy::match("a1b2n3c4d5e6s7f8g9r0h1i2j3c4k5l6h.tmp", "nsrch");
        QVERIFY(garbage.has_value());
        QVERIFY(!fuzzy::passesAutoThreshold(garbage->score, 5));

        // Good abbreviations pass automatic threshold.
        const auto nsrch = fuzzy::match("NameSearch.cpp", "nsrch");
        const auto vcode = fuzzy::match("Visual Studio Code", "vcode");
        const auto swqml = fuzzy::match("SettingsWindow.qml", "swqml");
        const auto pkgjson = fuzzy::match("package.json", "pkgjson");
        QVERIFY(nsrch && fuzzy::passesAutoThreshold(nsrch->score, 5));
        QVERIFY(vcode && fuzzy::passesAutoThreshold(vcode->score, 5));
        QVERIFY(swqml && fuzzy::passesAutoThreshold(swqml->score, 5));
        QVERIFY(pkgjson && fuzzy::passesAutoThreshold(pkgjson->score, 7));

        // Better later contiguous boundary match beats weak earlier match (V2 DP).
        std::vector<std::size_t> pos;
        const auto later = fuzzy::matchPositions("axxx_ab", "ab", pos);
        QVERIFY(later.has_value());
        QCOMPARE(pos, (std::vector<std::size_t> {5, 6}));

        // Exact tie prefers earlier occurrence.
        const auto tie = fuzzy::matchPositions("ab_ab", "ab", pos);
        QVERIFY(tie.has_value());
        QCOMPARE(pos, (std::vector<std::size_t> {0, 1}));
    }

    void discreteHighlightRangesAndUnicode()
    {
        using Range = std::pair<qsizetype, qsizetype>;
        // NameSearch.cpp with "nsrch" -> N(0,1), S(4,1), rch(7,3)
        QCOMPARE(fuzzy::highlightRanges(u"NameSearch.cpp", u"nsrch"),
            (QList<Range> {{0, 1}, {4, 1}, {7, 3}}));
        // Report.pdf with "rpt" -> R(0,1), p(2,1), t(5,1)
        QCOMPARE(fuzzy::highlightRanges(u"Report.pdf", u"rpt"),
            (QList<Range> {{0, 1}, {2, 1}, {5, 1}}));
        // Visual Studio Code with "vcode" -> V(0,1), Code(14,4)
        QCOMPARE(fuzzy::highlightRanges(u"Visual Studio Code", u"vcode"),
            (QList<Range> {{0, 1}, {14, 4}}));
        // Unicode / Chinese subsequence highlighting
        QCOMPARE(fuzzy::highlightRanges(u"年度报告.docx", u"年度告"),
            (QList<Range> {{0, 2}, {3, 1}}));
        QCOMPARE(fuzzy::highlightRanges(u"年度报告2026.pdf", u"报告26"),
            (QList<Range> {{2, 3}, {7, 1}}));
        // Surrogate pair (emoji = 2 UTF-16 code units) offset safety
        const QString withEmoji = QString::fromUtf8("\xF0\x9F\x98\x80Report.pdf");
        QCOMPARE(fuzzy::highlightRanges(withEmoji, u"rpt"),
            (QList<Range> {{2, 1}, {4, 1}, {7, 1}}));
        QVERIFY(fuzzy::highlightRanges(u"Report.pdf", u"xyz").isEmpty());
    }

    void v1FallbackForHugeInputs()
    {
        // Pattern longer than V2 cap (64) triggers linear V1 fallback cleanly.
        const std::string pat(70, 'a');
        std::string text;
        for (int i = 0; i < 70; ++i)
            text += "xa";
        std::vector<std::size_t> pos;
        const auto m = fuzzy::matchPositions(text, pat, pos);
        QVERIFY(m.has_value());
        QCOMPARE(pos.size(), pat.size());
        QVERIFY(std::ranges::is_sorted(pos));

        // Large text * pattern > 65536 triggers V1 fallback cleanly.
        std::string huge(3000, 'x');
        huge += "report_target";
        const auto hugeMatch = fuzzy::matchPositions(huge, "reporttarget12345678901234567890", pos);
        QVERIFY(!hugeMatch.has_value());
        std::string hugeHit = std::string(4000, 'z') + "a" + std::string(3000, 'z') + "bcdefghijklmnopqrstuvwxyz";
        const auto hugeOk = fuzzy::matchPositions(hugeHit, "abcdefghijklmnopqrstuvwxyz", pos);
        QVERIFY(hugeOk.has_value());
        QCOMPARE(pos.size(), std::size_t(26));
    }

    void eligibilityBounds()
    {
        // Reject 1-2 char terms and pure punctuation for automatic fuzzy.
        QVERIFY(!fuzzy::isEligibleAutoTerm("a"));
        QVERIFY(!fuzzy::isEligibleAutoTerm("sr"));
        QVERIFY(!fuzzy::isEligibleAutoTerm("ab"));
        QVERIFY(!fuzzy::isEligibleAutoTerm("---"));
        QVERIFY(fuzzy::isEligibleAutoTerm("rpt"));
        QVERIFY(fuzzy::isEligibleAutoTerm("nsrch"));
        QVERIFY(fuzzy::isEligibleAutoTerm("报告26"));
        QVERIFY(!fuzzy::isEligibleAutoTerm("报告")); // only 2 code points
        QVERIFY(fuzzy::isEligibleAutoTerm("年度告")); // 3 code points
    }

    void independentSyntaxAndIsolation()
    {
        // Explicit fz:/fuzzy: works even when NameMatcher has autoFuzzy=false.
        {
            const auto q = parseQuery(u"fz:nsrch"_s);
            QVERIFY(q.error.isEmpty());
            QCOMPARE(q.highlights, QStringList {u"nsrch"_s});
            QCOMPARE(q.fuzzyHighlights, QStringList {u"nsrch"_s});
            const NameMatcher m(q, false);
            QVERIFY(m.hasFuzzyTerms());
            QVERIFY(m.matchPath(u"D:\\src\\NameSearch.cpp"_s, false) >= 0);
            QVERIFY(m.matchPath(u"D:\\src\\OtherFile.cpp"_s, false) < 0);
        }
        {
            const auto q = parseQuery(u"fuzzy:rpt ext:pdf"_s);
            QVERIFY(q.error.isEmpty());
            const NameMatcher m(q, false);
            QVERIFY(m.matchPath(u"D:\\docs\\Report.pdf"_s, false) >= 0);
            QVERIFY(m.matchPath(u"D:\\docs\\Report.txt"_s, false) < 0);
        }
        // Quoted "fz:rpt" stays strictly literal.
        {
            const auto q = parseQuery(u"\"fz:rpt\""_s);
            QVERIFY(q.fuzzyHighlights.isEmpty());
            const NameMatcher m(q, true);
            QVERIFY(m.matchPath(u"D:\\docs\\Report.pdf"_s, false) < 0);
            QVERIFY(m.matchPath(u"D:\\docs\\fz:rpt.txt"_s, false) >= 0);
        }
        // Quoted "nsrch" is NEVER auto-fuzzy even when autoFuzzy=true.
        {
            const auto q = parseQuery(u"\"nsrch\""_s);
            QVERIFY(!isFuzzyEligible(q));
            const NameMatcher m(q, true);
            QVERIFY(!m.hasFuzzyTerms());
            QVERIFY(m.matchPath(u"D:\\src\\NameSearch.cpp"_s, false) < 0);
        }
        // Plain !rpt stays strictly literal exclusion even when autoFuzzy=true;
        // explicit !fz:rpt excludes fuzzy matches.
        {
            const auto qLiteral = parseQuery(u"pdf !rpt"_s);
            const NameMatcher mLiteral(qLiteral, true);
            QVERIFY(mLiteral.matchPath(u"D:\\docs\\Report.pdf"_s, false) >= 0);

            const auto qFuzzyNeg = parseQuery(u"pdf !fz:rpt"_s);
            const NameMatcher mFuzzyNeg(qFuzzyNeg, false);
            QVERIFY(mFuzzyNeg.matchPath(u"D:\\docs\\Report.pdf"_s, false) < 0);
            QVERIFY(mFuzzyNeg.matchPath(u"D:\\docs\\Notes.pdf"_s, false) >= 0);
        }
        // Empty fz: does not produce a syntax error while typing.
        {
            const auto q = parseQuery(u"fz:"_s);
            QVERIFY(q.error.isEmpty());
            QVERIFY(q.isEmpty());
        }
        // Regex mode ignores fz: prefix.
        {
            const auto q = parseQuery(u"fz:rpt"_s, true);
            QVERIFY(q.fuzzyHighlights.isEmpty());
            const NameMatcher m(q, true);
            QVERIFY(m.matchPath(u"D:\\docs\\Report.pdf"_s, false) < 0);
        }
        // Everything IPC (allowFuzzyPrefix=false) ignores fz: prefix completely.
        {
            const auto q = parseQuery(u"fz:rpt"_s, false, false);
            QVERIFY(q.fuzzyHighlights.isEmpty());
            const NameMatcher m(q, false);
            QVERIFY(!m.hasFuzzyTerms());
            QVERIFY(m.matchPath(u"D:\\docs\\Report.pdf"_s, false) < 0);
        }
        // Saved quick-filter conditions stay deterministic even when outer query uses autoFuzzy.
        {
            ParsedQuery q = parseQuery(u"nsrch"_s);
            q.conditions = parseConditions({u"rpt"_s});
            const NameMatcher m(q, true);
            // "Report_NameSearch.cpp" contains "Report" (fuzzy rpt, NOT literal rpt) -> condition must reject!
            QVERIFY(m.matchPath(u"D:\\src\\Report_NameSearch.cpp"_s, false) < 0);
            QVERIFY(m.matchPath(u"D:\\src\\rpt_NameSearch.cpp"_s, false) >= 0);
        }
        // Ancestor + explicit fuzzy filename.
        {
            const auto q = parseQuery(u"fz:core\\nsrch"_s);
            const NameMatcher m(q, false);
            QVERIFY(m.matchPath(u"D:\\src\\core\\NameSearch.cpp"_s, false) >= 0);
            QVERIFY(m.matchPath(u"D:\\src\\app\\NameSearch.cpp"_s, false) < 0);
        }
    }

    void nameSearchTieringAndPruningExactness()
    {
        FileIndex index;
        const EntryId root = index.addRoot("T:");
        const EntryId core = index.add(root, "core", EntryFlag::Directory);
        const EntryId exact = index.add(core, "report.txt", EntryFlag::LowPriority); // even LowPriority normal hit!
        const EntryId normal2 = index.add(core, "my_report.md", 0);
        const EntryId normal3 = index.add(core, "report_2026.pdf", 0);
        const EntryId normal4 = index.add(core, "annual_report.docx", 0);
        const EntryId nsrch = index.add(core, "NameSearch.cpp", 0);
        // Many fuzzy-only hits for "report" (Wait: use "rpt" which matches normal rpt_X and fuzzy Report_X!)
        std::vector<EntryId> normalRpts;
        for (int i = 0; i < 6; ++i)
            normalRpts.push_back(index.add(core, "rpt_" + std::to_string(i) + ".txt", i == 0 ? EntryFlag::LowPriority : 0));
        std::vector<EntryId> fuzzyRpts;
        for (int i = 0; i < 40; ++i)
            fuzzyRpts.push_back(index.add(core, "Report_" + std::to_string(i) + ".pdf", 0));

        WorkerPool pool(2);
        const auto lock = index.readLock();

        // 1) Normal hits ALWAYS outrank fuzzy-only hits, and Top-K pruning keeps totalMatches exact!
        {
            const ParsedQuery q = parseQuery(u"rpt"_s);
            const NameMatcher matcher(q, true);
            const NameSearchOutput out = searchNames(index, matcher, {.limit = 4}, pool, {});
            QCOMPARE(out.hits.size(), std::size_t(4));
            for (const NameHit& h : out.hits)
                QVERIFY2(!h.fuzzy, "Top-4 must all be normal hits when 6 normal hits exist");

            // Brute-force check against NameMatcher::match()
            std::size_t brute = 0;
            NameMatcher::Folders seen;
            for (std::size_t i = 0; i < index.slotCount(); ++i) {
                const auto id = static_cast<EntryId>(i);
                const EntryScan& e = index.scan(id);
                if (!(e.flags & (EntryFlag::Deleted | EntryFlag::Root)) && matcher.match(index, e, seen, id) >= 0)
                    ++brute;
            }
            QCOMPARE(out.totalMatches, brute);
        }

        // 2) Larger limit includes all 6 normal hits FIRST, then fuzzy hits after them!
        {
            const ParsedQuery q = parseQuery(u"rpt"_s);
            const NameMatcher matcher(q, true);
            const NameSearchOutput out = searchNames(index, matcher, {.limit = 15}, pool, {});
            QCOMPARE(out.hits.size(), std::size_t(15));
            for (std::size_t i = 0; i < 6; ++i)
                QVERIFY(!out.hits[i].fuzzy);
            for (std::size_t i = 6; i < 15; ++i)
                QVERIFY(out.hits[i].fuzzy);
        }

        // 3) autoFuzzy=false does not find NameSearch.cpp for "nsrch", autoFuzzy=true does!
        {
            const ParsedQuery q = parseQuery(u"nsrch"_s);
            QCOMPARE(searchNames(index, NameMatcher(q, false), {.limit = 10}, pool, {}).totalMatches, std::size_t(0));
            const auto out = searchNames(index, NameMatcher(q, true), {.limit = 10}, pool, {});
            QCOMPARE(out.totalMatches, std::size_t(1));
            QCOMPARE(out.hits.front().id, nsrch);
            QVERIFY(out.hits.front().fuzzy);
        }

        // 4) Multi-term ancestor literal + filename fuzzy: "core nsrch" ranks
        //    src\core\NameSearch.cpp ABOVE a file matching both terms only fuzzily.
        {
            const EntryId other = index.add(root, "other", EntryFlag::Directory);
            const EntryId doubleFuzzy = index.add(other, "controls-v1-recheck", 0);
            const ParsedQuery q = parseQuery(u"core nsrch"_s);
            const auto out = searchNames(index, NameMatcher(q, true), {.limit = 10}, pool, {});
            QCOMPARE(out.totalMatches, std::size_t(2));
            QCOMPARE(out.hits[0].id, nsrch);
            QCOMPARE(out.hits[1].id, doubleFuzzy);
        }
        (void)exact;
        (void)normal2;
        (void)normal3;
        (void)normal4;
    }

    void appCatalogFuzzyModes()
    {
        AppList apps;
        AppInfo vscode;
        vscode.name = u"Visual Studio Code"_s;
        vscode.id = u"test.vscode"_s;
        vscode.target = u"C:\\VSCode\\Code.exe"_s;
        vscode.prepare();
        apps.push_back(vscode);

        AppInfo vcodeApp;
        vcodeApp.name = u"VCode Editor"_s;
        vcodeApp.id = u"test.vcode"_s;
        vcodeApp.target = u"C:\\VCode\\vcode.exe"_s;
        vcodeApp.prepare();
        apps.push_back(vcodeApp);

        // Off mode: "vcode" finds only literal VCode Editor, NOT Visual Studio Code.
        {
            const ParsedQuery q = parseQuery(u"vcode"_s);
            const NameMatcher m(q, false);
            const auto hits = searchApps(apps, q, m, {}, {}, FuzzyMode::Off);
            QCOMPARE(hits.size(), std::size_t(1));
            QCOMPARE(apps[hits[0].index].id, u"test.vcode"_s);
            QVERIFY(!hits[0].fuzzy);
        }

        // Off mode with explicit "fz:vcode": finds BOTH!
        {
            const ParsedQuery q = parseQuery(u"fz:vcode"_s);
            const NameMatcher m(q, false);
            const auto hits = searchApps(apps, q, m, {}, {}, FuzzyMode::Off);
            QCOMPARE(hits.size(), std::size_t(2));
            QVERIFY(hits[0].fuzzy && hits[1].fuzzy);
        }

        // Fallback & On modes: "vcode" finds literal VCode Editor FIRST, fuzzy Visual Studio Code SECOND!
        for (const FuzzyMode mode : {FuzzyMode::Fallback, FuzzyMode::On}) {
            const ParsedQuery q = parseQuery(u"vcode"_s);
            const NameMatcher m(q, mode == FuzzyMode::On);
            const auto hits = searchApps(apps, q, m, {}, {}, mode);
            QCOMPARE(hits.size(), std::size_t(2));
            QCOMPARE(apps[hits[0].index].id, u"test.vcode"_s);
            QVERIFY(!hits[0].fuzzy);
            QCOMPARE(apps[hits[1].index].id, u"test.vscode"_s);
            QVERIFY(hits[1].fuzzy);
        }

        // Quoted "vcode" disables app auto-fuzzy even in On mode.
        {
            const ParsedQuery q = parseQuery(u"\"vcode\""_s);
            const NameMatcher m(q, true);
            const auto hits = searchApps(apps, q, m, {}, {}, FuzzyMode::On);
            QCOMPARE(hits.size(), std::size_t(1));
            QCOMPARE(apps[hits[0].index].id, u"test.vcode"_s);
        }
    }

    // "tim" with fuzzy always on (a chat report, 2026-11-27): the app named
    // TIM comes before System Informer and RealTimeSync, whatever their order
    // in the catalog.
    void appNamedAsTypedComesFirstWithFuzzyOn()
    {
        AppList apps;
        const auto add = [&apps](const QString& name, const QString& id, const QString& target) {
            AppInfo app;
            app.name = name;
            app.id = id;
            app.target = target;
            app.prepare();
            apps.push_back(app);
        };
        add(u"System Informer"_s, u"test.si"_s, u"C:\\Program Files\\SystemInformer\\SystemInformer.exe"_s);
        add(u"RealTimeSync"_s, u"test.rts"_s, u"C:\\Program Files\\FreeFileSync\\RealTimeSync.exe"_s);
        add(u"TIM"_s, u"test.tim"_s, u"C:\\Program Files (x86)\\Tencent\\TIM\\Bin\\TIM.exe"_s);
        for (const FuzzyMode mode : {FuzzyMode::Off, FuzzyMode::Fallback, FuzzyMode::On}) {
            const ParsedQuery q = parseQuery(u"tim"_s);
            const NameMatcher m(q, mode == FuzzyMode::On);
            const auto hits = searchApps(apps, q, m, {}, {}, mode);
            QVERIFY(!hits.empty());
            QStringList order;
            for (const auto& hit : hits)
                order.append(apps[hit.index].name + (hit.fuzzy ? u"~"_s : QString()));
            qInfo().noquote() << int(mode) << order.join(u", "_s);
            QCOMPARE(apps[hits[0].index].id, u"test.tim"_s);
        }
        // System Informer opened every day, never from "tim": how much it is
        // used does not lift a fuzzy guess over the apps that have the letters.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        History history(dir.filePath(u"history.json"_s));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < 30; ++i)
            history.record(appLaunchPath(u"test.si"_s), u"sys"_s, now - i * 3'600'000LL);
        const ParsedQuery q = parseQuery(u"tim"_s);
        const NameMatcher m(q, true);
        const auto score = [&](const History::Affinities& affinities) {
            QHash<QString, int> scores;
            for (const AppHit& hit : searchApps(apps, q, m, history.items(), affinities, FuzzyMode::On))
                scores.insert(apps[hit.index].id, hit.score);
            return scores;
        };
        const QHash<QString, int> used = score(history.affinities(u"tim"_s, now));
        QVERIFY(used.contains(u"test.si"_s));
        QVERIFY2(used.value(u"test.si"_s) < used.value(u"test.rts"_s),
            qPrintable(u"System Informer %1, RealTimeSync %2"_s.arg(used.value(u"test.si"_s)).arg(used.value(u"test.rts"_s))));
        // Chosen for "tim" itself, it is what "tim" means to this user: first, as anything learned.
        for (int i = 0; i < 3; ++i)
            history.record(appLaunchPath(u"test.si"_s), u"tim"_s, now + i);
        const QHash<QString, int> learned = score(history.affinities(u"tim"_s, now + 10));
        QVERIFY(learned.value(u"test.si"_s) > learned.value(u"test.rts"_s));
    }

    void searchEngineEndToEndModes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        IndexService::Options opts;
        opts.content.enabled = false;
        IndexService service(dir.filePath(u"index"_s), opts);
        {
            auto idx = service.index();
            const auto lock = idx->writeLock();
            const EntryId root = idx->addRoot("T:");
            const EntryId src = idx->add(root, "src", EntryFlag::Directory);
            idx->add(src, "NameSearch.cpp", 0);
            idx->add(src, "report.txt", 0);
            idx->add(src, "report", EntryFlag::Directory);
            idx->add(src, "r_e_p_o_r_t_notes.txt", 0);
            idx->add(src, "avcodec.dll", 0);
            idx->add(src, "年度报告.docx", 0);
            idx->add(src, "年度报告2026.pdf", 0);
        }

        AppCatalog apps;
        auto appList = std::make_shared<AppList>();
        AppInfo vscode;
        vscode.name = u"Visual Studio Code"_s;
        vscode.id = u"test.vscode"_s;
        vscode.target = u"C:\\VSCode\\Code.exe"_s;
        vscode.prepare();
        appList->push_back(vscode);
        apps.setApps(appList);

        SearchEngine engine(&service, &apps, nullptr);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto run = [&](const QString& text, FuzzyMode mode, Scope scope = Scope::All) -> SearchResults {
            SearchEngine::Request req;
            req.text = text;
            req.scope = scope;
            req.fuzzyMode = mode;
            req.limit = 50;
            spy.clear();
            engine.submit(std::move(req));
            if (spy.isEmpty() && !spy.wait(5000))
                return {};
            return spy.first().at(1).value<SearchResults>();
        };

        // 1) Off mode: "nsrch" returns nothing; explicit "fz:nsrch" finds NameSearch.cpp (not approximate).
        QVERIFY(run(u"nsrch"_s, FuzzyMode::Off).isEmpty());
        {
            const auto rows = run(u"fz:nsrch"_s, FuzzyMode::Off);
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows[0].name, u"NameSearch.cpp"_s);
            QVERIFY(!rows[0].approximate);
            QVERIFY(rows[0].fuzzy);
        }

        // 2) Fallback mode:
        //    - Normal query "report" returns ONLY literal matches (no fuzzy r_e_p_o_r_t_notes.txt!).
        {
            const auto rows = run(u"report"_s, FuzzyMode::Fallback, Scope::Files);
            QCOMPARE(rows.size(), 2); // report.txt + report folder
            for (const auto& r : rows) {
                QVERIFY(!r.approximate);
                QVERIFY(!r.fuzzy);
            }
        }
        //    - 0-hit >=3 char query "nsrch" falls back to FZF and marks approximate + fuzzy.
        {
            const auto rows = run(u"nsrch"_s, FuzzyMode::Fallback);
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows[0].name, u"NameSearch.cpp"_s);
            QVERIFY(rows[0].approximate);
            QVERIFY(rows[0].fuzzy);
        }
        //    - 0-hit 2-char query "nr" does NOT trigger fuzzy noise.
        QVERIFY(run(u"nr"_s, FuzzyMode::Fallback).isEmpty());
        //    - Chinese typo "年度报高" prefers homophone fallback (approximate=true, fuzzy=false).
        {
            const auto rows = run(u"年度报高"_s, FuzzyMode::Fallback, Scope::Files);
            QVERIFY(!rows.isEmpty());
            QVERIFY(rows[0].approximate);
            QVERIFY(!rows[0].fuzzy);
        }
        //    - Chinese subsequence "报告26" falls through homophone to fuzzy fallback (approximate=true, fuzzy=true).
        {
            const auto rows = run(u"报告26"_s, FuzzyMode::Fallback, Scope::Files);
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows[0].name, u"年度报告2026.pdf"_s);
            QVERIFY(rows[0].approximate);
            QVERIFY(rows[0].fuzzy);
        }
        //    - "vcode" in Scope::All finds Visual Studio Code (app fuzzy) AND avcodec.dll (literal file).
        {
            const auto rows = run(u"vcode"_s, FuzzyMode::Fallback, Scope::All);
            QVERIFY(std::ranges::any_of(rows, [](const SearchResult& r) { return r.name == u"Visual Studio Code" && r.fuzzy; }));
            QVERIFY(std::ranges::any_of(rows, [](const SearchResult& r) { return r.name == u"avcodec.dll" && !r.fuzzy; }));
        }

        // 3) On mode: "report" lists normal file AND normal folder BEFORE fuzzy-only file!
        {
            const auto rows = run(u"report"_s, FuzzyMode::On, Scope::Files);
            QCOMPARE(rows.size(), 3);
            QVERIFY(!rows[0].fuzzy); // report.txt
            QVERIFY(!rows[1].fuzzy); // report (folder)
            QVERIFY(rows[2].fuzzy);  // r_e_p_o_r_t_notes.txt
            QVERIFY(!rows[2].approximate);
        }
    }
};

QTEST_GUILESS_MAIN(FuzzyMatchTest)
#include "tst_fuzzymatch.moc"
