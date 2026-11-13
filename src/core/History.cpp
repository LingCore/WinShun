#include "History.h"

#include "Query.h"
#include "PathText.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ws {

namespace {

constexpr double kHalfLifeMs = 14.0 * 24 * 60 * 60 * 1000;
constexpr qsizetype kMostTexts = 8; // typed texts kept per path
constexpr qsizetype kLongestText = 64;
// In place of a typed text: last run as administrator (History::admin).
const QString kAdminMark = QChar(1) + QStringLiteral("admin");

// What `weight`, as of `at`, has decayed to by `now`. No time known (0) is
// taken as one half-life ago: old, but not forgotten.
double decayed(double weight, qint64 at, qint64 now)
{
    const double age = at <= 0 ? kHalfLifeMs : static_cast<double>(std::max<qint64>(0, now - at));
    return weight * std::exp2(-age / kHalfLifeMs);
}

} // namespace

History::History(QString filePath, int capacity)
    : m_filePath(std::move(filePath))
    , m_capacity(capacity)
{
}

QString History::key(const QString& path)
{
    return pathtext::pathKey(path);
}

void History::load()
{
    m_items.clear();
    m_uses.clear();
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    while (!file.atEnd() && m_items.size() < m_capacity) {
        const QStringList fields = QString::fromUtf8(file.readLine()).trimmed().split(u'\t');
        const QString& path = fields.first();
        if (path.isEmpty() || m_uses.contains(key(path)))
            continue;
        Entry entry;
        entry.use = {1, 0};
        if (fields.size() >= 3) {
            entry.use.count = std::max(1, fields[1].toInt());
            entry.use.last = std::max<qint64>(0, fields[2].toLongLong());
        }
        entry.weight = entry.use.count; // kept before weights were: as many as it was opened
        if (fields.size() >= 4) {
            bool ok = false;
            const double weight = fields[3].toDouble(&ok);
            if (ok && weight > 0)
                entry.weight = weight;
        }
        for (qsizetype i = 4; i + 2 < fields.size(); i += 3) {
            if (fields[i] == kAdminMark) {
                entry.admin = true;
                continue;
            }
            if (entry.typed.size() >= kMostTexts)
                continue;
            Typed typed;
            typed.text = fields[i];
            typed.weight = fields[i + 1].toDouble();
            typed.last = std::max<qint64>(0, fields[i + 2].toLongLong());
            if (typed.text.isEmpty() || typed.weight <= 0)
                continue;
            entry.typed.append(std::move(typed));
        }
        m_items.append(path);
        m_uses.insert(key(path), std::move(entry));
    }
}

QString History::normalize(const QString& query)
{
    QString text = query;
    text.replace(u'\t', u' ').replace(u'\r', u' ').replace(u'\n', u' ');
    // The folder to look in goes, as the content search takes it out ...
    const QStringList words = queryWords(splitContentQuery(text).words);
    // ... and the filters, as the name search takes them out.
    QStringList kept;
    const QDateTime now = QDateTime::currentDateTime();
    for (const QString& word : words) {
        EntryFilter filter;
        if (word.startsWith(u"size:", Qt::CaseInsensitive) && parseSizeToken(word, filter))
            continue;
        if (word.startsWith(u"dm:", Qt::CaseInsensitive) && parseDateToken(word, now, filter))
            continue;
        if (word.startsWith(u"ext:", Qt::CaseInsensitive))
            continue;
        kept.append(word);
    }
    return kept.join(u' ').simplified().toCaseFolded().left(kLongestText).trimmed();
}

void History::record(const QString& path, const QString& query, qint64 now)
{
    if (path.isEmpty())
        return;
    m_items.removeIf([&](const QString& p) { return pathtext::samePath(p, path); });
    m_items.prepend(path);
    Entry& entry = m_uses[key(path)];
    entry.weight = decayed(entry.weight, entry.use.last, now) + 1;
    ++entry.use.count;
    entry.use.last = now;

    const QString text = normalize(query);
    if (!text.isEmpty()) {
        qsizetype at = -1;
        for (qsizetype i = 0; i < entry.typed.size(); ++i) {
            if (entry.typed[i].text == text)
                at = i;
        }
        if (at < 0) {
            entry.typed.append({text, 0, now});
            at = entry.typed.size() - 1;
        }
        Typed& typed = entry.typed[at];
        typed.weight = decayed(typed.weight, typed.last, now) + 1;
        typed.last = now;
        if (entry.typed.size() > kMostTexts) {
            // The weakest as of now goes; not the one just typed.
            qsizetype weakest = -1;
            for (qsizetype i = 0; i < entry.typed.size(); ++i) {
                if (i != at
                    && (weakest < 0
                        || decayed(entry.typed[i].weight, entry.typed[i].last, now)
                            < decayed(entry.typed[weakest].weight, entry.typed[weakest].last, now)))
                    weakest = i;
            }
            entry.typed.removeAt(weakest);
        }
    }
    while (m_items.size() > m_capacity)
        m_uses.remove(key(m_items.takeLast()));
    save();
}

void History::remove(const QString& path)
{
    if (m_items.removeIf([&](const QString& p) { return pathtext::samePath(p, path); }) > 0) {
        m_uses.remove(key(path));
        save();
    }
}

void History::clear()
{
    if (m_items.isEmpty())
        return;
    m_items.clear();
    m_uses.clear();
    save();
}

History::Use History::use(const QString& path) const
{
    return m_uses.value(key(path)).use;
}

bool History::admin(const QString& path) const
{
    const auto it = m_uses.constFind(key(path));
    return it != m_uses.cend() && it->admin;
}

void History::setAdmin(const QString& path, bool on)
{
    const auto it = m_uses.find(key(path));
    if (it == m_uses.end() || it->admin == on)
        return;
    it->admin = on;
    save();
}

int History::Affinity::bonus() const noexcept
{
    if (weight <= 0)
        return 0;
    const double scale = group == 0 ? 40 : group == 1 ? 30 : group == 2 ? 24 : 20;
    return static_cast<int>(std::lround(scale * std::log2(1 + std::min(weight, 15.0))));
}

double History::Affinity::preferenceWeight() const noexcept
{
    // Query choices can change quickly; entity popularity retains its frequency.
    return group <= 2 ? std::min(weight, 2.5) : weight;
}

History::Affinity History::affinity(const Affinities& affinities, const QString& path)
{
    return affinities.value(key(path));
}

History::Affinities History::affinities(const QString& query, qint64 now) const
{
    const QString text = normalize(query);
    Affinities out;
    out.reserve(m_uses.size());
    for (auto it = m_uses.cbegin(); it != m_uses.cend(); ++it) {
        const Entry& entry = it.value();
        const double entityWeight = decayed(entry.weight, entry.use.last, now);
        Affinity best {3, entityWeight, entry.use.last};
        if (!text.isEmpty()) {
            for (const Typed& typed : entry.typed) {
                const int group = typed.text == text ? 0 : typed.text.contains(text) ? 1 : text.contains(typed.text) ? 2 : 3;
                if (group == 3)
                    continue;
                const double typedWeight = decayed(typed.weight, typed.last, now);
                if (typedWeight < 0.5) // an obsolete query must not hide a recent entity preference
                    continue;
                const double weight = std::max(typedWeight, entityWeight);
                if (group < best.group || (group == best.group && (weight > best.weight || (weight == best.weight && typed.last > best.last))))
                    best = {group, weight, typed.last};
            }
        }
        out.insert(it.key(), best);
    }
    return out;
}

QStringList History::ranked(const QString& query, qint64 now) const
{
    const QString text = normalize(query);
    if (text.isEmpty())
        return m_items;
    const Affinities matches = affinities(query, now);
    struct Rank {
        int group; // exact query, containing query, contained query, entity only
        double weight;
        qint64 last;
        qsizetype index; // in m_items: a tie goes to the newer
    };
    std::vector<Rank> ranks;
    ranks.reserve(static_cast<std::size_t>(m_items.size()));
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        const Affinity match = affinity(matches, m_items[i]);
        ranks.push_back({match.group, match.preferenceWeight(), match.last, i});
    }
    std::ranges::sort(ranks, [](const Rank& a, const Rank& b) {
        if (a.group != b.group)
            return a.group < b.group;
        if (a.weight != b.weight)
            return a.weight > b.weight;
        if (a.last != b.last)
            return a.last > b.last;
        return a.index < b.index;
    });
    QStringList out;
    out.reserve(m_items.size());
    for (const Rank& rank : ranks)
        out.append(m_items[rank.index]);
    return out;
}

void History::save() const
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    for (const QString& item : m_items) {
        const Entry entry = m_uses.value(key(item));
        file.write(item.toUtf8());
        file.write("\t" + QByteArray::number(entry.use.count) + "\t" + QByteArray::number(entry.use.last) + "\t"
            + QByteArray::number(entry.weight, 'g', 9));
        for (const Typed& typed : entry.typed) {
            file.write("\t" + typed.text.toUtf8() + "\t" + QByteArray::number(typed.weight, 'g', 9) + "\t"
                + QByteArray::number(typed.last));
        }
        if (entry.admin)
            file.write("\t" + kAdminMark.toUtf8() + "\t1\t" + QByteArray::number(entry.use.last));
        file.write("\n");
    }
    file.commit();
}

} // namespace ws
