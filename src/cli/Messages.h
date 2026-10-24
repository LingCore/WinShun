#pragma once

// What ws tells the user on stderr, in both languages. Settings are named as
// Win顺's settings window names them (zh: 设置 → AI 助手; en: Settings → AI
// assistants).

#include "Text.h"

namespace ws::cli::msg {

// ---- reaching Win顺 ---------------------------------------------------------
inline constexpr Text kPipeError {L"连不上 Win顺：{}", L"Cannot connect to WinShun: {}"};
inline constexpr Text kSwitchOff {
    L"Win顺 没有让命令行连接：在 Win顺 的 设置 → AI 助手 里打开“允许 AI 助手和命令行连接”",
    L"WinShun does not let the command line connect: turn on \"Allow AI assistants and the command line to connect\" in "
    L"WinShun's Settings → AI assistants"};
inline constexpr Text kRunningNoPipe {
    L"Win顺 在运行，但没有让命令行连接：在 设置 → AI 助手 里打开“允许 AI 助手和命令行连接”（已经开着的话，请把 Win顺 更新到最新版）",
    L"WinShun is running but does not let the command line connect: turn on \"Allow AI assistants and the command line to "
    L"connect\" in Settings → AI assistants (if it is on, update WinShun)"};
inline constexpr Text kNotRunningNoStart {L"Win顺 没在运行（--no-start：没有启动它）", L"WinShun is not running (--no-start: not started)"};
inline constexpr Text kStarting {L"Win顺 没在运行，正在启动…", L"WinShun is not running; starting it…"};
inline constexpr Text kCannotStart {L"Win顺 没在运行，也没能启动它（找不到它的开机自启任务和 WinShun.exe）",
    L"WinShun is not running and could not be started (found neither its autostart task nor WinShun.exe)"};
inline constexpr Text kStartTimedOut {L"启动了 Win顺，但它 {} 秒内没有回应；稍后再试", L"Started WinShun, but it did not answer within {} seconds; try again in a moment"};
inline constexpr Text kStartingWait {L"Win顺 正在启动，等它准备好…", L"WinShun is starting; waiting for it…"};
inline constexpr Text kNoAnswer {L"Win顺 {} 秒没有回答（它可能卡住了）；稍后再试", L"WinShun has not answered for {} seconds (it may be stuck); try again in a moment"};
inline constexpr Text kWinShunTooOld {L"这个版本的 Win顺 不认 {}：请把 Win顺 更新到最新版", L"This WinShun does not understand {}: update WinShun to the latest version"};
inline constexpr Text kBroken {L"和 Win顺 的连接断了", L"The connection to WinShun broke"};
inline constexpr Text kServerError {L"Win顺 回答：{}", L"WinShun answered: {}"};
inline constexpr Text kNoResult {L"Win顺 的回答里没有结果", L"WinShun's answer has no result in it"};
inline constexpr Text kAnswerTooLarge {L"Win顺 的回答太大，一次传不过来（最多 16 MB）：用 -n 少要一些，或把查询写细一点",
    L"WinShun's answer is too large to send (16 MB at most): ask for fewer with -n, or narrow the query"};

// ---- the index -------------------------------------------------------------
inline constexpr Text kLoading {L"正在加载索引…", L"Loading the index…"};
inline constexpr Text kLoadingPercent {L"正在加载索引 {}%…", L"Loading the index, {}%…"};
inline constexpr Text kNotReady {L"索引还没准备好（等了 {} 秒）；稍后再试，或用 --wait 60s 多等一会儿",
    L"The index is not ready yet (waited {} s); try again later, or wait longer with --wait 60s"};
inline constexpr Text kNotReadyNoWait {L"索引还没准备好；用 --wait 30s 等它", L"The index is not ready yet; wait for it with --wait 30s"};
inline constexpr Text kRefreshing {L"索引正在更新，刚改过的文件可能还没有", L"The index is catching up: files changed just now may be missing"};

// ---- results -----------------------------------------------------------------
inline constexpr Text kTruncated {L"只列了前 {} 个，用 -n 0 看全部，或把查询写细一点", L"Only the first {} are listed: -n 0 lists all, or narrow the query"};
inline constexpr Text kTruncatedOf {L"只列了前 {} 个（共 {} 个），用 -n 0 看全部", L"Only the first {} of {} are listed: -n 0 lists all"};
inline constexpr Text kTruncatedMax {L"只列了前 {} 个（一次最多这么多），把查询写细一点", L"Only the first {} are listed (the most at a time); narrow the query"};
inline constexpr Text kServerCapped {L"只列了前 {} 个：这个版本的 Win顺 一次最多给这么多，更新后可以多给",
    L"Only the first {} are listed: this WinShun gives no more at a time (update it for more)"};
inline constexpr Text kLossy {L"有 {} 行含代码页 {} 写不出的字，已写成 ?；用 --encoding utf8 或 --json 可以保留",
    L"{} line(s) had characters code page {} cannot hold, written as ?; --encoding utf8 or --json keeps them"};
inline constexpr Text kIncomplete {L"搜到时间上限就停了，结果可能不全；用 --timeout 30s 多搜一会儿",
    L"The search stopped at its time limit, so results may be missing; --timeout 30s searches longer"};

// ---- what Win顺 said no to -------------------------------------------------
inline constexpr Text kNotVisible {L"看不到这个路径（不存在、没有权限，或在不让 AI 看到的文件夹里）：{}",
    L"This path cannot be seen (it does not exist, may not be read, or is in a folder kept from AI): {}"};
inline constexpr Text kNotIndexed {L"这个文件夹不在索引里（不存在、被排除，或所在的盘没有索引）：{}",
    L"This folder is not in the index (it does not exist, is excluded, or is on a drive that is not indexed): {}"};
inline constexpr Text kIsFile {L"这是文件，不是文件夹：{}", L"This is a file, not a folder: {}"};
inline constexpr Text kIsFolder {L"这是文件夹，用 ws ls 列出它：{}", L"This is a folder; list it with ws ls: {}"};
inline constexpr Text kTurnedOff {L"Win顺 的设置里关着它（设置 → AI 助手）：{}", L"It is turned off in WinShun's settings (Settings → AI assistants): {}"};
inline constexpr Text kClipOff {L"剪贴板历史没有对命令行和 AI 开放：在 设置 → AI 助手 里打开“允许 AI 读取剪贴板历史”",
    L"The clipboard history is not open to the command line and AI: turn on \"Allow AI to read the clipboard history\" in "
    L"Settings → AI assistants"};
inline constexpr Text kClipUnavailable {L"Win顺 的剪贴板历史没开（设置 → 剪贴板）", L"WinShun's clipboard history is off (Settings → Clipboard)"};
inline constexpr Text kContentOff {L"Win顺 的文件内容搜索关着：在 设置 → 文件内容搜索 里打开",
    L"Content search is off in WinShun: turn it on in Settings → File contents"};
inline constexpr Text kUnknownTool {L"这个版本的 Win顺 没有 {}；请更新 Win顺", L"This WinShun has no {}; update it"};
inline constexpr Text kNotText {L"这不是 Win顺 能读的文本文件或文档：{}", L"This is not a text file or a document WinShun can read: {}"};
inline constexpr Text kTooLarge {L"文件超过了 Win顺 对这类文件的大小上限（设置 → 文件内容搜索）：{}",
    L"The file is larger than WinShun's size limit for its kind (Settings → File contents): {}"};
inline constexpr Text kCloudOnly {L"这是云端文件，不在这台电脑上，读它会先下载；先打开它下载下来：{}",
    L"This is a cloud file that is not on this PC; open it first to download it: {}"};
inline constexpr Text kUnreadable {L"读不了这个文件（被占用、没有权限或已损坏）：{}", L"The file could not be read (in use, no access, or damaged): {}"};

// ---- the commands' own -------------------------------------------------------
inline constexpr Text kNeedsQuery {L"ws {} 要一个查询，比如 ws {} \"报告\"", L"ws {} needs a query, such as ws {} \"report\""};
inline constexpr Text kNeedsFile {L"ws text 要一个文件，比如 ws text \"D:\\文档\\计划.docx\"", L"ws text needs a file, such as ws text \"D:\\Docs\\plan.docx\""};
inline constexpr Text kOneFile {L"ws text 一次读一个文件", L"ws text reads one file at a time"};
inline constexpr Text kOneFolder {L"ws ls 一次列一个文件夹", L"ws ls lists one folder at a time"};
inline constexpr Text kRecentNoWords {L"ws recent 不接查询词：限定文件夹用 --in，限定类型用 --ext", L"ws recent takes no query: --in keeps to a folder, --ext to types"};
inline constexpr Text kOneIn {L"ws {} 只能有一个 --in", L"ws {} takes one --in"};
inline constexpr Text kRecentHiddenPlaces {L"这里改过的只有系统、程序或工具文件夹里的文件（或隐藏文件）：平时不列，这次一并列出",
    L"Only files in system, program or tool folders (or hidden ones) changed here: usually left out, listed this time"};
inline constexpr Text kFilesOrFolders {L"--files 和 --folders 只能选一个", L"Use --files or --folders, not both"};
inline constexpr Text kNoFolder {L"没有这个文件夹：{}", L"There is no such folder: {}"};
inline constexpr Text kMissingQuery {L"少了要找的东西；ws help 看用法", L"Nothing to look for; see ws help"};
inline constexpr Text kNotFound {L"没找到", L"Nothing found"};
inline constexpr Text kOpenFailed {L"打不开 {}：{}", L"Could not open {}: {}"};
inline constexpr Text kOpenNotElevated {L"ws 以管理员身份运行，没能交给资源管理器打开 {}（搜到的文件不会用管理员权限打开，所以没有直接打开）",
    L"ws runs as administrator and could not hand {} to Explorer to open (a file found by a search is not opened with administrator rights, so it was not opened directly)"};
inline constexpr Text kServerUnknown {L"看不出和 ws 连着的是哪个程序，没有打开任何东西", L"Cannot tell which program is at the other end of the pipe: nothing was opened"};
inline constexpr Text kNotWinShun {L"管道那头不是 Win顺（是 {}），没有打开任何东西", L"The program at the other end of the pipe is not WinShun ({}): nothing was opened"};
inline constexpr Text kMoreText {L"还有 {} 字没读，用 --from {} 接着读", L"{} more characters: go on with --from {}"};
inline constexpr Text kReadPastEnd {L"已经读完了：全文共 {} 字，--from {} 在它后面", L"Nothing left to read: the text has {} characters and --from {} is past them"};
inline constexpr Text kTextCut {L"文件很长，Win顺 只读了前面的部分（16 MB 文字）", L"The file is long: WinShun read only its first part (16 MB of text)"};
inline constexpr Text kNoTextDocument {L"没有读出文字：可能是扫描件（整页都是图片）", L"No text in it: perhaps a scanned document (pictures of pages)"};
inline constexpr Text kEmptyFile {L"文件是空的", L"The file is empty"};
inline constexpr Text kSkillInstalled {L"装好了：Claude Code 从下一个对话起就会用它", L"Installed: Claude Code uses it from its next conversation"};
inline constexpr Text kSkillWriteFailed {L"写不进 {}：{}", L"Could not write {}: {}"};
inline constexpr Text kSkillMissing {L"这个 ws.exe 里没有 SKILL.md", L"This ws.exe has no SKILL.md in it"};

} // namespace ws::cli::msg
