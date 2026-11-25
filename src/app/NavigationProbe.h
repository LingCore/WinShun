#pragma once

class QQmlEngine;
class QDir;
class QQuickWindow;
class QString;

namespace ws { class SettingsEditor; }

namespace ws::preview {
bool checkPureSearch(QQmlEngine* engine, const QDir& output);
// Two-line results as drawn, in the box typed into a folder and in the
// launcher: a name's gap to its folder smaller than the gap to the next
// result, measured on the rendered ink (GitHub #245).
bool checkResultRowSpacing(QQmlEngine* engine, const QDir& output);
bool checkPluginReviewSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkPluginGallerySettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
// Production QML/controller checks on isolated data and non-activating,
// offscreen windows. No global key injection or clipboard writes.
bool checkNavigationWorkflows(QQmlEngine* engine, const QDir& output, bool flipped);
// The panel's 名称 filter on a believable folder of office files, for the
// release poster (--name-filter-poster); writes name-filter-poster.png.
bool checkNameFilterPoster(QQmlEngine* engine, const QDir& output);
bool checkNavigationSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkOptimizationSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkClipboardAlignment(QQmlEngine* engine, const QDir& output, bool flipped);
bool checkSearchExperience(QQmlEngine* engine, const QDir& output);
bool checkSearchExperienceSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
// A plugin's 使用说明 as it is read (--plugin-readme-reading [--readme <file>]):
// laid out by ReadingStyle, text under the scroll bar's drag wherever it goes,
// and every frame of a wheel's glide on the same device-pixel phase.
bool checkPluginReadmeReading(QQuickWindow* window, SettingsEditor* editor, const QDir& output, const QString& readme);
bool checkLogoPositionSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
bool checkDialogRuleSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output, const QString& fixture);
// The 捐赠 button moved to the 拾穗计划 page and back (GitHub #402).
bool checkDonatePlace(QQuickWindow* window, SettingsEditor* editor, const QDir& output);
}
