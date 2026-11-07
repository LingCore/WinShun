#pragma once

class QQmlEngine;
class QDir;

namespace ws::preview {
bool checkForegroundMemory(QQmlEngine* engine, const QDir& output, bool requireLazy, bool preload = false);
}
