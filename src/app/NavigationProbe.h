#pragma once

class QQmlEngine;
class QDir;
class QQuickWindow;

namespace ws { class SettingsEditor; }

namespace ws::preview {
bool checkPureSearch(QQmlEngine* engine, const QDir& output);
// Two-line results as drawn, in the box typed into a folder and in the
// launcher: a name's gap to its folder smaller than the gap to the next
// result, measured on the rendered ink (GitHub #245).
bool checkResultRowSpacing(QQmlEngine* engine, const QDir& output);
bool checkPluginReviewSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
// Production QML/controller checks on isolated data and non-activating,
// offscreen windows. No global key injection or clipboard writes.
bool checkNavigationWorkflows(QQmlEngine* engine, const QDir& output, bool flipped);
bool checkNavigationSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkOptimizationSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkClipboardAlignment(QQmlEngine* engine, const QDir& output, bool flipped);
bool checkSearchExperience(QQmlEngine* engine, const QDir& output);
bool checkSearchExperienceSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkLogoPositionSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
}
