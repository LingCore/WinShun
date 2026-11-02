# GitHub #36: the content index on a hard disk — checks from the MFT

## What the reporter's diagnostics showed (0.17.0, 2026-11-02)

The diagnostics bundle has `events.jsonl`, `summary.json` and the service log. It holds no file contents.

- The pass was not a rebuild. When it started, the index already had 644,749 files and 26,773 document texts. It counted 382,555 candidates. 379,436 of them were pending known documents: almost all were to **check** (`Need::Check`), and a few were dirty. The other ~3,000 were new.
- Over the first 8.7 minutes (01:39:12 → 01:47:52), `indexed_files` and `document_texts` stayed exactly the same while `done` rose to 379,964. All of that time went to checks: one `GetFileAttributesExW` per file, at 800–1,000 files a second, with `busy_readers` 1 throughout (one hard-disk slot, quiet mode, user present). `bulk_sessions` was 0, because the bulk path only took text files being read for the first time.
- The search window paused the pass four times (`paused-search-window` / `waiting-after-search-window`), about 107 s in total.
- The last ~2.5 minutes read about 1,200 new files on E:, mostly PDFs. Six `content-slow-read` events took 3.6–11.3 s each. They were last in line because new files are sorted after known ones.
- Why everything was "to check": on 2026-11-01 the service log shows fresh MFT reads of C: and D:, because their journals were no longer current after four days off. `IndexService::contentUnsureOf` marked every document on them. That day's pass was deferred (`file-index-not-ready`), and Win顺 quit two minutes later, so the marks were still there on 11-02. On 10-28 all four volumes were MFT-read and that pass was cut short at 246,782 of 382,625. The pattern repeats whenever a journal is lost.
- All of C:, D: and E: share one hard-disk queue. Only one file was ever in progress, and E:'s work came only after D:'s, so they are on one physical disk.
- Memory: the user's ~360 MB is WinShun.exe's, taken during the pass. IdleTrim waits until the pass ends (`idle-trim: waiting, reading content`). The service is 1–10 MB. For comparison, `wsbench --stats` here: 4.23 M entries take 176 MB private (43.6 B per entry). The reporter's 3.71 M entries come to roughly 160 MB for the file index alone. This machine's own WinShun (4.23 M entries) shows 367 MB private working set, so 360 MB is in line and not a leak. Nothing changed for memory.

## Change

`ContentIndexer::pass`: on a volume with at least `kBulkCheckThreshold` (2,000) files to check, the bulk session's single MFT scan also answers the checks.

- If the record's size and `$STANDARD_INFORMATION` write time give the stamp that was stored, the file is confirmed and not opened.
- If the stamp differs, a changed text file is fetched in the same session; a changed document goes to the extractor the usual way.
- Not found, denied, fallback, and files written in the last 5 minutes are still checked by path.
- On one hard disk, only one session scans its MFT at a time.
- New: `BulkStats::confirmed` and the diagnostics field `bulk_confirmed`. `WINSHUN_BULK_CHECK=0` turns the change off for comparison; `WINSHUN_BULK_VERIFY` also checks each confirmed file by path.

## Tests

- `tst_core contentChecksFromRecords` (new) uses a fake bulk source that serves the files of a temp folder. It has 2,100 text files plus a PDF, all marked unsure: one changed, one deleted, and one "written lately". It checks one scan, 2,098 confirmed by record, the changed file fetched and indexed with its new text, the deleted one still to check, and the recent one checked by path. The test fails when `WINSHUN_BULK_CHECK=0` (no scan).
- `tst_core` (whole), `tst_diagnostics`, `tst_remotecontent`: see the report.

## Live measurement (wsbench through the stage's WinShunService console, `--only W:`)

The test volume is a 4 GB .vhd on C: (SSD), NTFS 4K, attached as W:. It holds 80,000 generated text files of mixed kinds and sizes (lognormal, median about 1.8 KB, some crowded folders), 1,500 PDFs made unique by a trailer, and 500 docx copies. No user files. The content index was on for W: only; the other drives were excluded.

- `--save-unsure D` builds the index and saves it with all 82,000 documents marked to check.
- Then for each mode: detach and attach the VHD again (cold cache), and run `--from D --only W:` with `WINSHUN_USER=present`.

| mode | per-path checks | check pass |
|---|---|---|
| hard disk (`WINSHUN_SPINNING=W:`), `WINSHUN_BULK_CHECK=0` (old) | 82,000 | 6.7 s |
| hard disk, new | 0 (82,000 confirmed by record) | under 0.25 s (service: MFT 83,968 records in 72 ms, session 215 ms) |
| SSD, old | 82,000 | 3.8 s |
| SSD, new | 0 | under 0.25 s (MFT in 82 ms, session 238 ms) |

A warm-cache run with `WINSHUN_BULK_VERIFY=1` gave 80,000 confirmed by record, every one checked by path too, and 0 differ.

**Not reproducible here:** hard-disk seek latency. The VHD sits on an SSD, so a cold path check costs about 80 µs, where the reporter's log shows about 1.1 ms. The proxy is the number of per-path checks avoided (82,000 → 0). For the reporter's D:, the projection is about 330 k checks, ~6–7 min one at a time, replaced by one MFT scan that their own service log timed at 15–30 s for its 2.58 M records.
