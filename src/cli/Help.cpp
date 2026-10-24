// help: the overview, one command's, and the short one for ws alone.

#include "Commands.h"
#include "Format.h"
#include "Messages.h"

#include <algorithm>

namespace ws::cli {

namespace {

constexpr Text kTitle {L"ws：在命令行里用 Win顺 搜索——文件名、文件里的文字、文档的内容",
    L"ws: search with WinShun from the command line: file names, the text in files, documents"};
constexpr Text kUsage {L"用法：", L"Usage: "};
constexpr Text kUsageFind {L"ws <查询> [选项]", L"ws <query> [options]"};
constexpr Text kUsageFindNote {L"按名字找（就是 ws find）", L"find by name (that is ws find)"};
constexpr Text kUsageCommand {L"ws <命令> [参数] [选项]", L"ws <command> [arguments] [options]"};
constexpr Text kCommands {L"命令：", L"Commands:"};
constexpr Text kOptions {L"选项：", L"Options:"};
constexpr Text kCommonOptions {L"常用选项：", L"Common options:"};
constexpr Text kEveryCommandTitle {L"通用选项：", L"Options of every command:"};
constexpr Text kExamples {L"例子：", L"Examples:"};
constexpr Text kNotes {
    L"查询要加引号（bash 会先把 * 换成文件名）。第一个词正好是命令名时，写 ws find <词> 或 ws -- <词>。\n"
    L"结果写到 stdout，提示写到 stderr（以 ws: 开头）。\n"
    L"退出码：0 有结果，1 没有结果，2 用法不对，3 Win顺 没在运行，4 连接关着，5 索引还没好，6 路径看不到，7 其他错误，130 Ctrl+C。\n"
    L"PowerShell：在 profile 里加 [Console]::OutputEncoding = [Text.UTF8Encoding]::new()，ws 的输出就一直是 UTF-8。\n"
    L"每个命令的用法：ws help <命令>",
    L"Quote the query (bash turns * into file names first). When the first word is a command's name, write ws find <word> "
    L"or ws -- <word>.\n"
    L"Results go to stdout, hints to stderr (starting with ws:).\n"
    L"Exit codes: 0 found, 1 nothing found, 2 wrong usage, 3 WinShun not running, 4 the connection is off, 5 the index is not "
    L"ready, 6 the path cannot be seen, 7 other errors, 130 Ctrl+C.\n"
    L"PowerShell: put [Console]::OutputEncoding = [Text.UTF8Encoding]::new() in your profile and ws writes UTF-8 there too.\n"
    L"One command's usage: ws help <command>"};
constexpr Text kShort {
    L"ws：在命令行里用 Win顺 搜索\n"
    L"  ws \"周报\"                 按名字找（拼音也行）\n"
    L"  ws grep \"销售额\"          按内容找（Word、Excel、PPT、PDF 也行）\n"
    L"  ws text 计划.docx         读出文档的文字\n"
    L"  ws recent --days 3        最近改过的文件\n"
    L"  ws help                   全部命令和选项",
    L"ws: search with WinShun from the command line\n"
    L"  ws \"report\"               find by name (pinyin too)\n"
    L"  ws grep \"revenue\"         find by content (Word, Excel, PowerPoint, PDF too)\n"
    L"  ws text plan.docx         print a document's text\n"
    L"  ws recent --days 3        files changed lately\n"
    L"  ws help                   every command and option"};

constexpr Opt kOverviewOptions[] = {Opt::Limit, Opt::Long, Opt::Json, Opt::Null, Opt::In, Opt::Ext, Opt::Files, Opt::Folders, Opt::Sort,
    Opt::Wait, Opt::NoStart, Opt::Color, Opt::Encoding, Opt::Help, Opt::Version};
constexpr Opt kEveryCommand[] = {Opt::Wait, Opt::NoStart, Opt::Color, Opt::Encoding, Opt::Help};

std::wstring optionLabel(const OptionSpec& spec)
{
    std::wstring label = spec.letter ? std::wstring {L'-', spec.letter, L',', L' '} : std::wstring(L"    ");
    label += L"--";
    label += spec.name;
    if (spec.takesValue) {
        label += L" <";
        label += tr(spec.valueName);
        label += L'>';
    }
    return label;
}

// Two columns, the second lined up (wide characters count two).
void appendTable(std::wstring& text, const std::vector<std::pair<std::wstring, std::wstring>>& rows, std::wstring_view indent = L"  ")
{
    std::size_t width = 0;
    for (const auto& row : rows)
        width = std::max(width, displayWidth(row.first));
    for (const auto& [left, right] : rows) {
        text += indent;
        text += left;
        if (!right.empty()) {
            text.append(width - displayWidth(left) + 3, L' ');
            text += right;
        }
        text += L'\n';
    }
}

void appendOptions(std::wstring& text, std::span<const Opt> options)
{
    std::vector<std::pair<std::wstring, std::wstring>> rows;
    for (const Opt id : options) {
        const OptionSpec& spec = optionSpec(id);
        if (!spec.hidden)
            rows.emplace_back(optionLabel(spec), std::wstring(tr(spec.help)));
    }
    appendTable(text, rows);
}

void appendIndented(std::wstring& text, std::wstring_view lines)
{
    while (!lines.empty()) {
        const std::size_t end = lines.find(L'\n');
        text += L"  ";
        text += lines.substr(0, end);
        text += L'\n';
        lines.remove_prefix(end == std::wstring_view::npos ? lines.size() : end + 1);
    }
}

} // namespace

std::wstring shortHelp()
{
    return std::wstring(tr(kShort));
}

std::wstring helpText(const Command* command)
{
    std::wstring text;
    if (!command) {
        text += tr(kTitle);
        text += L"\n\n";
        const std::wstring usage(tr(kUsage));
        const std::wstring indent(displayWidth(usage), L' ');
        appendTable(text,
            {{usage + std::wstring(tr(kUsageFind)), std::wstring(tr(kUsageFindNote))}, {indent + std::wstring(tr(kUsageCommand)), {}}}, {});
        text += L'\n';
        text += tr(kCommands);
        text += L'\n';
        std::vector<std::pair<std::wstring, std::wstring>> rows;
        for (const Command& c : commandTable())
            rows.emplace_back(std::wstring(c.name), std::wstring(tr(c.summary)));
        appendTable(text, rows);
        text += L'\n';
        text += tr(kCommonOptions);
        text += L'\n';
        appendOptions(text, kOverviewOptions);
        text += L'\n';
        text += tr(kNotes);
        text += L'\n';
        return text;
    }
    text += L"ws ";
    text += command->name;
    text += language() == Lang::Zh ? L"：" : L": ";
    text += tr(command->summary);
    text += L"\n\n";
    text += tr(kUsage);
    text += tr(command->usage);
    text += L'\n';
    if (!tr(command->details).empty()) {
        text += L'\n';
        text += tr(command->details);
        text += L'\n';
    }
    if (!command->options.empty()) {
        text += L'\n';
        text += tr(kOptions);
        text += L'\n';
        appendOptions(text, command->options);
    }
    if (command->talks) {
        text += L'\n';
        text += tr(kEveryCommandTitle);
        text += L'\n';
        appendOptions(text, kEveryCommand);
    }
    if (!tr(command->examples).empty()) {
        text += L'\n';
        text += tr(kExamples);
        text += L'\n';
        appendIndented(text, tr(command->examples));
    }
    return text;
}

Result<Exit> runHelp(Context& context)
{
    const Command* command = nullptr;
    if (!context.args.words.empty()) {
        command = commandNamed(context.args.words.front());
        if (!command) {
            constexpr Text kNoCommand {L"没有 {} 这个命令；ws help 列出全部", L"There is no command {}; ws help lists them"};
            return Failure {Exit::Usage, fmt(kNoCommand, context.args.words.front())};
        }
    }
    context.out.write(helpText(command));
    return Exit::Found;
}

} // namespace ws::cli
