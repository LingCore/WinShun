#pragma once

class QQmlEngine;
class QDir;
class QQuickWindow;

namespace ws { class SettingsEditor; }

namespace ws::preview {
// Production QML/controller checks on isolated data and non-activating,
// offscreen windows. No global key injection or clipboard writes.
bool checkNavigationWorkflows(QQmlEngine* engine, const QDir& output, bool flipped);
bool checkNavigationSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkOptimizationSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkClipboardAlignment(QQmlEngine* engine, const QDir& output, bool flipped);
bool checkSearchExperience(QQmlEngine* engine, const QDir& output);
bool checkSearchExperienceSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
}
