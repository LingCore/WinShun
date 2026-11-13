#include "Commands.h"

#include "Encoding.h"
#include "Messages.h"
#include "Win.h"

#include <array>

namespace ws::cli {

namespace {

constexpr Opt kFindOptions[] = {Opt::Limit, Opt::Files, Opt::Folders, Opt::Ext, Opt::In, Opt::Sort, Opt::Long, Opt::Json, Opt::Null};
constexpr Opt kGrepOptions[] = {Opt::Limit, Opt::In, Opt::Timeout, Opt::Json, Opt::Null};
constexpr Opt kRecentOptions[] = {Opt::Limit, Opt::Days, Opt::Ext, Opt::In, Opt::Files, Opt::Folders, Opt::Long, Opt::Json, Opt::Null};
constexpr Opt kTextOptions[] = {Opt::From, Opt::Max, Opt::Json};
constexpr Opt kLsOptions[] = {Opt::Limit, Opt::Files, Opt::Folders, Opt::Long, Opt::Json, Opt::Null};
constexpr Opt kOpenOptions[] = {Opt::Reveal, Opt::Files, Opt::Folders, Opt::Ext, Opt::In, Opt::Sort};
constexpr Opt kClipOptions[] = {Opt::Limit, Opt::Kind, Opt::Json, Opt::Null};
constexpr Opt kStatusOptions[] = {Opt::Json};
// status says what is there: it does not wait for the index (--wait) and never starts Win顺 (--no-start).
constexpr Opt kStatusShared[] = {Opt::Color, Opt::Encoding, Opt::Help, Opt::Version, Opt::Pipe, Opt::Instance};
constexpr Opt kSkillOptions[] = {Opt::Install};
constexpr Opt kPluginOptions[] = {Opt::Json};

const Command kCommands[] = {
    {L"find", {L"按名字找文件和文件夹（默认命令）", L"Find files and folders by name (the default command)"},
        {L"ws [find] <查询> [选项]", L"ws [find] <query> [options]"},
        {L"多个词都要命中（在名字里，或在上级文件夹的名字里）；中文名也能用拼音找（bg、baogao 都能找到 报告）。\n"
         L"\"引号\" 里是整个短语；!词 排除；*.pdf 和 ? 是通配符；ext:pdf,docx 只要这些扩展名；"
         L"文件夹\\名字 要求上级文件夹的名字含“文件夹”；D:\\项目\\ 报告 只在这个文件夹里找。\n"
         L"查询要加引号：bash 会先把 * 换成当前文件夹里的文件名。第一个词正好是命令名时（recent、ls…），"
         L"写 ws find recent 或 ws -- recent（-- 之后的一切都是查询词，选项要写在 -- 前面）。",
            L"All words must match, in the name or in a parent folder's name; Chinese names match pinyin too (bg or baogao "
            L"finds 报告).\n"
            L"\"Quotes\" keep a phrase together; !word excludes; *.pdf and ? are wildcards; ext:pdf,docx keeps those "
            L"extensions; folder\\name needs a parent folder named like \"folder\"; D:\\Projects\\ report searches in that "
            L"folder.\n"
            L"Quote the query: bash turns * into the names in the current folder first. When the first word is a command's "
            L"name (recent, ls...), write ws find recent or ws -- recent (after -- everything is the query: options go before it)."},
        {L"ws \"周报\"\nws find \"report\" --ext pdf,docx -n 20\nws find \"*.psd\" --in . --sort newest -l",
            L"ws \"weekly report\"\nws find \"report\" --ext pdf,docx -n 20\nws find \"*.psd\" --in . --sort newest -l"},
        kFindOptions, true, runFind},
    {L"grep", {L"按内容找：文件里的文字，Word、Excel、PPT、PDF 也行", L"Find by content: the text in files, Word, Excel, PowerPoint and PDF too"},
        {L"ws grep <文字> [选项]", L"ws grep <text> [options]"},
        {L"几个词都要出现在文件里；\"引号\" 里是整个短语，一行代码照原样找。\n"
         L"\"马上|立即\" 是有一个就行；regex: 开头的词是正则表达式（在一行之内匹配，不分大小写）。\n"
         L"每个结果一行：路径:行号:那一行；文档写页 p3、幻灯片 s2、表格 [表名]r12。\n"
         L"比按名字找慢：到 --timeout 就停，给出已经找到的。",
            L"All words must occur in the file; \"quotes\" keep a phrase together, a line of code is found as it is.\n"
            L"\"now|soon\" is either of them; a word that starts with regex: is a regular expression (within a line, any case).\n"
            L"One result a line: path:line:the line; documents give the page p3, slide s2 or sheet row [Sheet]r12.\n"
            L"Slower than finding by name: it stops at --timeout with what it found."},
        {L"ws grep \"第三季度 销售额\" -n 20\nws grep \"TODO\" --in . --timeout 10s\nws grep \"regex:合同编号[A-Z]\\d+\"",
            L"ws grep \"quarterly revenue\" -n 20\nws grep \"TODO\" --in . --timeout 10s\nws grep \"regex:invoice-\\d{4}\""},
        kGrepOptions, true, runGrep},
    {L"recent", {L"最近改过的文件，最新的在前", L"Files changed lately, newest first"}, {L"ws recent [选项]", L"ws recent [options]"},
        {L"不接查询词：--days 限定天数，--in 限定文件夹，--ext 限定类型，--folders 列文件夹。",
            L"No query: --days keeps to the last days, --in to a folder, --ext to types; --folders lists folders."},
        {L"ws recent --days 3\nws recent --in . --ext docx,xlsx -l", L"ws recent --days 3\nws recent --in . --ext docx,xlsx -l"}, kRecentOptions, true,
        runRecent},
    {L"text", {L"读出文件里的文字（docx、xlsx、pptx、pdf 也行）", L"Print a file's text (docx, xlsx, pptx and pdf too)"},
        {L"ws text <文件> [选项]", L"ws text <file> [options]"},
        {L"文档的文字带 [page 3] 这样的位置行。默认最多读 20000 字；没读完时会说从哪接着读（--from）。",
            L"A document's text comes with lines such as [page 3]. At most 20000 characters unless --max says otherwise; when "
            L"there is more, ws says where to go on from (--from)."},
        {L"ws text \"D:\\文档\\计划.docx\"\nws text 报告.pdf --from 20000 --max 0", L"ws text \"D:\\Docs\\plan.docx\"\nws text report.pdf --from 20000 --max 0"},
        kTextOptions, true, runText},
    {L"ls", {L"列出文件夹里的东西（从索引，大文件夹也快）", L"List a folder (from the index: fast even for huge folders)"},
        {L"ws ls [文件夹] [选项]", L"ws ls [folder] [options]"},
        {L"不写文件夹就是当前文件夹。文件夹在前，文件在后，按名字排。", L"The current folder unless one is given. Folders first, then files, by name."},
        {L"ws ls\nws ls \"D:\\项目\" --files -l", L"ws ls\nws ls \"D:\\Projects\" --files -l"}, kLsOptions, true, runLs},
    {L"open", {L"打开最匹配的那个（和在启动器里按 Enter 一样）", L"Open the best match (as Enter does in the launcher)"},
        {L"ws open <查询或路径> [选项]", L"ws open <query or path> [options]"},
        {L"是已有的路径就直接打开，否则按名字找，打开第一个，并打印打开的路径。--reveal 在文件夹里选中它，不打开。"
         L"用你自己的权限打开，和双击一样。",
            L"An existing path is opened as it is; anything else is searched for by name and the first match opened. The path "
            L"opened is printed. --reveal selects it in its folder instead. Opened with your own rights, as a double-click."},
        {L"ws open \"周报\" --ext docx --sort newest\nws open . --reveal", L"ws open \"weekly report\" --ext docx --sort newest\nws open . --reveal"},
        kOpenOptions, true, runOpen},
    {L"clip", {L"剪贴板历史，最新的在前", L"The clipboard history, newest first"}, {L"ws clip [查询] [选项]", L"ws clip [query] [options]"},
        {L"要先在 Win顺 里打开两个开关：设置 → 剪贴板 里的“记录剪贴板历史”，和 设置 → AI 助手 里的“允许 AI 读取剪贴板历史”。"
         L"每条一行，换行写成 \\n；-0 给出完整的文字。",
            L"Needs two switches turned on in WinShun: \"Keep a clipboard history\" in Settings > Clipboard and \"Allow AI to read the "
            L"clipboard history\" in Settings > AI assistants. One entry a line, line breaks written as \\n; -0 gives the whole texts."},
        {L"ws clip -n 5\nws clip \"http\" --kind link", L"ws clip -n 5\nws clip \"http\" --kind link"}, kClipOptions, true, runClip},
    {L"plugin", {L"插件：列出、在插件库里找、安装、开关、卸载", L"Plugins: list, search the library, install, switch, remove"},
        {L"ws plugin <list | search [词] | install <包、文件夹或 id> | enable <id> | disable <id> | remove <id>> [选项]",
            L"ws plugin <list | search [words] | install <package, folder or id> | enable <id> | disable <id> | remove <id>> [options]"},
        {L"list 列出装了哪些；search 在插件库里找（不写词就全列出来）；install 装一个插件包（.wsplugin、.zip）、一个插件文件夹，"
         L"或插件库里的一个 id；enable、disable 开关；remove 卸载。\n"
         L"自己的包和文件夹当场装好：新装的先关着，盖在开着的版本上就直接换上。要你点头的事，Win顺 在它自己的窗口里问你，"
         L"ws 不等：第一次打开一个插件（它要哪些权限）、插件库里没人审核过的包、卸载。从插件库安装时，下载进度也在那个窗口里。\n"
         L"退出码：0 办好了；1 Win顺 正在问你，答完用 ws plugin list 看结果（search 是没找到）；6 没有这个插件或文件；7 没办成。",
            L"list shows what is installed; search looks in the plugin library (everything, without words); install takes a "
            L"package (.wsplugin, .zip), a plugin's folder, or an id of the library; enable and disable are its switch; remove "
            L"uninstalls it.\n"
            L"A package or folder of your own is installed at once: a new plugin stays off, one put over a version that is on "
            L"is on. What needs your yes, WinShun asks in its own window, and ws does not wait: a plugin turned on for the "
            L"first time (the permissions it asks for), a package of the library nobody reviewed, a removal. A download from "
            L"the library shows in that window too.\n"
            L"Exit codes: 0 done; 1 WinShun is asking you: see ws plugin list once you answered (search: nothing found); 6 no "
            L"such plugin or file; 7 it could not be done."},
        {L"ws plugin list\nws plugin search 翻译\nws plugin install winshun.translate\nws plugin install .\\dist\\me.hello-1.0.0.wsplugin\nws plugin enable me.hello",
            L"ws plugin list\nws plugin search translate\nws plugin install winshun.translate\nws plugin install .\\dist\\me.hello-1.0.0.wsplugin\nws plugin enable me.hello"},
        kPluginOptions, true, runPlugin},
    {L"status", {L"Win顺 在不在运行、索引好了没", L"Whether WinShun runs and its index is ready"}, {L"ws status [选项]", L"ws status [options]"},
        {L"不会启动 Win顺，也不等索引（所以没有 --wait 和 --no-start）；只在 Win顺 刚启动、管道还没出来时多等几秒。"
         L"退出码：0 就绪，5 索引还没好，3 没在运行，4 连接关着。",
            L"Never starts WinShun and never waits for the index (so no --wait or --no-start); it waits a few seconds only for a "
            L"WinShun that has just started and has no pipe yet. Exit code: 0 ready, 5 the index is not ready yet, 3 not "
            L"running, 4 the connection is off."},
        {L"ws status\nws status --json", L"ws status\nws status --json"}, kStatusOptions, true, runStatus, kStatusShared},
    {L"skill", {L"给 AI 用的说明（SKILL.md）", L"The instructions for AI agents (SKILL.md)"}, {L"ws skill [选项]", L"ws skill [options]"},
        {L"打印 SKILL.md。--install claude 把它装进 Claude Code：%USERPROFILE%\\.claude\\skills\\winshun\\SKILL.md"
         L"（设了 CLAUDE_CONFIG_DIR 就装在那里）。",
            L"Prints SKILL.md. --install claude installs it for Claude Code: %USERPROFILE%\\.claude\\skills\\winshun\\SKILL.md "
            L"(under CLAUDE_CONFIG_DIR when that is set)."},
        {L"ws skill\nws skill --install claude", L"ws skill\nws skill --install claude"}, kSkillOptions, false, runSkill},
    {L"help", {L"看帮助：ws help <命令>", L"Help: ws help <command>"}, {L"ws help [命令]", L"ws help [command]"}, {}, {L"ws help grep", L"ws help grep"},
        {}, false, runHelp},
};

constexpr long long kDefaultLimit = 100;

std::vector<std::wstring> splitList(std::wstring_view text)
{
    std::vector<std::wstring> parts;
    std::wstring current;
    const auto push = [&] {
        std::wstring_view part = current;
        while (!part.empty() && (part.front() == L' ' || part.front() == L'.'))
            part.remove_prefix(1);
        while (!part.empty() && part.back() == L' ')
            part.remove_suffix(1);
        if (!part.empty())
            parts.emplace_back(part);
        current.clear();
    };
    for (const wchar_t c : text) {
        if (c == L',' || c == L';' || c == L'\x3001' || c == L'\xFF0C') // , ; 、 ，
            push();
        else
            current += c;
    }
    push();
    return parts;
}

} // namespace

std::span<const Command> commandTable()
{
    return kCommands;
}

const Command* commandNamed(std::wstring_view word)
{
    const auto same = [&](std::wstring_view name) {
        return !name.empty()
            && ::CompareStringOrdinal(word.data(), static_cast<int>(word.size()), name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
    };
    for (const Command& command : kCommands) {
        if (same(command.name))
            return &command;
    }
    return nullptr;
}

const Command& defaultCommand()
{
    return kCommands[0];
}

std::vector<std::wstring> extensionList(const Parsed& args)
{
    std::vector<std::wstring> list;
    for (const std::wstring& value : args.all(Opt::Ext)) {
        for (std::wstring& ext : splitList(value))
            list.push_back(std::move(ext));
    }
    return list;
}

std::wstring buildQuery(const Parsed& args)
{
    std::wstring query;
    for (const std::wstring& word : args.words) {
        if (!query.empty())
            query += L' ';
        query += word;
    }
    std::wstring extensions;
    for (const std::wstring& ext : extensionList(args)) {
        if (!extensions.empty())
            extensions += L',';
        extensions += ext;
    }
    if (!extensions.empty()) {
        if (!query.empty())
            query += L' ';
        query += L"ext:" + extensions;
    }
    return query;
}

Result<void> noQuoteIn(std::wstring_view path)
{
    // No path has a '"' in it, so one here is a quote that cmd and PowerShell 5.1 did not take as
    // one: "C:\My Folder\" -n 5 arrives as the single argument  C:\My Folder" -n 5  (the
    // backslash escapes the quote). Said, rather than looked for as a folder of that name.
    if (path.find(L'"') != std::wstring_view::npos)
        return Failure {Exit::Usage, fmt(msg::kQuoteInPath, path)};
    return {};
}

Result<std::vector<std::wstring>> inFolders(const Parsed& args)
{
    std::vector<std::wstring> folders;
    for (const std::wstring& given : args.all(Opt::In)) {
        if (Result<void> clean = noQuoteIn(given); !clean)
            return std::move(clean).failure();
        std::wstring path = absolutePath(given);
        while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
            path.pop_back();
        switch (pathKind(path)) {
        case PathKind::None:
            return Failure {Exit::NotVisible, fmt(msg::kNoFolder, path)};
        case PathKind::File: // the wrong thing named, as ws ls <file>
            return Failure {Exit::Usage, fmt(msg::kIsFile, path)};
        case PathKind::Folder:
            break;
        }
        folders.push_back(std::move(path));
    }
    return folders;
}

Result<std::string> kindOf(const Parsed& args, std::string_view fallback)
{
    const bool files = args.has(Opt::Files);
    const bool folders = args.has(Opt::Folders);
    if (files && folders)
        return Failure {Exit::Usage, std::wstring(tr(msg::kFilesOrFolders))};
    if (files)
        return std::string("files");
    if (folders)
        return std::string("folders");
    return std::string(fallback);
}

Result<long long> limitOf(const Parsed& args, long long most)
{
    Result<long long> n = integerOption(args, Opt::Limit, kDefaultLimit, 0, most);
    if (!n)
        return n;
    return *n == 0 ? most : *n;
}

} // namespace ws::cli
