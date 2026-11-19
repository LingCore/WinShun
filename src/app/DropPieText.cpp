#include "DropPieText.h"

#include "PieGeometry.h"
#include "PieLabels.h"
#include "SystemTheme.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QList>

#include <utility>

namespace ws {

namespace {

// GUI thread only.
int g_folderCount = 0;
QList<DropPieText*> g_instances;

// About eight Chinese characters at 界面大小 standard.
constexpr double kNameWidth = 104;

} // namespace

DropPieText::DropPieText(QObject* parent)
    : QObject(parent)
{
    g_instances.append(this);
}

DropPieText::~DropPieText()
{
    g_instances.removeOne(this);
}

int DropPieText::folderCount() const
{
    return g_folderCount;
}

int DropPieText::pageSize() const noexcept
{
    return pie::kPageSize;
}

void DropPieText::setFolderCount(int count)
{
    if (g_folderCount == count)
        return;
    g_folderCount = count;
    for (DropPieText* text : std::as_const(g_instances))
        emit text->folderCountChanged();
}

int DropPieText::uiStep()
{
    const QString size = SystemTheme::uiSize();
    return size == u"dense" || size == u"compact" ? 0 : size == u"roomy" ? 2 : 1;
}

QFont DropPieText::labelFont()
{
    QFont font = QGuiApplication::font(); // what a QML Text uses unless told otherwise
    font.setPixelSize(pie::labelPixelSize(uiStep()));
    return font;
}

double DropPieText::nameWidth()
{
    return kNameWidth * pie::scaleForStep(uiStep());
}

QString DropPieText::label(const QString& name) const
{
    return pielabels::elideMiddle(name.simplified(), QFontMetricsF(labelFont()), nameWidth());
}

} // namespace ws
