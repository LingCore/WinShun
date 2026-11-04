#include "ContentPane.h"

#include "ContentIndexer.h"
#include "FileText.h"
#include "IndexService.h"
#include "Query.h"
#include "platform/ComWorker.h"
#include "platform/ContentReportPrint.h"
#include "platform/PreviewHost.h"
#include "platform/Shell.h"

#include <QCache>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSettings>
#include <algorithm>
#include <numeric>

using namespace Qt::StringLiterals;

namespace ws {
namespace {
QString versionOf(const QString& path)
{
    const win32::UniqueHandle handle(::CreateFileW(win32::longPath(path.toStdWString()).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_NO_RECALL, nullptr));
    BY_HANDLE_FILE_INFORMATION info {};
    if (!handle.valid() || !::GetFileInformationByHandle(handle.get(), &info))
        return {};
    return u"%1:%2:%3:%4:%5:%6:%7"_s.arg(info.dwVolumeSerialNumber).arg(info.nFileIndexHigh)
        .arg(info.nFileIndexLow).arg(info.nFileSizeHigh).arg(info.nFileSizeLow)
        .arg(info.ftLastWriteTime.dwHighDateTime).arg(info.ftLastWriteTime.dwLowDateTime);
}
QString readError(filetext::Status status)
{
    switch (status) {
    case filetext::Status::Ok: return {};
    case filetext::Status::Missing: return ContentPane::tr("文件已移动或删除");
    case filetext::Status::Folder: return ContentPane::tr("请选择一个文件");
    case filetext::Status::NotText: return ContentPane::tr("此文件没有可读取的文本");
    case filetext::Status::TooLarge: return ContentPane::tr("文件超过当前读取上限");
    case filetext::Status::CloudOnly: return ContentPane::tr("文件仅保存在云端，请先下载到本机");
    default: return ContentPane::tr("无法读取文件，可能被占用、没有权限或格式不受支持");
    }
}
}

int ContentLines::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}
QHash<int, QByteArray> ContentLines::roleNames() const
{
    return {{Value, "value"}, {Html, "html"}, {Location, "location"}, {Heading, "heading"},
        {Hit, "hit"}, {Current, "current"}, {SourceLine, "sourceLine"}, {Number, "number"}};
}
QString ContentLines::location(const contentdetails::Document& doc, int line)
{
    if (line < 0 || line >= std::ssize(doc.lines))
        return {};
    const auto& at = doc.lines[line];
    switch (at.kind) {
    case doctext::PlaceKind::Page: return ContentPane::tr("第 %1 页").arg(at.number);
    case doctext::PlaceKind::Slide: return ContentPane::tr("第 %1 张幻灯片").arg(at.number);
    case doctext::PlaceKind::Row:
        return ContentPane::tr("%1 · 第 %2 行").arg(at.sheet < doc.sheets.size() ? doc.sheets[at.sheet] : QString()).arg(at.number);
    default: return ContentPane::tr("第 %1 行").arg(at.number);
    }
}
QVariant ContentLines::data(const QModelIndex& index, int role) const
{
    if (!m_matches || !m_matches->document || !index.isValid() || index.row() >= rowCount() || index.row() < 0)
        return {};
    const int line = m_rows[index.row()];
    const auto& doc = *m_matches->document;
    if (role == SourceLine) return line;
    if (role == Number) return QString::number(doc.lines[line].kind == doctext::PlaceKind::Row
            || doc.lines[line].kind == doctext::PlaceKind::None ? doc.lines[line].number : line + 1);
    if (role == Location) return location(doc, line);
    if (role == Current) return line == m_current;
    if (role == Heading) {
        const int before = index.row() > 0 ? m_rows[index.row() - 1] : -1;
        return before < 0 || doc.lines[before].section != doc.lines[line].section || before + 1 != line
            ? location(doc, line) : QString();
    }
    const auto first = std::ranges::lower_bound(m_matches->items, line, {}, &contentdetails::Match::line);
    const auto last = std::ranges::upper_bound(m_matches->items, line, {}, &contentdetails::Match::line);
    const auto visible = [&](const contentdetails::Match& match) {
        return std::ranges::binary_search(m_visibleMatches, static_cast<int>(&match - m_matches->items.data()));
    };
    if (role == Hit)
        return std::any_of(first, last, visible);
    const auto& source = doc.lines[line];
    const QStringView value = QStringView(doc.text).mid(source.offset, source.length);
    if (role == Value) return value.toString();
    if (role != Html) return {};
    // Avoid creating a giant Qt text layout for generated single-line data.
    const qsizetype start = value.size() > 16384 && first != last ? std::max<qsizetype>(0, first->start - 256) : 0;
    const qsizetype end = std::min(value.size(), start + 16384);
    QString html = start > 0 ? u"…"_s : QString();
    qsizetype at = start;
    std::vector<std::pair<qsizetype, qsizetype>> spans;
    for (auto it = first; it != last; ++it) {
        if (visible(*it)) spans.emplace_back(it->start, it->start + it->length);
    }
    if (m_refinements) {
        auto it = std::ranges::lower_bound(*m_refinements, line, {}, &contentdetails::Match::line);
        for (; it != m_refinements->end() && it->line == line; ++it) spans.emplace_back(it->start, it->start + it->length);
    }
    std::ranges::sort(spans);
    for (std::size_t i = 0; i < spans.size(); ++i) {
        auto [from, to] = spans[i];
        while (i + 1 < spans.size() && spans[i + 1].first <= to) to = std::max(to, spans[++i].second);
        from = std::max(from, start); to = std::min(to, end);
        if (from >= to || from < at) continue;
        html += value.mid(at, from - at).toString().toHtmlEscaped();
        html += u"<font color=\"%1\">%2%3%4</font>"_s.arg(m_color.name(), m_bold ? u"<b>"_s : QString(),
            value.mid(from, to - from).toString().toHtmlEscaped(), m_bold ? u"</b>"_s : QString());
        at = to;
    }
    html += value.mid(at, end - at).toString().toHtmlEscaped();
    if (end < value.size()) html += u"…"_s;
    html.replace(u'\t', u"&nbsp; │ &nbsp;"_s);
    html.replace(u"  "_s, u" &nbsp;"_s);
    return html;
}
void ContentLines::assign(std::shared_ptr<const contentdetails::Matches> matches, std::vector<int> rows,
    std::vector<int> visibleMatches, QColor color, bool bold, std::shared_ptr<const std::vector<contentdetails::Match>> refinements)
{
    beginResetModel();
    m_matches = std::move(matches);
    m_rows = std::move(rows);
    m_visibleMatches = std::move(visibleMatches);
    m_refinements = std::move(refinements);
    m_color = color;
    m_bold = bold;
    endResetModel();
}
void ContentLines::setCurrent(int sourceLine)
{
    const int before = indexOf(m_current);
    m_current = sourceLine;
    const int after = indexOf(m_current);
    if (before >= 0) emit dataChanged(index(before), index(before), {Current});
    if (after >= 0) emit dataChanged(index(after), index(after), {Current});
}
int ContentLines::indexOf(int sourceLine) const
{
    const auto at = std::ranges::lower_bound(m_rows, sourceLine);
    return at != m_rows.end() && *at == sourceLine ? static_cast<int>(at - m_rows.begin()) : -1;
}
QString ContentLines::plainText() const
{
    if (!m_matches || !m_matches->document) return {};
    QString out;
    for (const int line : m_rows)
        out += location(*m_matches->document, line) + u'\t' + contentdetails::lineText(*m_matches->document, line) + u'\n';
    return out;
}

struct ContentPane::Worker {
    struct Cached { std::shared_ptr<const contentdetails::Document> document; bool fromIndex = false; };
    struct Read { std::shared_ptr<const contentdetails::Document> document; QString error; bool fromIndex = false; };
    std::atomic<quint64> generation {0};
    QCache<QString, Cached> cache {32768}; // KiB, includes decoded text and line metadata
    QString lastKey;
    Cached last;
    std::unique_ptr<DocExtractor> extractor;
    Read read(const QString& path, filetext::Sources sources, qint64 limit, quint64 ticket)
    {
        const auto cancel = [&] { return generation.load() != ticket; };
        if (cancel()) return {};
        const QString version = versionOf(path);
        if (version.isEmpty()) return {{}, ContentPane::tr("无法读取文件属性"), false};
        const QString key = path.toCaseFolded() + u'\n' + version;
        if (lastKey == key && last.document) return {last.document, {}, last.fromIndex};
        if (const auto* cached = cache.object(key))
            return {cached->document, {}, cached->fromIndex};
        if (!extractor) extractor = std::make_unique<DocExtractor>(1, DocExtractor::Priority::Normal);
        sources.extractor = extractor.get();
        sources.verifyFileVersion = true;
        sources.preserveLocations = true;
        const filetext::Result read = filetext::read(path.toStdWString(), sources, limit, cancel);
        if (cancel()) return {};
        if (read.status != filetext::Status::Ok) return {{}, readError(read.status), false};
        if (version != versionOf(path)) return {{}, ContentPane::tr("文件在读取时发生变化，请刷新重试"), false};
        doctext::DocText plain;
        if (!read.structured) {
            plain.text = read.text.toUtf8().toStdString();
            plain.truncated = read.cut;
        }
        const auto doc = contentdetails::document(read.structured ? *read.structured : plain, cancel);
        if (!doc || cancel()) return {};
        const auto cost = (doc->text.size() * 2 + static_cast<qsizetype>(doc->lines.size() * sizeof(contentdetails::Line))) / 1024 + 1;
        cache.insert(key, new Cached {doc, read.fromIndex}, static_cast<int>(std::min<qsizetype>(cost, INT_MAX)));
        lastKey = key;
        last = {doc, read.fromIndex};
        return {doc, {}, read.fromIndex};
    }
};

ContentPane::ContentPane(IndexService* index, QObject* parent)
    : QObject(parent), m_index(index), m_detailState(std::make_shared<Worker>()), m_reportState(std::make_shared<Worker>()),
      m_preview(std::make_unique<PreviewHost>())
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_debounce, &QTimer::timeout, this, &ContentPane::load);
    connect(m_preview.get(), &PreviewHost::failed, this, [this](quint64 ticket) {
        if (ticket != m_previewTicket) return;
        m_preview->hide();
        m_previewMessage = tr("此文件的预览处理程序不可用，可查看文本或打开文件");
        emit previewChanged();
    });
}
ContentPane::~ContentPane()
{
    ++m_detailState->generation;
    ++m_reportState->generation;
    if (m_audit) m_audit->cancel();
    closePreview();
}
void ContentPane::loadPreferences(const QString& file)
{
    m_stateFile = file;
    if (!file.isEmpty()) {
        QSettings state(file, QSettings::IniFormat);
        m_opened = state.value(u"ContentPane/Open"_s, true).toBool();
        m_width = std::clamp(state.value(u"ContentPane/Width"_s, 420).toInt(), 320, 1200);
        m_tab = std::clamp(state.value(u"ContentPane/Tab"_s, 1).toInt(), 0, 4);
    }
    emit preferencesChanged();
}
void ContentPane::savePreferences()
{
    if (m_stateFile.isEmpty()) return;
    QSettings state(m_stateFile, QSettings::IniFormat);
    state.setValue(u"ContentPane/Open"_s, m_opened);
    state.setValue(u"ContentPane/Width"_s, m_width);
    state.setValue(u"ContentPane/Tab"_s, m_tab);
}
void ContentPane::setOpened(bool value)
{
    if (m_opened == value) return;
    m_opened = value;
    savePreferences();
    schedule();
    emit preferencesChanged();
}
void ContentPane::setPreferredWidth(int value)
{
    value = std::clamp(value, 320, 1200);
    if (m_width == value) return;
    m_width = value;
    savePreferences();
    emit preferencesChanged();
}
void ContentPane::setTab(int value)
{
    value = std::clamp(value, 0, 4);
    if (m_tab == value) return;
    m_tab = value;
    closePreview();
    savePreferences();
    emit preferencesChanged();
    schedule();
    startPreview();
}
void ContentPane::setActive(bool value)
{
    if (m_active == value) return;
    m_active = value;
    if (!value) closePreview();
    schedule();
    emit activeChanged();
    startPreview();
}
void ContentPane::update(SearchEngine::Request request, const QString& words, const QString& key,
    SearchResults files, SearchResults selected, const QString& current,
    int scanned, int total, bool complete, bool searching, const ContentLeftOut& leftOut)
{
    const QStringList terms = contentTerms(words);
    const bool queryChanged = key != m_key || terms != m_terms;
    const bool fileChanged = current != m_path;
    m_request = std::move(request);
    m_files = std::move(files);
    m_selected = std::move(selected);
    if (queryChanged || fileChanged) {
        m_key = key;
        m_path = current;
        m_terms = terms;
        if (queryChanged) {
            m_countGroups.clear();
            m_enabled.assign(m_terms.size(), true);
            m_filter = {};
            cancelReport();
            m_report.reset();
            m_reportPreview.clear();
            m_reportStatus.clear();
            emit reportChanged();
        }
        m_sheet = -1;
        m_matches.reset();
        m_rows = {};
        m_current = -1;
        m_message.clear();
        showRows();
        closePreview();
        schedule();
        startPreview();
    }
    m_snapshotComplete = complete && !searching;
    m_summary = tr("当前命中文件：%1\n已处理候选：%2 / %3（含索引排除）\n状态：%4\n内容索引：%5")
        .arg(m_files.size()).arg(scanned).arg(total)
        .arg(searching ? tr("正在搜索") : complete ? tr("当前搜索已结束") : tr("当前结果尚不完整"))
        .arg(!m_indexEnabled ? tr("未开启") : m_index && m_index->readingContent() ? tr("正在更新") : tr("以文件版本为准"));
    if (leftOut.large.size()) m_summary += u'\n' + tr("超过大小上限：%1").arg(leftOut.large.size());
    if (leftOut.deferred) m_summary += u'\n' + tr("尚未搜索内容：%1").arg(leftOut.deferred);
    if (leftOut.remote) m_summary += u'\n' + tr("尚未索引的远程文件：%1").arg(leftOut.remote);
    if (leftOut.unreadable) m_summary += u'\n' + tr("读取失败：%1").arg(leftOut.unreadable);
    if (leftOut.truncated) m_summary += u'\n' + tr("仅有部分提取文本：%1").arg(leftOut.truncated);
    if (leftOut.indexExcluded) m_summary += u'\n' + tr("由内容索引排除：%1").arg(leftOut.indexExcluded);
    if (leftOut.elapsedMs) m_summary += u'\n' + tr("搜索耗时：%1 ms").arg(leftOut.elapsedMs);
    m_summary += u'\n' + tr("全部匹配次数按需统计；结果受上限或读取限制时会标明。");
    QMap<QString, int> types;
    QMap<QString, int> folders;
    for (const auto& file : m_files) {
        const QFileInfo info(file.path);
        ++types[info.suffix().toLower()];
        ++folders[info.path()];
    }
    m_groups.clear();
    for (auto it = types.cbegin(); it != types.cend() && m_groups.size() < 20; ++it)
        m_groups.append(QVariantMap {{u"name"_s, it.key().isEmpty() ? tr("无扩展名") : u'.' + it.key()},
            {u"count"_s, it.value()}, {u"kind"_s, tr("类型 · 命中文件")}});
    int shownFolders = 0;
    for (auto it = folders.cbegin(); it != folders.cend() && shownFolders++ < 20; ++it)
        m_groups.append(QVariantMap {{u"name"_s, QDir::toNativeSeparators(it.key())},
            {u"count"_s, it.value()}, {u"kind"_s, tr("文件夹 · 命中文件")}});
    if (types.size() > 20 || folders.size() > 20)
        m_summary += u'\n' + tr("分布仅展示部分条目，完整数据见报告。");
    emit summaryChanged();
}
void ContentPane::schedule()
{
    ++m_detailState->generation;
    m_debounce.stop();
    m_busy = m_active && m_opened && (m_tab == 1 || m_tab == 2) && !m_path.isEmpty() && !m_terms.isEmpty();
    if (m_busy) m_debounce.start();
    emit changed();
}
void ContentPane::load()
{
    const auto state = m_detailState;
    const quint64 ticket = state->generation.load();
    const filetext::Sources sources {m_index ? m_index->index() : nullptr, m_index ? m_index->contentIndex() : nullptr};
    const QString path = m_path;
    const auto terms = m_terms;
    const auto enabled = m_enabled;
    const auto filter = m_filter;
    const auto context = m_context;
    const auto sheet = m_sheet;
    const auto limit = m_request.contentSizeLimits.of(path.toStdWString());
    if (!m_detailWorker) m_detailWorker = std::make_unique<ComWorker>();
    m_detailWorker->post([this, state, ticket, sources, path, terms, enabled, filter, context, sheet, limit] {
        const auto cancel = [&] { return state->generation.load() != ticket; };
        if (cancel()) return;
        const auto read = state->read(path, sources, limit, ticket);
        auto matches = std::make_shared<contentdetails::Matches>(contentdetails::find(read.document, terms, cancel));
        auto rows = contentdetails::filter(*matches, enabled, filter, context, sheet, cancel);
        if (cancel()) return;
        QMetaObject::invokeMethod(this, [this, state, ticket, matches, rows = std::move(rows), error = read.error,
                                          fromIndex = read.fromIndex] () mutable {
            if (state->generation.load() != ticket) return;
            m_matches = matches;
            m_rows = std::move(rows);
            m_busy = false;
            m_message = !error.isEmpty() ? error : !m_rows.error.isEmpty() ? tr("筛选表达式无效：%1").arg(m_rows.error)
                : matches->truncated || m_rows.truncated ? tr("仅显示部分内容或匹配，计数不是全文总数")
                : fromIndex ? tr("来自当前版本的内容索引") : QString();
            m_current = m_rows.matches.empty() ? -1 : std::clamp(m_current, 0, static_cast<int>(m_rows.matches.size()) - 1);
            showRows();
            emit changed();
        }, Qt::QueuedConnection);
    });
}
void ContentPane::showRows()
{
    const auto refinements = m_filter.enabled ? m_rows.refinements : nullptr;
    m_hits.assign(m_matches, m_rows.lines, m_rows.matches, m_color, m_weight >= 600, refinements);
    std::vector<int> text;
    if (m_matches && m_matches->document) {
        const auto& doc = *m_matches->document;
        for (int i = 0; i < std::ssize(doc.lines); ++i)
            if (m_sheet < 0 || doc.lines[i].sheet == m_sheet) text.push_back(i);
    }
    m_text.assign(m_matches, std::move(text), m_rows.matches, m_color, m_weight >= 600, refinements);
    if (m_current >= 0 && m_current < std::ssize(m_rows.matches)) {
        const int line = m_matches->items[m_rows.matches[m_current]].line;
        m_hits.setCurrent(line);
        m_text.setCurrent(line);
        emit positionRequested(m_hits.indexOf(line), m_text.indexOf(line));
    }
}
QString ContentPane::matchStatus() const
{
    if (m_busy) return tr("正在读取与匹配…");
    if (!m_matches || !m_matches->document) return tr("选择文件查看匹配和文本");
    return tr("显示 %1 / %2 处匹配 · 当前 %3").arg(m_rows.matches.size()).arg(m_matches->items.size())
        .arg(m_current < 0 ? 0 : m_current + 1);
}
QVariantList ContentPane::terms() const
{
    QVariantList out;
    for (int i = 0; i < m_terms.size(); ++i)
        out.append(QVariantMap {{u"text"_s, m_terms[i]}, {u"count"_s, m_matches && i < std::ssize(m_matches->counts) ? m_matches->counts[i] : 0},
            {u"enabled"_s, i < std::ssize(m_enabled) && m_enabled[i]}});
    if (!m_filter.text.isEmpty()) out.append(QVariantMap {{u"text"_s, m_filter.text}, {u"count"_s, m_rows.filterCount},
        {u"enabled"_s, m_filter.enabled}});
    return out;
}
QVariantList ContentPane::sheets() const
{
    QVariantList out;
    if (!m_matches || !m_matches->document) return out;
    const auto& doc = *m_matches->document;
    for (int i = 0; i < doc.sheets.size(); ++i) {
        const auto count = i < std::ssize(m_matches->sheetCounts) ? m_matches->sheetCounts[i] : 0;
        out.append(QVariantMap {{u"text"_s, doc.sheets[i]}, {u"count"_s, static_cast<int>(count)}});
    }
    return out;
}
void ContentPane::setSheet(int value) { if (value != m_sheet) { m_sheet = value; schedule(); } }
void ContentPane::setContext(int value) { value = std::clamp(value, 0, 3); if (value != m_context) { m_context = value; schedule(); } }
void ContentPane::setHighlightColor(const QColor& value)
{
    if (m_color == value) return;
    m_color = value;
    showRows();
    emit changed();
}
void ContentPane::setHighlightWeight(int value)
{
    if (m_weight == value) return;
    m_weight = value;
    showRows();
    emit changed();
}
void ContentPane::setFilter(const QString& text, bool matchCase, bool wholeWord, bool regex)
{
    if (m_filter.text == text && m_filter.matchCase == matchCase && m_filter.wholeWord == wholeWord && m_filter.regex == regex) return;
    m_filter = {text, matchCase, wholeWord, regex};
    schedule();
}
void ContentPane::toggleTerm(int index)
{
    if (index == m_terms.size() && !m_filter.text.isEmpty()) { m_filter.enabled = !m_filter.enabled; schedule(); return; }
    if (index < 0 || index >= std::ssize(m_enabled)) return;
    m_enabled[index] = !m_enabled[index];
    schedule();
}
void ContentPane::nextMatch(int delta)
{
    if (m_busy || m_rows.matches.empty()) return;
    m_current = (m_current + delta + static_cast<int>(m_rows.matches.size())) % static_cast<int>(m_rows.matches.size());
    const int line = m_matches->items[m_rows.matches[m_current]].line;
    m_hits.setCurrent(line);
    m_text.setCurrent(line);
    emit positionRequested(m_hits.indexOf(line), m_text.indexOf(line));
    emit changed();
}
void ContentPane::activateLine(int row, bool textView)
{
    const int line = (textView ? m_text : m_hits).sourceLineAt(row);
    if (line < 0 || !m_matches || !m_matches->document) return;
    const auto& at = m_matches->document->lines[line];
    emit openRequested(m_path, at.kind == doctext::PlaceKind::None ? static_cast<int>(at.number) : 0);
}
void ContentPane::selectLine(int row, bool textView)
{
    const int line = (textView ? m_text : m_hits).sourceLineAt(row);
    if (line < 0 || !m_matches) return;
    for (int i = 0; i < std::ssize(m_rows.matches); ++i) {
        if (m_matches->items[m_rows.matches[i]].line == line) {
            m_current = i;
            m_hits.setCurrent(line);
            m_text.setCurrent(line);
            emit changed();
            return;
        }
    }
}
void ContentPane::openCurrent()
{
    if (m_busy || m_path.isEmpty()) return;
    if (m_current >= 0 && m_matches && m_current < std::ssize(m_rows.matches)) {
        const auto& at = m_matches->document->lines[m_matches->items[m_rows.matches[m_current]].line];
        emit openRequested(m_path, at.kind == doctext::PlaceKind::None ? static_cast<int>(at.number) : 0);
    } else emit openRequested(m_path, 0);
}
void ContentPane::refresh()
{
    const auto state = m_detailState;
    if (m_detailWorker) m_detailWorker->post([state] { state->cache.clear(); state->lastKey.clear(); state->last = {}; });
    closePreview();
    schedule();
    startPreview();
}
void ContentPane::copyText(bool matchesOnly)
{
    if (m_busy || !m_matches || !m_matches->document) return;
    const auto matches = m_matches;
    const auto lines = m_rows.lines;
    const int sheet = m_sheet;
    const auto state = m_detailState;
    const quint64 ticket = state->generation.load();
    if (!m_detailWorker) m_detailWorker = std::make_unique<ComWorker>();
    m_detailWorker->post([this, state, ticket, matches, lines, sheet, matchesOnly] {
        const auto& doc = *matches->document;
        QString text;
        if (matchesOnly) {
            for (const int line : lines) {
                if (state->generation.load() != ticket) return;
                text += ContentLines::location(doc, line) + u'\t' + contentdetails::lineText(doc, line) + u'\n';
            }
        } else {
            for (int i = 0; i < std::ssize(doc.lines); ++i) {
                if (state->generation.load() != ticket) return;
                if (sheet < 0 || doc.lines[i].sheet == sheet) text += contentdetails::lineText(doc, i) + u'\n';
            }
        }
        QMetaObject::invokeMethod(this, [state, ticket, text = std::move(text)] {
            if (state->generation.load() == ticket) QGuiApplication::clipboard()->setText(text);
        }, Qt::QueuedConnection);
    });
}

QVariantMap ContentPane::reportOptions() const
{
    return m_reportOptions.isEmpty() ? QVariantMap {{u"scope"_s, 0}, {u"files"_s, true}, {u"matches"_s, true}, {u"context"_s, true}} : m_reportOptions;
}
void ContentPane::setReportOptions(int scope, bool includeFiles, bool includeMatches, bool context)
{
    if (m_reporting) return;
    const QVariantMap options {{u"scope"_s, std::clamp(scope, 0, 2)}, {u"files"_s, includeFiles},
        {u"matches"_s, includeMatches}, {u"context"_s, context}};
    if (options == reportOptions()) return;
    m_reportOptions = options;
    m_report.reset();
    m_reportPreview.clear();
    m_reportStatus.clear();
    emit reportChanged();
}
void ContentPane::prepareReport(bool selectedOnly, bool allFiles, bool includeFiles, bool includeMatches, bool context)
{
    if (m_reporting || (!includeFiles && !includeMatches)) return;
    if (m_terms.isEmpty()) { m_reportStatus = tr("请选择一个文件"); emit reportChanged(); return; }
    setReportOptions(selectedOnly ? 1 : allFiles ? 2 : 0, includeFiles, includeMatches, context);
    m_report.reset();
    m_reportPreview.clear();
    if (allFiles && !selectedOnly && m_index) {
        if (!m_audit) m_audit = std::make_unique<SearchEngine>(m_index);
        disconnect(m_audit.get(), nullptr, this, nullptr);
        m_auditFiles.clear();
        m_auditLeftOut = {};
        m_auditTerms = m_terms;
        connect(m_audit.get(), &SearchEngine::contentResults, this, [this](quint64 id, const SearchResults& rows) {
            if (id == m_auditId) {
                for (const auto& row : rows) if (m_auditFiles.size() < 20000) m_auditFiles.append(row);
            }
        });
        connect(m_audit.get(), &SearchEngine::contentScope, this, [this](quint64 id, const ContentScope& scope) {
            if (id == m_auditId) m_auditTerms = contentTerms(scope.words);
        });
        connect(m_audit.get(), &SearchEngine::contentLeftOut, this, [this](quint64 id, const ContentLeftOut& leftOut) {
            if (id == m_auditId) m_auditLeftOut = leftOut;
        });
        connect(m_audit.get(), &SearchEngine::contentProgress, this,
            [this, includeFiles, includeMatches, context](quint64 id, int scanned, int total, bool finished) {
                if (id != m_auditId || !m_reporting) return;
                m_reportStatus = tr("完整搜索：已处理 %1 / %2 个候选").arg(scanned).arg(total);
                emit reportChanged();
                if (finished) {
                    m_auditId = 0;
                    buildReport(std::move(m_auditFiles), true, scanned == total, includeFiles, includeMatches, context);
                }
            });
        auto request = m_request;
        request.maxContentResults = 20000; // bounded, explicitly labelled when reached
        request.contentThreads = 2;
        m_reporting = true;
        m_reportStatus = tr("正在完整搜索，可随时取消…");
        m_auditId = m_audit->submit(std::move(request));
        emit reportChanged();
    } else {
        SearchResults files = selectedOnly ? m_selected : m_files;
        if (selectedOnly && files.isEmpty()) {
            for (const auto& file : m_files) if (file.path == m_path) files.append(file);
        }
        if (selectedOnly && files.isEmpty()) { m_reportStatus = tr("请选择一个文件"); emit reportChanged(); return; }
        buildReport(std::move(files), false, selectedOnly || m_snapshotComplete, includeFiles, includeMatches, context);
    }
}
void ContentPane::cancelReport()
{
    ++m_reportState->generation;
    m_auditId = 0;
    if (m_audit) m_audit->cancel();
    if (m_reporting) {
        m_reporting = false;
        m_reportStatus = tr("已取消，未生成报告");
        emit reportChanged();
    }
}
void ContentPane::buildReport(SearchResults files, bool full, bool complete, bool includeFiles, bool includeMatches, bool context)
{
    m_reporting = true;
    m_reportStatus = tr("正在统计全部匹配：0 / %1 个文件").arg(files.size());
    const auto state = m_reportState;
    const quint64 ticket = ++state->generation;
    const filetext::Sources sources {m_index ? m_index->index() : nullptr, m_index ? m_index->contentIndex() : nullptr};
    const auto limits = m_request.contentSizeLimits;
    const auto terms = full ? m_auditTerms : m_terms;
    const auto leftOut = full ? m_auditLeftOut : ContentLeftOut{};
    const QString query = m_request.text;
    const int radius = m_context;
    emit reportChanged();
    if (!m_reportWorker) m_reportWorker = std::make_unique<ComWorker>();
    m_reportWorker->post([this, state, ticket, sources, files = std::move(files), limits, terms, query, radius,
                            full, complete, includeFiles, includeMatches, context, leftOut] {
        const auto cancel = [&] { return state->generation.load() != ticket; };
        auto report = std::make_shared<contentreport::Report>();
        report->title = tr("Win顺 · 内容搜索报告");
        report->notes = {tr("搜索：%1").arg(query),
            full ? tr("范围：完整搜索（最多 20,000 个命中文件）") : tr("范围：生成时的结果／所选文件快照"),
            tr("生成时间：%1").arg(QDateTime::currentDateTime().toString(Qt::ISODate))};
        if (full) report->notes.append(tr("搜索跳过：大小上限 %1；无法读取 %2；远程未索引 %3；延后读取 %4")
            .arg(leftOut.large.size()).arg(leftOut.unreadable).arg(leftOut.remote).arg(leftOut.deferred));
        contentreport::Table fileTable {tr("文件列表"), {tr("文件"), tr("路径"), tr("匹配次数"), tr("状态")}, {}};
        contentreport::Table hitTable {tr("逐处匹配"), {tr("文件"), tr("路径"), tr("位置"), tr("关键词"), tr("文本"), tr("上下文")}, {}};
        int failed = 0, partial = 0, done = 0;
        qint64 count = 0, payload = 0;
        const bool incomplete = !complete || leftOut.unreadable || !leftOut.large.isEmpty() || leftOut.remote || leftOut.deferred;
        bool limited = false;
        QMap<QString, qint64> typeMatches, folderMatches;
        for (const auto& file : files) {
            if (cancel()) return;
            const auto read = state->read(file.path, sources, limits.of(file.path.toStdWString()), ticket);
            const auto matches = contentdetails::find(read.document, terms, cancel);
            if (cancel()) return;
            if (!read.error.isEmpty()) ++failed;
            if (matches.truncated) ++partial;
            const qint64 n = static_cast<qint64>(matches.items.size());
            count += n;
            typeMatches[QFileInfo(file.path).suffix().toLower()] += n;
            folderMatches[QFileInfo(file.path).path()] += n;
            if (includeFiles)
                fileTable.rows.push_back({file.name, file.path, QString::number(n), !read.error.isEmpty() ? read.error
                    : matches.truncated ? tr("部分内容") : tr("已读取")});
            if (includeMatches && matches.document) {
                const auto& doc = *matches.document;
                for (const auto& match : matches.items) {
                    if (cancel()) return;
                    QString around;
                    if (context) {
                        for (int line = std::max(0, match.line - radius);
                            line <= std::min(static_cast<int>(doc.lines.size()) - 1, match.line + radius); ++line) {
                            const auto& at = doc.lines[line];
                            const auto& origin = doc.lines[match.line];
                            if (at.section == origin.section && (origin.kind != doctext::PlaceKind::Row
                                    || std::abs(static_cast<qint64>(at.number) - origin.number) <= radius))
                                around += ContentLines::location(doc, line) + u'\t' + contentdetails::lineText(doc, line) + u'\n';
                        }
                    }
                    QString value = contentdetails::lineText(doc, match.line);
                    if (value.size() > 32767 || around.size() > 32767) limited = true;
                    value.truncate(32767);
                    around.truncate(32767);
                    payload += (value.size() + around.size() + file.path.size()) * 2;
                    if (hitTable.rows.size() >= 100000 || payload > (32ll << 20)) { limited = true; break; }
                    hitTable.rows.push_back({file.name, file.path, ContentLines::location(doc, match.line),
                        terms[match.term], value, around});
                }
            }
            ++done;
            if (done % 8 == 0) QMetaObject::invokeMethod(this, [this, state, ticket, done, total = files.size()] {
                if (state->generation.load() != ticket) return;
                m_reportStatus = tr("正在统计全部匹配：%1 / %2 个文件").arg(done).arg(total);
                emit reportChanged();
            }, Qt::QueuedConnection);
            if (includeMatches && (hitTable.rows.size() >= 100000 || payload > (32ll << 20))) break;
        }
        if (cancel()) return;
        report->notes.append(tr("已读取／尝试：%1 个文件；匹配：%2 处；读取失败：%3；部分内容：%4")
            .arg(done).arg(count).arg(failed).arg(partial));
        report->notes.append(incomplete || limited || failed || partial
                ? tr("此报告包含未完成或未读取的内容，数字不是完整全文总数。")
                : tr("本报告范围内的内容已读取完成。"));
        if (limited) report->notes.append(tr("安全上限：100,000 条匹配、32 MiB 正文，每个字段 32,767 字符；超出部分未写入。"));
        if (includeFiles && full) {
            for (const auto& path : leftOut.unreadableSamples) fileTable.rows.push_back({QFileInfo(path).fileName(), path, {}, tr("搜索时无法读取")});
            for (const auto& path : leftOut.large) fileTable.rows.push_back({QFileInfo(path).fileName(), path, {}, tr("超过大小上限")});
        }
        if (includeFiles) report->tables.push_back(std::move(fileTable));
        if (includeMatches) report->tables.push_back(std::move(hitTable));
        contentreport::Table distribution {tr("匹配分布"), {tr("类型／文件夹"), tr("匹配次数")}, {}};
        for (auto it = typeMatches.cbegin(); it != typeMatches.cend(); ++it) distribution.rows.push_back({u'.' + it.key(), QString::number(it.value())});
        for (auto it = folderMatches.cbegin(); it != folderMatches.cend(); ++it) distribution.rows.push_back({it.key(), QString::number(it.value())});
        report->tables.push_back(std::move(distribution));
        const QString preview = report->title + u'\n' + report->notes.join(u'\n');
        QMetaObject::invokeMethod(this, [this, state, ticket, report, preview, full] {
            if (state->generation.load() != ticket) return;
            m_report = report;
            m_reportPreview = preview;
            m_reportStatus = tr("报告已就绪");
            m_reporting = false;
            m_countGroups.clear();
            if (!report->tables.empty()) {
                const auto& rows = report->tables.back().rows;
                for (int i = 0; i < std::ssize(rows) && i < 40; ++i)
                    m_countGroups.append(QVariantMap {{u"name"_s, rows[i][0]}, {u"count"_s, rows[i][1]},
                        {u"kind"_s, full ? tr("完整搜索 · 匹配次数") : tr("报告范围 · 匹配次数")}});
            }
            emit summaryChanged();
            emit reportChanged();
        }, Qt::QueuedConnection);
    });
}
void ContentPane::copyReport()
{
    if (!m_report) return;
    const auto report = m_report;
    const auto state = m_reportState;
    const quint64 ticket = state->generation.load();
    if (!m_reportWorker) m_reportWorker = std::make_unique<ComWorker>();
    m_reportWorker->post([this, report, state, ticket] {
        if (state->generation.load() != ticket) return;
        const QString text = contentreport::text(*report);
        QMetaObject::invokeMethod(this, [text, state, ticket] {
            if (state->generation.load() == ticket) QGuiApplication::clipboard()->setText(text);
        }, Qt::QueuedConnection);
    });
}
void ContentPane::saveReport(int format)
{
    if (!m_report || m_reporting || m_dialogOpen || format < 0 || format > 3) return;
    const QStringList extensions {u"txt"_s, u"csv"_s, u"html"_s, u"xlsx"_s};
    const QString extension = extensions[format];
    const auto report = m_report;
    closePreview();
    m_dialogOpen = true;
    const QString path = shell::pickSaveFile(m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr,
        tr("保存搜索报告"), u"WinShun-search."_s + extension, tr("搜索报告"), extension);
    m_dialogOpen = false;
    if (path.isEmpty()) return;
    m_reporting = true;
    m_reportStatus = tr("正在保存报告…");
    emit reportChanged();
    const auto state = m_reportState;
    const quint64 ticket = state->generation.load();
    if (!m_reportWorker) m_reportWorker = std::make_unique<ComWorker>();
    m_reportWorker->post([this, report, path, format, state, ticket] {
        const bool ok = contentreport::save(*report, static_cast<contentreport::Format>(format), path);
        QMetaObject::invokeMethod(this, [this, ok, path, state, ticket] {
            if (state->generation.load() != ticket) return;
            m_reporting = false;
            m_reportStatus = ok ? tr("已保存：%1").arg(path) : tr("保存失败，原文件已保留");
            emit reportChanged();
        }, Qt::QueuedConnection);
    });
}
void ContentPane::printReport()
{
    if (!m_report || m_reporting || m_dialogOpen) return;
    m_dialogOpen = true;
    const auto report = m_report;
    m_reporting = true;
    m_reportStatus = tr("请选择打印机…");
    emit reportChanged();
    const auto state = m_reportState;
    const quint64 ticket = ++state->generation;
    if (!m_reportWorker) m_reportWorker = std::make_unique<ComWorker>();
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    m_reportWorker->post([this, state, ticket, report, owner] {
        const auto result = reportprint::print(*report, [&] { return state->generation.load() != ticket; }, owner);
        QMetaObject::invokeMethod(this, [this, state, ticket, result] {
            m_dialogOpen = false;
            if (state->generation.load() != ticket) return;
            m_reporting = false;
            m_reportStatus = result == reportprint::Result::Printed ? tr("报告已发送到打印机")
                : result == reportprint::Result::Cancelled ? tr("已取消打印") : tr("打印失败，请检查打印机后重试");
            emit reportChanged();
        }, Qt::QueuedConnection);
    });
}

void ContentPane::setPreviewArea(QQuickItem* area, bool dark)
{
    if (m_previewArea != area || m_dark != dark) closePreview();
    m_previewArea = area;
    m_dark = dark;
    startPreview();
}
void ContentPane::startPreview()
{
    if (!m_active || !m_opened || m_tab != 3 || !m_previewArea || !m_previewArea->isVisible()
        || !m_previewArea->window() || !m_previewArea->window()->isVisible() || m_path.isEmpty()) return;
    if (m_previewPath == m_path) { movePreview(); return; }
    const auto handler = PreviewHost::handlerFor(m_path);
    m_previewMessage = handler ? tr("正在加载文件预览…") : tr("此文件没有可用的预览处理程序，可查看文本或打开文件");
    emit previewChanged();
    if (!handler) return;
    m_previewPath = m_path;
    const auto rect = m_previewArea->mapRectToScene(m_previewArea->boundingRect());
    const auto* window = m_previewArea->window();
    POINT origin {};
    ::ClientToScreen(reinterpret_cast<HWND>(window->winId()), &origin);
    const qreal dpr = window->devicePixelRatio();
    m_preview->show(++m_previewTicket, m_path, *handler, origin.x + qRound(rect.left() * dpr), origin.y + qRound(rect.top() * dpr),
        origin.x + qRound(rect.right() * dpr), origin.y + qRound(rect.bottom() * dpr),
        reinterpret_cast<HWND>(window->winId()), m_dark);
}
void ContentPane::movePreview()
{
    if (!m_previewArea || !m_previewArea->isVisible() || !m_previewArea->window()
        || !m_previewArea->window()->isVisible() || !m_active || !m_opened || m_tab != 3) { closePreview(); return; }
    if (m_previewPath.isEmpty()) { startPreview(); return; }
    const auto rect = m_previewArea->mapRectToScene(m_previewArea->boundingRect());
    const auto* window = m_previewArea->window();
    POINT origin {};
    ::ClientToScreen(reinterpret_cast<HWND>(window->winId()), &origin);
    const qreal dpr = window->devicePixelRatio();
    m_preview->move(origin.x + qRound(rect.left() * dpr), origin.y + qRound(rect.top() * dpr),
        origin.x + qRound(rect.right() * dpr), origin.y + qRound(rect.bottom() * dpr));
}
void ContentPane::closePreview()
{
    ++m_previewTicket;
    m_preview->hide();
    m_previewPath.clear();
}
std::optional<QRect> ContentPane::previewRect() const { return m_preview->visibleRect(); }

bool ContentPane::ownsForeground() const
{
    return m_active && m_opened && m_tab == 3 && m_preview->owns(::GetForegroundWindow());
}

} // namespace ws
