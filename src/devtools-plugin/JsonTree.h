#pragma once

#include "Json.h"

#include <QAbstractListModel>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>
#include <vector>

namespace ws::devtools {

// The rows of the JSON window's tree: a member or an element a row, those
// of an open object or array under it. The root's own row is left out (its
// members start the list) unless it is a lone value. Searching opens what
// a match lies in.
class JsonTree : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the tool")
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchesChanged)
    Q_PROPERTY(int matchNumber READ matchNumber NOTIFY matchesChanged) // the current match from 1; 0 for none
    Q_PROPERTY(int levels READ levels NOTIFY documentChanged) // how deep it goes, for 展开到第几层

public:
    enum Role {
        DepthRole = Qt::UserRole + 1,
        KeyRole, // a member's name, or an element's index
        KeyIsIndexRole,
        HasKeyRole, // false for a lone value at the root
        KindRole, // Kind, as an int; an embedded string is Kind::String
        ShapeRole, // what it shows as: Kind::Object for an embedded string holding one
        ValueRole, // a scalar as shown: a string in quotes, escapes visible, cut short
        PreviewRole, // a closed object or array: its first members, cut short
        CountRole,
        ExpandableRole,
        ExpandedRole,
        EmbeddedRole,
        MatchRole, // 1 the key, 2 the value, 3 both
        CurrentRole, // the match stepped to
    };
    Q_ENUM(Role)

    explicit JsonTree(QObject* parent = nullptr);

    void setDocument(std::shared_ptr<const Document> document);
    const Document* document() const { return m_document.get(); }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void toggle(int row);
    Q_INVOKABLE void setExpanded(int row, bool open);
    Q_INVOKABLE void expandAll();
    Q_INVOKABLE void collapseAll();
    Q_INVOKABLE void expandToLevel(int level); // 1: the root's members only
    Q_INVOKABLE void setSubtreeExpanded(int row, bool open); // the row and all under it
    Q_INVOKABLE int parentRow(int row) const;
    Q_INVOKABLE int firstChildRow(int row) const; // -1 when it is closed or has none

    // For the clipboard: data.items[3].id; a member's name; the value (a
    // string as its text, an object or array as JSON).
    Q_INVOKABLE QString path(int row) const;
    Q_INVOKABLE QString key(int row) const;
    Q_INVOKABLE QString value(int row) const;
    Q_INVOKABLE QString json(int row, bool pretty) const;

    // Matches `words` in names and values, case aside; the row of the first
    // (what it lies in opened), or -1.
    Q_INVOKABLE int search(const QString& words);
    Q_INVOKABLE int step(int direction); // to the next (1) or previous (-1) match: its row
    Q_INVOKABLE int currentMatchRow() const; // -1 for none (searched again for a new text)
    int matchCount() const { return int(m_matches.size()); }
    int matchNumber() const { return m_current + 1; }
    int levels() const { return m_levels; }

    void setIndent(const QString& indent) { m_indent = indent; }
    int rowOf(int node) const;

signals:
    void matchesChanged();
    void documentChanged();

private:
    void open(int row);
    void close(int row);
    void appendVisible(int node, std::vector<int>& out) const; // what is under an open node, in order
    void rebuild(); // the rows from m_open, the model reset
    int reveal(int node); // its ancestors opened: its row
    void computeDepths();
    QString shown(int node) const;
    QString preview(int node) const;

    std::shared_ptr<const Document> m_document;
    std::vector<int> m_rows; // the nodes shown, in order
    std::vector<int> m_depth; // per node: its indent
    std::vector<quint8> m_open; // per node
    std::vector<quint8> m_match; // per node: MatchRole
    std::vector<int> m_matches; // the nodes matched, in the order shown
    int m_current = -1;
    int m_levels = 0;
    QString m_words;
    QString m_indent = QStringLiteral("  ");
};

} // namespace ws::devtools
