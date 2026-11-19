// The settings window's rules, for QML: the plugin's own rules (Gesture.h)
// in a list model, and filters that show the ones a page is about (the
// gestures drawn anywhere, those with words selected, one program's).
#pragma once

#include "Gesture.h"

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include <vector>

namespace ws::gestureseditor {

QString qstr(const std::wstring& text);
std::wstring wstr(const QString& text);
// An action from what the window's picker chose: kind "builtin" (value: its id), "keys",
// "open", "type" (value: the text) or "nothing".
gestures::Action makeAction(const QString& kind, const QString& value);
// "builtin" "keys" "open" "type" "nothing" "invalid": how QML names an action's kind.
QString kindName(gestures::Action::Kind kind);
// The Phosphor icon that stands for an action ("ph:globe": its category's), as Glyph names it.
QString actionIcon(const gestures::Action& action);

class RulesModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS

public:
    enum Role {
        SourceRowRole = Qt::UserRole + 1,
        OnRole,
        ArrowsRole, // "↓→"
        DescriptionRole, // "向下，再向右"
        WhereRole, // gestures::Zone as a number
        WhereTitleRole, // "任何位置", "从右下角开始画"
        SelectionRole,
        AppsRole,
        KindRole, // "builtin" "keys" …
        ActionTitleRole,
        ActionNoteRole, // "用选中的文字", "会先倒数 5 秒，可取消"
        ActionIconRole,
        ActionIdRole, // the builtin's id, or the text of a keys / open / type action
        ProvisionalRole, // added from the pad, not kept until it has an action
    };

    explicit RulesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    const std::vector<gestures::Rule>& rules() const { return m_rules; }
    // What is kept: all but the row not yet given an action.
    std::vector<gestures::Rule> saved() const;
    // Whole replacement (the plugin sent others): nothing in progress survives.
    void setRules(std::vector<gestures::Rule> rules);
    void setChinese(bool chinese);

    // The row of a rule for the same thing (gestures::Rule::sameCase), or -1.
    int find(const gestures::Rule& probe) const;
    // Appended. `provisional`: shown, but not part of saved() until it gets an action.
    int add(gestures::Rule rule, bool provisional);
    void setAction(int row, const gestures::Action& action);
    // False when another rule is for the same thing there already.
    bool setWhere(int row, gestures::Zone zone);
    void removeAt(int row);
    // A rule for several programs, shown under `app`: split, the one for `app` alone put right
    // after the rest. Its row; `row` itself when it was for `app` alone.
    int isolate(int row, const std::wstring& app);
    // Without `app`: it leaves the rule's programs (the rule goes with the last one).
    void removeApp(int row, const std::wstring& app);
    int provisionalRow() const { return m_provisional; }
    void discardProvisional();

signals:
    // What saved() holds changed (not the row in progress).
    void edited();

private:
    void changed(int row);

    std::vector<gestures::Rule> m_rules;
    int m_provisional = -1;
    bool m_chinese = true;
};

// The rows a page shows: mode 0 the gestures drawn anywhere, 1 those with words selected, 2 a program's.
class RulesFilter : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString app READ app WRITE setApp NOTIFY appChanged)

public:
    explicit RulesFilter(QObject* parent = nullptr);

    int mode() const { return m_mode; }
    void setMode(int mode);
    QString app() const { return m_app; }
    void setApp(const QString& app);
    // The row of this filter that shows `sourceRow`, or -1.
    Q_INVOKABLE int proxyRow(int sourceRow) const;

signals:
    void modeChanged();
    void appChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& parent) const override;

private:
    int m_mode = 0;
    QString m_app;
};

} // namespace ws::gestureseditor
