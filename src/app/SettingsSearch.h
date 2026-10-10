#pragma once

#include "SettingsMatch.h"

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace ws {

// The settings window's search: the words typed into the box above the
// categories, matched against each option (each SettingRow asks match() for
// itself, in a binding), and which options are on show, by page and by
// section, as the rows report it.
class SettingsSearch : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged FINAL)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged FINAL) // some word typed
    Q_PROPERTY(QColor highlightColor READ highlightColor WRITE setHighlightColor NOTIFY highlightColorChanged FINAL)
    // Goes up whenever match() would answer differently: pass it to match(),
    // so that bindings ask again.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged FINAL)
    Q_PROPERTY(QList<int> counts READ counts NOTIFY countsChanged FINAL) // options on show, by page
    Q_PROPERTY(int total READ total NOTIFY countsChanged FINAL)
    // The option Enter goes to; ↑ and ↓ in the box move it.
    Q_PROPERTY(QObject* current READ current WRITE setCurrent NOTIFY currentChanged FINAL)

public:
    explicit SettingsSearch(QObject* parent = nullptr);

    QString query() const { return m_query; }
    void setQuery(const QString& query);
    bool active() const { return !m_parsed.isEmpty(); }
    QColor highlightColor() const { return m_color; }
    void setHighlightColor(const QColor& color);
    int revision() const { return m_revision; }
    QList<int> counts() const { return m_counts; }
    int total() const { return m_total; }
    QObject* current() const { return m_current; }
    void setCurrent(QObject* row);

    // How an option fits the query. `fields`: title, description, keywords,
    // options, context, values (see settingsmatch::Fields). Null when
    // it does not; else {score, title, description} with the words found
    // marked (StyledText), and the values a word is in.
    Q_INVOKABLE QVariant match(int revision, const QVariantMap& fields) const;

    // A row is on show with `score`, or no longer (-1).
    Q_INVOKABLE void report(QObject* row, QObject* section, int page, int score);
    Q_INVOKABLE int sectionCount(QObject* section) const;
    // The rows on show and their scores, [{row, score}], in no order.
    Q_INVOKABLE QVariantList results() const;

signals:
    void queryChanged();
    // After every change a new query makes: the rows matched again, the
    // counts, `active`. `wasActive`: whether a word was typed before.
    void queryApplied(bool wasActive);
    void activeChanged();
    void highlightColorChanged();
    void revisionChanged();
    void countsChanged();
    void currentChanged();

private:
    struct Shown {
        QPointer<QObject> section;
        int page = 0;
        int score = 0;
    };

    void recount();

    QString m_query;
    settingsmatch::Query m_parsed;
    QColor m_color;
    int m_revision = 0;
    QHash<QObject*, Shown> m_shown;
    QSet<QObject*> m_watched; // rows whose going away is watched
    QList<int> m_counts;
    QHash<QObject*, int> m_sectionCounts;
    int m_total = 0;
    QPointer<QObject> m_current;
};

} // namespace ws
