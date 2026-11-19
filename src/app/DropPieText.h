#pragma once

#include <QFont>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace ws {

// For the settings (收藏夹): how the drop pie will show a folder's name, by
// the same fitter and font as the pie itself (PieLabels), and how many
// folders it has to choose from, eight to a page. One per QML engine.
class DropPieText : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int folderCount READ folderCount NOTIFY folderCountChanged FINAL)
    Q_PROPERTY(int pageSize READ pageSize CONSTANT FINAL) // folders to a page of the pie

public:
    explicit DropPieText(QObject* parent = nullptr);
    ~DropPieText() override;

    int folderCount() const;
    int pageSize() const noexcept;

    // The name as its chip shows it, on one line: shortened in its middle
    // ("…", the end kept) when it is too long.
    Q_INVOKABLE QString label(const QString& name) const;

    // From DropPie: the folder favorites (and places that are folders) it
    // could show; every instance is told.
    static void setFolderCount(int count);

    // What the chips' names are drawn in (the application's font, at the
    // size for 界面大小), and how wide one may be.
    static QFont labelFont();
    static double nameWidth();
    static int uiStep(); // 界面大小: 0 compact, 1 standard, 2 roomy (Theme.step)

signals:
    void folderCountChanged();
};

} // namespace ws
