#pragma once

class QQmlEngine;
class QQuickWindow;
class QDir;
namespace ws { class SettingsEditor; class App; }

namespace ws::preview {
bool checkDoubleTapSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkDoubleTapPlugin(App& app, const QDir& output);
}
