#pragma once
class QQmlEngine;
class QDir;
namespace ws::preview {
bool checkPreviewInteractions(QQmlEngine* engine, const QDir& output);
}
