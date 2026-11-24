#include "AnnotationCommands.h"

#include <QCoreApplication>

#include <algorithm>
#include <utility>

namespace ws::capture::annot {

namespace {

bool anyNumber(const QList<Item>& items)
{
    return std::any_of(items.cbegin(), items.cend(), [](const Item& item) { return item.kind == Kind::Number; });
}

QRectF numberArea(const Document& doc, const QHash<quint32, int>& shown)
{
    QRectF area;
    for (const Item& item : doc.items) {
        if (item.kind == Kind::Number)
            area |= bounds(item, doc.dpr, shown.value(item.id, 1), formatOf(doc, item.series));
    }
    return area;
}

bool touchesNumber(const Document& doc, const QList<quint32>& ids)
{
    return std::any_of(ids.cbegin(), ids.cend(), [&](quint32 id) {
        const Item* item = findItem(doc, id);
        return item && item->kind == Kind::Number;
    });
}

struct SpotlightVeil {
    bool any = false;
    QRgb color = 0;
    int opacity = 0;
    bool operator==(const SpotlightVeil&) const = default;
};

SpotlightVeil spotlightVeil(const Document& doc)
{
    SpotlightVeil v;
    for (const Item& item : doc.items) {
        if (item.kind != Kind::Spotlight || itemBox(item).isEmpty())
            continue;
        v.any = true;
        v.color = item.style.color;
        v.opacity = std::clamp(item.style.spotlightOpacity, 10, 100);
    }
    return v;
}

// What a command touches, measured before it changes the document and
// finished after: the touched items' old and new bounds; with `numbers`
// (what Numbers show may change) every Number's old and new bounds; and,
// when the redaction boxes changed, every redaction's, because each one is
// computed from the pixels outside all of them (3.10).
// Where an item's pixels may change: its bounds, but for a big rectangle
// with no fill only the strips along its edge (a rectangle's outline is
// inside its bounds, wide as its line; the middle stays what it was).
void addAreas(QList<QRectF>& out, const Item& item, const QRectF& bounds)
{
    const qreal t = item.style.width + 3; // the line and its antialiased fringe
    const bool hollow = item.kind == Kind::Rect && item.style.fill == Fill::None;
    if (!hollow || bounds.width() <= 2 * t + 8 || bounds.height() <= 2 * t + 8) {
        out.append(bounds);
        return;
    }
    out.append(QRectF(bounds.left(), bounds.top(), bounds.width(), t));
    out.append(QRectF(bounds.left(), bounds.bottom() - t, bounds.width(), t));
    out.append(QRectF(bounds.left(), bounds.top() + t, t, bounds.height() - 2 * t));
    out.append(QRectF(bounds.right() - t, bounds.top() + t, t, bounds.height() - 2 * t));
}

class Measure {
public:
    Measure(const Document& doc, QList<quint32> ids, bool numbers)
        : m_ids(std::move(ids))
        , m_numbers(numbers)
        , m_boxes(redactionBoxes(doc))
        , m_spotlight(spotlightVeil(doc))
    {
        QHash<quint32, int> shown;
        if (m_numbers || touchesNumber(doc, m_ids))
            shown = annot::numbers(doc);
        for (quint32 id : std::as_const(m_ids)) {
            if (const Item* item = findItem(doc, id)) {
                const QRectF b = bounds(*item, doc.dpr, shown.value(id, 1), formatOf(doc, item->series));
                m_dirty |= b;
                addAreas(m_parts, *item, b);
            }
        }
        if (m_numbers)
            m_numberArea = numberArea(doc, shown);
    }

    Change finish(const Document& doc) const
    {
        Change change;
        change.ids = m_ids;
        change.dirty = m_dirty;
        QHash<quint32, int> shown;
        if (m_numbers || touchesNumber(doc, m_ids))
            shown = annot::numbers(doc);
        QList<QRectF> parts = m_parts;
        for (quint32 id : m_ids) {
            if (const Item* item = findItem(doc, id)) {
                const QRectF b = bounds(*item, doc.dpr, shown.value(id, 1), formatOf(doc, item->series));
                change.dirty |= b;
                addAreas(parts, *item, b);
            }
        }
        if (m_numbers) {
            change.numbers = true;
            change.dirty |= m_numberArea | numberArea(doc, shown);
            parts.append(m_numberArea | numberArea(doc, shown));
        }
        const QList<QRect> boxes = redactionBoxes(doc);
        if (boxes != m_boxes) {
            change.redactions = true;
            for (const QRect& box : m_boxes) {
                change.dirty |= QRectF(box);
                parts.append(QRectF(box));
            }
            for (const QRect& box : boxes) {
                change.dirty |= QRectF(box);
                parts.append(QRectF(box));
            }
        }
        if (spotlightVeil(doc) != m_spotlight) {
            const QRectF full(QPointF(0, 0), QSizeF(doc.canvas));
            change.dirty |= full;
            parts.append(full);
        }
        // Pieces only when they say less than the whole.
        qreal sum = 0;
        for (const QRectF& r : parts)
            sum += r.width() * r.height();
        if (parts.size() > 1 && sum < 0.5 * change.dirty.width() * change.dirty.height())
            change.parts = parts;
        return change;
    }

private:
    QList<quint32> m_ids;
    bool m_numbers = false;
    QList<QRect> m_boxes;
    SpotlightVeil m_spotlight;
    QRectF m_dirty;
    QList<QRectF> m_parts;
    QRectF m_numberArea;
};

QList<quint32> idsOf(const QList<Item>& items)
{
    QList<quint32> ids;
    ids.reserve(items.size());
    for (const Item& item : items)
        ids.append(item.id);
    return ids;
}

bool numberingDiffers(const Item& a, const Item& b)
{
    return (a.kind == Kind::Number || b.kind == Kind::Number)
        && (a.kind != b.kind || a.series != b.series || a.seq != b.seq || a.pinned != b.pinned);
}

} // namespace

QList<QRectF> Change::areas() const
{
    if (!parts.isEmpty())
        return parts;
    return dirty.isEmpty() ? QList<QRectF>() : QList<QRectF> {dirty};
}

void Change::merge(const Change& other)
{
    for (quint32 id : other.ids) {
        if (!ids.contains(id))
            ids.append(id);
    }
    if (!parts.isEmpty() || !other.parts.isEmpty())
        parts = areas() + other.areas();
    dirty |= other.dirty;
    numbers = numbers || other.numbers;
    redactions = redactions || other.redactions;
    crop = crop || other.crop;
}

AddItems::AddItems(QList<Item> items_, int at_)
    : items(std::move(items_))
    , at(at_)
{
    label = QCoreApplication::translate("Annotation", "添加");
}

Change AddItems::apply(Document& doc)
{
    const Measure measure(doc, idsOf(items), anyNumber(items));
    m_nextId = doc.nextId;
    m_nextSeq = doc.nextSeq;
    for (qsizetype k = 0; k < items.size(); ++k) {
        const Item& item = items[k];
        doc.items.insert(insertionIndex(doc, item.kind, at < 0 ? -1 : at + int(k)), item);
        doc.nextId = std::max(doc.nextId, item.id + 1);
        doc.nextSeq = std::max(doc.nextSeq, item.seq + 1);
    }
    return measure.finish(doc);
}

Change AddItems::revert(Document& doc)
{
    const Measure measure(doc, idsOf(items), anyNumber(items));
    for (const Item& item : std::as_const(items)) {
        const int i = indexOf(doc, item.id);
        if (i >= 0)
            doc.items.removeAt(i);
    }
    doc.nextId = m_nextId;
    doc.nextSeq = m_nextSeq;
    return measure.finish(doc);
}

RemoveItems::RemoveItems(QList<quint32> ids_)
    : ids(std::move(ids_))
{
    label = QCoreApplication::translate("Annotation", "删除");
}

Change RemoveItems::apply(Document& doc)
{
    QList<int> found;
    for (quint32 id : std::as_const(ids)) {
        const int i = indexOf(doc, id);
        if (i >= 0)
            found.append(i);
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    items.clear();
    at = found;
    for (int i : std::as_const(found))
        items.append(doc.items[i]);
    const Measure measure(doc, ids, anyNumber(items));
    for (qsizetype k = found.size() - 1; k >= 0; --k)
        doc.items.removeAt(found[k]);
    return measure.finish(doc);
}

Change RemoveItems::revert(Document& doc)
{
    const Measure measure(doc, ids, anyNumber(items));
    // Back at their indices, lowest first: each lands where it was.
    for (qsizetype k = 0; k < items.size(); ++k)
        doc.items.insert(std::min(at[k], int(doc.items.size())), items[k]);
    return measure.finish(doc);
}

MoveItems::MoveItems(QList<quint32> ids_, QPoint delta_)
    : ids(std::move(ids_))
    , delta(delta_)
{
    label = QCoreApplication::translate("Annotation", "移动");
}

Change MoveItems::apply(Document& doc)
{
    const Measure measure(doc, ids, false);
    for (quint32 id : std::as_const(ids)) {
        const int i = indexOf(doc, id);
        if (i >= 0)
            doc.items[i] = translated(doc.items[i], delta);
    }
    return measure.finish(doc);
}

Change MoveItems::revert(Document& doc)
{
    const Measure measure(doc, ids, false);
    for (quint32 id : std::as_const(ids)) {
        const int i = indexOf(doc, id);
        if (i >= 0)
            doc.items[i] = translated(doc.items[i], -delta);
    }
    return measure.finish(doc);
}

bool MoveItems::absorb(const Command& next)
{
    const auto* move = dynamic_cast<const MoveItems*>(&next);
    if (!move || !merge || !move->merge || move->ids != ids)
        return false;
    delta += move->delta;
    return true;
}

Reshape::Reshape(quint32 id_, Item before_, Item after_)
    : id(id_)
    , before(std::move(before_))
    , after(std::move(after_))
{
    label = QCoreApplication::translate("Annotation", "改形状");
}

Reshape::Reshape(const Document& doc, Item after_)
    : Reshape(after_.id, findItem(doc, after_.id) ? *findItem(doc, after_.id) : after_, after_)
{
}

Change Reshape::apply(Document& doc)
{
    return put(doc, after);
}

Change Reshape::revert(Document& doc)
{
    return put(doc, before);
}

bool Reshape::absorb(const Command& next)
{
    const auto* reshape = dynamic_cast<const Reshape*>(&next);
    if (!reshape || !merge || !reshape->merge || reshape->id != id)
        return false;
    after = reshape->after;
    return true;
}

Change Reshape::put(Document& doc, const Item& item) const
{
    const Measure measure(doc, {id}, numberingDiffers(before, after));
    const int i = indexOf(doc, id);
    if (i >= 0)
        doc.items[i] = item;
    return measure.finish(doc);
}

Restyle::Restyle(QList<quint32> ids_, QList<Style> before_, QList<Style> after_)
    : ids(std::move(ids_))
    , before(std::move(before_))
    , after(std::move(after_))
{
    label = QCoreApplication::translate("Annotation", "改样式");
}

Restyle::Restyle(const Document& doc, QList<quint32> ids_, QList<Style> after_)
    : Restyle(ids_, {}, std::move(after_))
{
    for (quint32 id : std::as_const(ids)) {
        const Item* item = findItem(doc, id);
        before.append(item ? item->style : Style{});
    }
}

Change Restyle::apply(Document& doc)
{
    return put(doc, after);
}

Change Restyle::revert(Document& doc)
{
    return put(doc, before);
}

bool Restyle::absorb(const Command& next)
{
    const auto* restyle = dynamic_cast<const Restyle*>(&next);
    if (!restyle || !merge || !restyle->merge || restyle->ids != ids)
        return false;
    after = restyle->after;
    return true;
}

Change Restyle::put(Document& doc, const QList<Style>& styles) const
{
    const Measure measure(doc, ids, false);
    for (qsizetype k = 0; k < ids.size() && k < styles.size(); ++k) {
        const int i = indexOf(doc, ids[k]);
        if (i >= 0)
            doc.items[i].style = styles[k];
    }
    return measure.finish(doc);
}

Retext::Retext(quint32 id_, QString before_, QString after_)
    : id(id_)
    , before(std::move(before_))
    , after(std::move(after_))
{
    label = QCoreApplication::translate("Annotation", "改文字");
}

Retext::Retext(const Document& doc, quint32 id_, QString after_)
    : Retext(id_, findItem(doc, id_) ? findItem(doc, id_)->text : QString(), std::move(after_))
{
}

Change Retext::apply(Document& doc)
{
    return put(doc, after);
}

Change Retext::revert(Document& doc)
{
    return put(doc, before);
}

Change Retext::put(Document& doc, const QString& text) const
{
    const Measure measure(doc, {id}, false);
    const int i = indexOf(doc, id);
    if (i >= 0)
        doc.items[i].text = text;
    return measure.finish(doc);
}

Renumber::Renumber(quint32 id_, std::optional<int> before_, std::optional<int> after_)
    : id(id_)
    , before(before_)
    , after(after_)
{
    label = QCoreApplication::translate("Annotation", "改序号");
}

Renumber::Renumber(const Document& doc, quint32 id_, std::optional<int> after_)
    : Renumber(id_, findItem(doc, id_) ? findItem(doc, id_)->pinned : std::nullopt, after_)
{
}

Change Renumber::apply(Document& doc)
{
    return put(doc, after);
}

Change Renumber::revert(Document& doc)
{
    return put(doc, before);
}

Change Renumber::put(Document& doc, std::optional<int> pinned) const
{
    const Measure measure(doc, {id}, true);
    const int i = indexOf(doc, id);
    if (i >= 0)
        doc.items[i].pinned = pinned;
    return measure.finish(doc);
}

Reorder::Reorder(quint32 id_, int to_)
    : id(id_)
    , to(to_)
{
    label = QCoreApplication::translate("Annotation", "调层");
}

Change Reorder::apply(Document& doc)
{
    const Measure measure(doc, {id}, false);
    from = indexOf(doc, id);
    if (from >= 0) {
        const Item item = doc.items.takeAt(from);
        doc.items.insert(insertionIndex(doc, item.kind, to), item);
    }
    return measure.finish(doc);
}

Change Reorder::revert(Document& doc)
{
    const Measure measure(doc, {id}, false);
    const int i = indexOf(doc, id);
    if (i >= 0 && from >= 0) {
        const Item item = doc.items.takeAt(i);
        doc.items.insert(std::min(from, int(doc.items.size())), item);
    }
    return measure.finish(doc);
}

SetSeries::SetSeries(quint32 id_, std::optional<Series> before_, std::optional<Series> after_, quint32 currentBefore_,
    quint32 currentAfter_)
    : id(id_)
    , before(before_)
    , after(after_)
    , currentBefore(currentBefore_)
    , currentAfter(currentAfter_)
{
    label = QCoreApplication::translate("Annotation", "改序号");
}

std::unique_ptr<SetSeries> SetSeries::newSeries(const Document& doc)
{
    quint32 id = 0;
    for (const Series& s : doc.series)
        id = std::max(id, s.id);
    ++id;
    // The new group writes its numbers as the one before did.
    auto command = std::make_unique<SetSeries>(id, std::nullopt, Series{id, 1, formatOf(doc, doc.currentSeries)}, doc.currentSeries, id);
    command->label = QCoreApplication::translate("Annotation", "另起一组");
    return command;
}

std::unique_ptr<SetSeries> SetSeries::setStart(const Document& doc, quint32 id, int start)
{
    std::optional<Series> before;
    for (const Series& s : doc.series) {
        if (s.id == id)
            before = s;
    }
    Series after = before.value_or(Series{id});
    after.start = start;
    auto command = std::make_unique<SetSeries>(id, before, after, 0, 0);
    command->label = QCoreApplication::translate("Annotation", "改起始值");
    return command;
}

std::unique_ptr<SetSeries> SetSeries::setFormat(const Document& doc, quint32 id, NumberFormat format)
{
    std::optional<Series> before;
    for (const Series& s : doc.series) {
        if (s.id == id)
            before = s;
    }
    Series after = before.value_or(Series{id});
    after.format = format;
    auto command = std::make_unique<SetSeries>(id, before, after, 0, 0);
    command->label = QCoreApplication::translate("Annotation", "改序号格式");
    return command;
}

Change SetSeries::apply(Document& doc)
{
    return put(doc, after, currentAfter);
}

Change SetSeries::revert(Document& doc)
{
    return put(doc, before, currentBefore);
}

Change SetSeries::put(Document& doc, const std::optional<Series>& value, quint32 current) const
{
    const Measure measure(doc, {}, true);
    qsizetype at = -1;
    for (qsizetype i = 0; i < doc.series.size(); ++i) {
        if (doc.series[i].id == id)
            at = i;
    }
    if (value) {
        if (at >= 0)
            doc.series[at] = *value;
        else
            doc.series.append(*value);
    } else if (at >= 0) {
        doc.series.removeAt(at);
    }
    if (current != 0)
        doc.currentSeries = current;
    return measure.finish(doc);
}

SetCrop::SetCrop(QRect before_, QRect after_)
    : before(before_)
    , after(after_)
{
    label = QCoreApplication::translate("Annotation", "裁剪");
}

SetCrop::SetCrop(const Document& doc, QRect after_)
    : SetCrop(doc.crop, after_)
{
}

Change SetCrop::apply(Document& doc)
{
    doc.crop = after;
    Change change;
    change.crop = true;
    return change;
}

Change SetCrop::revert(Document& doc)
{
    doc.crop = before;
    Change change;
    change.crop = true;
    return change;
}

ReplaceDocument::ReplaceDocument(Document before_, Document after_)
    : before(std::move(before_))
    , after(std::move(after_))
{
    label = QCoreApplication::translate("Annotation", "放弃修改");
}

Change ReplaceDocument::put(Document& doc, const Document& to) const
{
    Change change;
    change.ids = idsOf(doc.items);
    for (const Item& item : to.items) {
        if (!change.ids.contains(item.id))
            change.ids.append(item.id);
    }
    change.dirty = QRectF(QPointF(0, 0), QSizeF(doc.canvas.expandedTo(to.canvas)));
    change.numbers = true;
    change.redactions = true;
    change.crop = true;
    doc = to;
    return change;
}

Change ReplaceDocument::apply(Document& doc)
{
    return put(doc, after);
}

Change ReplaceDocument::revert(Document& doc)
{
    return put(doc, before);
}

Batch::Batch(std::vector<std::unique_ptr<Command>> steps_)
    : steps(std::move(steps_))
{
    if (!steps.empty() && steps.front())
        label = steps.front()->label;
}

Change Batch::apply(Document& doc)
{
    Change change;
    for (const auto& step : steps) {
        if (step)
            change.merge(step->apply(doc));
    }
    return change;
}

Change Batch::revert(Document& doc)
{
    Change change;
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (*it)
            change.merge((*it)->revert(doc));
    }
    return change;
}

Change CommandStack::push(std::unique_ptr<Command> command, Document& doc, qint64 nowMs)
{
    if (!command)
        return {};
    Change change = command->apply(doc);
    keep(std::move(command), nowMs);
    return change;
}

void CommandStack::pushApplied(std::unique_ptr<Command> command, qint64 nowMs)
{
    if (command)
        keep(std::move(command), nowMs);
}

void CommandStack::keep(std::unique_ptr<Command> command, qint64 nowMs)
{
    const bool onTop = m_done == int(m_steps.size());
    if (m_mergeable && onTop && m_done > 0 && nowMs >= m_lastMs && nowMs - m_lastMs <= kMergeMs
        && m_steps.back()->absorb(*command)) {
        m_lastMs = nowMs;
        return;
    }
    m_steps.erase(m_steps.begin() + m_done, m_steps.end());
    m_steps.push_back(std::move(command));
    if (int(m_steps.size()) > kLimit)
        m_steps.erase(m_steps.begin());
    m_done = int(m_steps.size());
    m_lastMs = nowMs;
    m_mergeable = true;
}

Change CommandStack::undo(Document& doc)
{
    if (m_done == 0)
        return {};
    m_mergeable = false;
    --m_done;
    return m_steps[std::size_t(m_done)]->revert(doc);
}

Change CommandStack::redo(Document& doc)
{
    if (m_done >= int(m_steps.size()))
        return {};
    m_mergeable = false;
    Change change = m_steps[std::size_t(m_done)]->apply(doc);
    ++m_done;
    return change;
}

bool CommandStack::canUndo() const
{
    return m_done > 0;
}

bool CommandStack::canRedo() const
{
    return m_done < int(m_steps.size());
}

void CommandStack::clear()
{
    m_steps.clear();
    m_done = 0;
    m_lastMs = 0;
    m_mergeable = false;
}

int CommandStack::count() const
{
    return int(m_steps.size());
}

int CommandStack::position() const
{
    return m_done;
}

QString CommandStack::undoLabel() const
{
    return m_done > 0 ? m_steps[std::size_t(m_done - 1)]->label : QString();
}

QString CommandStack::redoLabel() const
{
    return canRedo() ? m_steps[std::size_t(m_done)]->label : QString();
}

}
