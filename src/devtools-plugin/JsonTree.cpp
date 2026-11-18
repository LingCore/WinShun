#include "JsonTree.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::devtools {

namespace {

constexpr int kShownCharacters = 300; // of a long string, in its row
constexpr int kPreviewMembers = 6;
constexpr int kPreviewCharacters = 90;
constexpr int kOpenAtFirst = 1500; // rows: levels are opened while they fit

bool isIdentifier(const QString& key)
{
    if (key.isEmpty() || !(key[0].isLetter() || key[0] == u'_' || key[0] == u'$'))
        return false;
    return std::all_of(key.begin(), key.end(), [](QChar c) { return c.isLetterOrNumber() || c == u'_' || c == u'$'; });
}

} // namespace

JsonTree::JsonTree(QObject* parent)
    : QAbstractListModel(parent)
{
}

void JsonTree::setDocument(std::shared_ptr<const Document> document)
{
    m_document = std::move(document);
    m_matches.clear();
    m_current = -1;
    if (!m_document || m_document->isEmpty()) {
        m_depth.clear();
        m_open.clear();
        m_match.clear();
        m_levels = 0;
        rebuild();
        emit documentChanged();
        emit matchesChanged();
        return;
    }
    const std::size_t count = m_document->nodes.size();
    m_open.assign(count, 0);
    m_match.assign(count, 0);
    computeDepths();
    // As many levels open as fit in a first look; all of a small document.
    std::vector<int> perDepth(std::size_t(m_levels) + 1, 0);
    for (std::size_t n = 0; n < count; ++n) {
        if (m_depth[n] >= 0)
            ++perDepth[std::size_t(m_depth[n])];
    }
    int level = 1;
    int shown = perDepth[0];
    while (level < m_levels && shown + perDepth[std::size_t(level)] <= kOpenAtFirst)
        shown += perDepth[std::size_t(level++)];
    for (std::size_t n = 0; n < count; ++n)
        m_open[n] = m_document->expandable(int(n)) && m_depth[n] + 1 < level ? 1 : 0;
    rebuild();
    emit documentChanged();
    if (!m_words.isEmpty())
        search(m_words);
    else
        emit matchesChanged();
}

void JsonTree::computeDepths()
{
    const auto& nodes = m_document->nodes;
    m_depth.assign(nodes.size(), 0);
    m_levels = 1;
    // The root's members start the list (depth 0) when it has any.
    m_depth[0] = m_document->expandable(0) ? -1 : 0;
    std::vector<int> stack {0};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        const int c = m_document->container(n);
        if (c != n)
            m_depth[std::size_t(c)] = m_depth[std::size_t(n)];
        for (int child = nodes[std::size_t(c)].first; child >= 0; child = nodes[std::size_t(child)].next) {
            m_depth[std::size_t(child)] = m_depth[std::size_t(n)] + 1;
            m_levels = std::max(m_levels, m_depth[std::size_t(child)] + 1);
            stack.push_back(child);
        }
    }
}

void JsonTree::appendVisible(int node, std::vector<int>& out) const
{
    for (int child = m_document->firstChild(node); child >= 0; child = m_document->nodes[std::size_t(child)].next) {
        out.push_back(child);
        if (m_open[std::size_t(child)] && m_document->expandable(child))
            appendVisible(child, out);
    }
}

void JsonTree::rebuild()
{
    beginResetModel();
    m_rows.clear();
    if (m_document && !m_document->isEmpty()) {
        if (m_depth[0] < 0)
            appendVisible(0, m_rows);
        else
            m_rows.push_back(0);
    }
    endResetModel();
}

int JsonTree::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> JsonTree::roleNames() const
{
    return {
        {DepthRole, "depth"},
        {KeyRole, "key"},
        {KeyIsIndexRole, "keyIsIndex"},
        {HasKeyRole, "hasKey"},
        {KindRole, "kind"},
        {ShapeRole, "shape"},
        {ValueRole, "value"},
        {PreviewRole, "preview"},
        {CountRole, "count"},
        {ExpandableRole, "expandable"},
        {ExpandedRole, "expanded"},
        {EmbeddedRole, "embedded"},
        {MatchRole, "match"},
        {CurrentRole, "current"},
    };
}

QVariant JsonTree::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_rows.size()))
        return {};
    const int n = m_rows[std::size_t(index.row())];
    const Node& node = m_document->nodes[std::size_t(n)];
    const int parent = node.parent;
    const bool inArray = parent >= 0 && m_document->nodes[std::size_t(parent)].kind == Kind::Array;
    switch (role) {
    case DepthRole: return m_depth[std::size_t(n)];
    case KeyRole: return parent < 0 ? QString() : inArray ? QString::number(node.index) : node.key;
    case KeyIsIndexRole: return inArray;
    case HasKeyRole: return parent >= 0;
    case KindRole: return int(node.kind);
    case ShapeRole: return int(m_document->shape(n));
    case ValueRole: return shown(n);
    case PreviewRole: return m_document->expandable(n) ? preview(n) : QString();
    case CountRole: return m_document->childCount(n);
    case ExpandableRole: return m_document->expandable(n);
    case ExpandedRole: return m_open[std::size_t(n)] != 0;
    case EmbeddedRole: return node.embedded;
    case MatchRole: return int(m_match[std::size_t(n)]);
    case CurrentRole: return m_current >= 0 && m_matches[std::size_t(m_current)] == n;
    default: return {};
    }
}

QString JsonTree::shown(int node) const
{
    const Node& n = m_document->nodes[std::size_t(node)];
    switch (n.kind) {
    case Kind::String: {
        QString q = quoted(QStringView(n.text).first(std::min<qsizetype>(n.text.size(), kShownCharacters)));
        if (n.text.size() > kShownCharacters)
            q.insert(q.size() - 1, u'…');
        return q;
    }
    case Kind::Number:
    case Kind::Bool:
    case Kind::Null:
        return n.text;
    case Kind::Object:
        return n.count == 0 ? u"{}"_s : QString(); // an empty one has no rows to open
    case Kind::Array:
        return n.count == 0 ? u"[]"_s : QString();
    }
    return {};
}

QString JsonTree::preview(int node) const
{
    const auto& nodes = m_document->nodes;
    const bool object = m_document->shape(node) == Kind::Object;
    QStringList parts;
    int length = 0;
    int taken = 0;
    for (int c = m_document->firstChild(node); c >= 0; c = nodes[std::size_t(c)].next) {
        if (taken == kPreviewMembers || length > kPreviewCharacters) {
            parts << u"…"_s;
            break;
        }
        QString part;
        if (object)
            part = nodes[std::size_t(c)].key + u": ";
        if (m_document->expandable(c))
            part += m_document->shape(c) == Kind::Object ? u"{…}" : u"[…]";
        else if (nodes[std::size_t(c)].kind == Kind::String)
            part += quoted(QStringView(nodes[std::size_t(c)].text).first(std::min<qsizetype>(nodes[std::size_t(c)].text.size(), 24)))
                + (nodes[std::size_t(c)].text.size() > 24 ? u"…"_s : QString());
        else
            part += m_document->shape(c) == Kind::Object ? u"{}"_s
                  : m_document->shape(c) == Kind::Array  ? u"[]"_s
                                                         : nodes[std::size_t(c)].text;
        length += int(part.size());
        parts << part;
        ++taken;
    }
    return (object ? u"{ "_s : u"[ "_s) + parts.join(u", ") + (object ? u" }"_s : u" ]"_s);
}

void JsonTree::open(int row)
{
    if (row < 0 || row >= int(m_rows.size()))
        return;
    const int node = m_rows[std::size_t(row)];
    if (m_open[std::size_t(node)] || !m_document->expandable(node))
        return;
    m_open[std::size_t(node)] = 1;
    std::vector<int> added;
    appendVisible(node, added);
    if (!added.empty()) {
        beginInsertRows({}, row + 1, row + int(added.size()));
        m_rows.insert(m_rows.begin() + row + 1, added.begin(), added.end());
        endInsertRows();
    }
    emit dataChanged(index(row), index(row), {ExpandedRole});
}

void JsonTree::close(int row)
{
    if (row < 0 || row >= int(m_rows.size()))
        return;
    const int node = m_rows[std::size_t(row)];
    if (!m_open[std::size_t(node)])
        return;
    m_open[std::size_t(node)] = 0;
    const int depth = m_depth[std::size_t(node)];
    int last = row;
    while (last + 1 < int(m_rows.size()) && m_depth[std::size_t(m_rows[std::size_t(last + 1)])] > depth)
        ++last;
    if (last > row) {
        beginRemoveRows({}, row + 1, last);
        m_rows.erase(m_rows.begin() + row + 1, m_rows.begin() + last + 1);
        endRemoveRows();
    }
    emit dataChanged(index(row), index(row), {ExpandedRole});
}

void JsonTree::toggle(int row)
{
    if (row < 0 || row >= int(m_rows.size()))
        return;
    if (m_open[std::size_t(m_rows[std::size_t(row)])])
        close(row);
    else
        open(row);
}

void JsonTree::setExpanded(int row, bool open)
{
    if (open)
        this->open(row);
    else
        close(row);
}

void JsonTree::expandAll()
{
    if (!m_document || m_document->isEmpty())
        return;
    for (std::size_t n = 0; n < m_open.size(); ++n)
        m_open[n] = m_document->expandable(int(n)) ? 1 : 0;
    rebuild();
}

void JsonTree::collapseAll()
{
    expandToLevel(1);
}

void JsonTree::expandToLevel(int level)
{
    if (!m_document || m_document->isEmpty())
        return;
    for (std::size_t n = 0; n < m_open.size(); ++n)
        m_open[n] = m_document->expandable(int(n)) && m_depth[n] + 1 < level ? 1 : 0;
    rebuild();
}

void JsonTree::setSubtreeExpanded(int row, bool open)
{
    if (row < 0 || row >= int(m_rows.size()))
        return;
    const int node = m_rows[std::size_t(row)];
    std::vector<int> stack {node};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        if (m_document->expandable(n))
            m_open[std::size_t(n)] = open ? 1 : 0;
        for (int c = m_document->firstChild(n); c >= 0; c = m_document->nodes[std::size_t(c)].next)
            stack.push_back(c);
    }
    const int depth = m_depth[std::size_t(node)];
    int last = row;
    while (last + 1 < int(m_rows.size()) && m_depth[std::size_t(m_rows[std::size_t(last + 1)])] > depth)
        ++last;
    if (last > row) {
        beginRemoveRows({}, row + 1, last);
        m_rows.erase(m_rows.begin() + row + 1, m_rows.begin() + last + 1);
        endRemoveRows();
    }
    if (open && m_document->expandable(node)) {
        std::vector<int> added;
        appendVisible(node, added);
        if (!added.empty()) {
            beginInsertRows({}, row + 1, row + int(added.size()));
            m_rows.insert(m_rows.begin() + row + 1, added.begin(), added.end());
            endInsertRows();
        }
    }
    emit dataChanged(index(row), index(row), {ExpandedRole});
}

int JsonTree::parentRow(int row) const
{
    if (row <= 0 || row >= int(m_rows.size()))
        return -1;
    const int depth = m_depth[std::size_t(m_rows[std::size_t(row)])];
    for (int r = row - 1; r >= 0; --r) {
        if (m_depth[std::size_t(m_rows[std::size_t(r)])] < depth)
            return r;
    }
    return -1;
}

int JsonTree::firstChildRow(int row) const
{
    if (row < 0 || row + 1 >= int(m_rows.size()))
        return -1;
    const int node = m_rows[std::size_t(row)];
    return m_open[std::size_t(node)] && m_depth[std::size_t(m_rows[std::size_t(row + 1)])] > m_depth[std::size_t(node)] ? row + 1 : -1;
}

int JsonTree::rowOf(int node) const
{
    const auto it = std::find(m_rows.begin(), m_rows.end(), node);
    return it == m_rows.end() ? -1 : int(it - m_rows.begin());
}

int JsonTree::reveal(int node)
{
    const auto& nodes = m_document->nodes;
    std::vector<int> chain;
    for (int p = nodes[std::size_t(node)].parent; p >= 0; p = nodes[std::size_t(p)].parent) {
        const int up = nodes[std::size_t(p)].parent;
        const bool embeddedRoot = up >= 0 && nodes[std::size_t(up)].embedded && nodes[std::size_t(up)].first == p;
        if (!embeddedRoot && m_depth[std::size_t(p)] >= 0)
            chain.push_back(p);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        open(rowOf(*it));
    return rowOf(node);
}

QString JsonTree::path(int row) const
{
    if (row < 0 || row >= int(m_rows.size()))
        return {};
    const auto& nodes = m_document->nodes;
    QString path;
    for (int n = m_rows[std::size_t(row)]; n >= 0;) {
        const int p = nodes[std::size_t(n)].parent;
        if (p < 0)
            break;
        if (nodes[std::size_t(p)].embedded && nodes[std::size_t(p)].first == n) { // a string's own document: on from the string
            n = p;
            continue;
        }
        const Node& node = nodes[std::size_t(n)];
        if (nodes[std::size_t(p)].kind == Kind::Array)
            path.prepend(u'[' + QString::number(node.index) + u']');
        else if (isIdentifier(node.key))
            path.prepend(u'.' + node.key);
        else
            path.prepend(u'[' + quoted(node.key) + u']');
        n = p;
    }
    return path.startsWith(u'.') ? path.sliced(1) : path;
}

QString JsonTree::key(int row) const
{
    if (row < 0 || row >= int(m_rows.size()))
        return {};
    return data(index(row), KeyRole).toString();
}

QString JsonTree::value(int row) const
{
    if (row < 0 || row >= int(m_rows.size()))
        return {};
    const int n = m_rows[std::size_t(row)];
    const Node& node = m_document->nodes[std::size_t(n)];
    if (node.kind == Kind::Object || node.kind == Kind::Array)
        return write(*m_document, n, Layout::Pretty, m_indent);
    return node.text;
}

QString JsonTree::json(int row, bool pretty) const
{
    if (row < 0 || row >= int(m_rows.size()))
        return {};
    return write(*m_document, m_document->container(m_rows[std::size_t(row)]), pretty ? Layout::Pretty : Layout::Compact, m_indent);
}

int JsonTree::search(const QString& words)
{
    m_words = words;
    std::fill(m_match.begin(), m_match.end(), quint8(0));
    m_matches.clear();
    m_current = -1;
    if (m_document && !m_document->isEmpty() && !words.isEmpty()) {
        const auto& nodes = m_document->nodes;
        // In the order shown: depth first, members in order, a string's document under it.
        std::vector<int> stack;
        if (m_depth[0] < 0) {
            std::vector<int> top;
            for (int c = m_document->firstChild(0); c >= 0; c = nodes[std::size_t(c)].next)
                top.push_back(c);
            stack.assign(top.rbegin(), top.rend());
        } else {
            stack.push_back(0);
        }
        while (!stack.empty()) {
            const int n = stack.back();
            stack.pop_back();
            const Node& node = nodes[std::size_t(n)];
            quint8 flags = 0;
            const int parent = node.parent;
            if (parent >= 0 && nodes[std::size_t(parent)].kind == Kind::Object && node.key.contains(words, Qt::CaseInsensitive))
                flags |= 1;
            if (!node.embedded && node.kind != Kind::Object && node.kind != Kind::Array && node.text.contains(words, Qt::CaseInsensitive))
                flags |= 2;
            if (flags) {
                m_match[std::size_t(n)] = flags;
                m_matches.push_back(n);
            }
            std::vector<int> children;
            for (int c = m_document->firstChild(n); c >= 0; c = nodes[std::size_t(c)].next)
                children.push_back(c);
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
    }
    int row = -1;
    if (!m_matches.empty()) {
        m_current = 0;
        row = reveal(m_matches.front());
    }
    if (!m_rows.empty())
        emit dataChanged(index(0), index(int(m_rows.size()) - 1), {MatchRole, CurrentRole});
    emit matchesChanged();
    return row;
}

int JsonTree::currentMatchRow() const
{
    return m_current >= 0 ? rowOf(m_matches[std::size_t(m_current)]) : -1;
}

int JsonTree::step(int direction)
{
    if (m_matches.empty())
        return -1;
    const int count = int(m_matches.size());
    m_current = ((m_current + (direction < 0 ? -1 : 1)) % count + count) % count;
    const int row = reveal(m_matches[std::size_t(m_current)]);
    if (!m_rows.empty())
        emit dataChanged(index(0), index(int(m_rows.size()) - 1), {CurrentRole});
    emit matchesChanged();
    return row;
}

} // namespace ws::devtools
