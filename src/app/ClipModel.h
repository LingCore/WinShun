#pragma once

#include "ClipStore.h"

#include <QAbstractListModel>
#include <QColor>
#include <QHash>
#include <QList>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace ws {

// The entries the clipboard page lists (ClipStore::find), with what a row
// shows. Several can be picked; the order they were picked in is the order
// they are pasted in, and each picked row shows its number.
class ClipModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by Clipboard")
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QColor highlightColor READ highlightColor WRITE setHighlightColor NOTIFY highlightColorChanged FINAL)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        KindRole, // ClipKind
        TitleRole, // the first line (or where the query matched), highlighted: StyledText
        DetailRole, // "微信 · 3 分钟前 · 128 字"
        IconRole, // files and paths: the file's icon
        ImageRole, // pictures: their file
        ColorRole, // text that is a colour ("#3B82F6"): that colour; else empty
        OrderRole, // picked: 1, 2, ... in the order picked; 0 if not
        GroupRole, // the group's name, "" if none
        PinnedRole, // in "固定"
        MissingRole, // files and paths: the (first) file is gone
    };

    explicit ClipModel(const ClipStore* store, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_ids.size()); }
    const Clip* at(int row) const;
    int rowOf(qint64 id) const; // -1 if not listed
    Q_INVOKABLE qint64 idAt(int row) const; // 0 if out of range

    // The rows to show: indexes into the store's clips(); set them again
    // whenever the store changes. Rows that stay keep their delegates (rows
    // that come and go are inserted and removed, not the whole list reset).
    // Picked entries stay picked while they are in the history, listed or
    // not: pick one, search for the next, pick that too.
    void setRows(const std::vector<int>& rows, const QString& query);
    void refreshTimes(); // "3 分钟前" moved on
    void retranslate() { refreshTimes(); }
    void pictureSaved(qint64 id); // its file is there now (ClipStore::imagePending)

    int selectedCount() const { return static_cast<int>(m_selected.size()); }
    Q_INVOKABLE bool isSelected(int row) const;
    void toggle(int row);
    void selectRange(int from, int to, bool add);
    void clearSelection();
    std::vector<const Clip*> selection() const; // in the order picked

    QColor highlightColor() const { return m_highlightColor; }
    void setHighlightColor(const QColor& color);

    // "刚刚", "3 分钟前", "昨天 14:32", "9月30日"
    static QString when(qint64 ms, qint64 now);
    // "#3b82f6" for a colour value (colortext::parse), "#803b82f6" with alpha, else empty.
    static QString colorOf(const Clip& clip);

signals:
    void countChanged();
    void selectionChanged();
    void highlightColorChanged();

private:
    QString title(const Clip& clip) const;
    QString detail(const Clip& clip) const;
    QString groupName(qint64 group) const;
    bool fileMissing(const Clip& clip) const;
    void selectionEdited(); // repaints the numbers

    const ClipStore* m_store;
    std::vector<qint64> m_ids; // of the rows
    QHash<qint64, int> m_index; // id -> index in the store's clips(), as of the last setRows()
    QString m_query;
    ClipMatcher m_matcher {QString()}; // of m_query, for the titles
    QList<qint64> m_selected; // ids, in the order picked
    QColor m_highlightColor {0x00, 0x5F, 0xB8};
    qint64 m_now = 0; // what "3 分钟前" is counted from
    mutable QHash<QString, bool> m_missing; // path -> gone, while the rows stay
};

} // namespace ws
