#pragma once

#include <QString>
#include <QStringList>

#include <array>

namespace ws::rowactions {

// The buttons a result's row can show at its right end (ResultActions.qml),
// by the names the settings keep them under (Results/RowActions): which show,
// and in which order, left to right. A button still shows only on a row it
// has something to do for (no terminal for an app, no delete for a setting).
// A switch's own button (its settings page) is not among them: it is all
// such a row has.
inline constexpr std::array<QStringView, 9> kKnown {
    u"admin", // 以管理员身份运行: a program's row
    u"reveal", // 打开所在位置
    u"terminal", // 在终端中打开
    u"copy", // 复制 (the file itself)
    u"copyPath", // 复制完整路径
    u"delete", // 删除, clicked twice
    u"preview", // 预览, as Alt+P
    u"copyName", // 复制名称
    u"favorite", // 加入收藏夹 / 从收藏夹移除
};

// As before there was a choice: the first six, in this order.
inline QStringList defaults()
{
    using namespace Qt::StringLiterals;
    return {u"admin"_s, u"reveal"_s, u"terminal"_s, u"copy"_s, u"copyPath"_s, u"delete"_s};
}

// The name as it is kept, whatever its case; empty: not a button.
inline QString known(QStringView name)
{
    const QStringView trimmed = name.trimmed();
    for (const QStringView id : kKnown) {
        if (id.compare(trimmed, Qt::CaseInsensitive) == 0)
            return id.toString();
    }
    return {};
}

// What a list written by hand comes to: the names known, each once, in the
// order given. Empty is a choice too: no buttons at all.
inline QStringList normalized(const QStringList& names)
{
    QStringList order;
    for (const QString& name : names) {
        if (const QString id = known(name); !id.isEmpty() && !order.contains(id))
            order.append(id);
    }
    return order;
}

} // namespace ws::rowactions
