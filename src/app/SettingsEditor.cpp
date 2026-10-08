#include "SettingsEditor.h"

#include "Win32Util.h"
#include "platform/Shell.h"

#include <QDir>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

QString displayPath(const QString& path)
{
    return QString::fromStdWString(win32::expandEnvironment(QDir::toNativeSeparators(path.trimmed()).toStdWString()));
}

bool containsIgnoreCase(const QStringList& list, const QString& value)
{
    return list.contains(value, Qt::CaseInsensitive);
}

} // namespace

SettingsEditor::SettingsEditor(const Settings& settings, const QString& runningRenderer, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_runningRenderer(runningRenderer)
{
}

void SettingsEditor::commit()
{
    m_settings.save();
    emit changed();
    emit edited(m_settings);
}

void SettingsEditor::setSettings(const Settings& settings)
{
    if (settings == m_settings)
        return;
    m_settings = settings;
    emit changed();
}

void SettingsEditor::setDoubleCtrl(bool on)
{
    if (m_settings.doubleCtrl == on)
        return;
    m_settings.doubleCtrl = on;
    commit();
}

void SettingsEditor::setHotkey(const QString& hotkey)
{
    const QString value = hotkey.trimmed();
    if (m_settings.hotkey == value)
        return;
    m_settings.hotkey = value;
    commit();
}

void SettingsEditor::setHotkeyError(const QString& error)
{
    if (m_hotkeyError == error)
        return;
    m_hotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setRecordingHotkey(bool recording)
{
    if (m_recordingHotkey == recording)
        return;
    m_recordingHotkey = recording;
    emit recordingHotkeyChanged();
}

bool SettingsEditor::autostart() const
{
    return autostart::isEnabled();
}

void SettingsEditor::setAutostart(bool on)
{
    if (autostart::isEnabled() == on)
        return;
    autostart::setEnabled(on);
    emit autostartChanged();
}

void SettingsEditor::setIncludeRemovableDrives(bool on)
{
    if (m_settings.includeRemovableDrives == on)
        return;
    m_settings.includeRemovableDrives = on;
    commit();
}

void SettingsEditor::setRescanOnStartup(bool on)
{
    if (m_settings.rescanOnStartup == on)
        return;
    m_settings.rescanOnStartup = on;
    commit();
}

QStringList SettingsEditor::excludedPaths() const
{
    QStringList paths;
    for (const QString& p : m_settings.excludedPaths)
        paths.append(displayPath(p));
    return paths;
}

void SettingsEditor::setMaxContentFileSizeMB(int mb)
{
    mb = std::clamp(mb, 1, 4096);
    if (m_settings.maxContentFileSizeMB == mb)
        return;
    m_settings.maxContentFileSizeMB = mb;
    commit();
}

void SettingsEditor::setContentIndex(bool on)
{
    if (m_settings.contentIndex == on)
        return;
    m_settings.contentIndex = on;
    commit();
}

void SettingsEditor::setContentInLowPriority(bool on)
{
    if (m_settings.contentInLowPriority == on)
        return;
    m_settings.contentInLowPriority = on;
    commit();
}

void SettingsEditor::setContentIndexStatus(const QString& status)
{
    if (m_contentIndexStatus == status)
        return;
    m_contentIndexStatus = status;
    emit contentIndexStatusChanged();
}

void SettingsEditor::setRenderer(const QString& renderer)
{
    if (m_settings.renderer == renderer)
        return;
    m_settings.renderer = renderer;
    commit();
}

void SettingsEditor::setTheme(const QString& theme)
{
    if (m_settings.theme == theme)
        return;
    m_settings.theme = theme;
    commit();
}

void SettingsEditor::setLanguage(const QString& language)
{
    if (m_settings.language == language)
        return;
    m_settings.language = language;
    commit();
}

bool SettingsEditor::isDefault() const
{
    return m_settings == Settings::defaults();
}

void SettingsEditor::addExcludedFolder()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString folder = shell::pickFolder(owner, tr("选择不需要搜索的文件夹"));
    if (folder.isEmpty() || containsIgnoreCase(excludedPaths(), displayPath(folder)))
        return;
    m_settings.excludedPaths.append(folder);
    commit();
}

void SettingsEditor::removeExcludedPath(int index)
{
    if (index < 0 || index >= m_settings.excludedPaths.size())
        return;
    m_settings.excludedPaths.removeAt(index);
    commit();
}

bool SettingsEditor::addExcludedName(const QString& name)
{
    QString value = QDir::fromNativeSeparators(name.trimmed());
    while (value.endsWith(u'/'))
        value.chop(1);
    if (value.isEmpty() || containsIgnoreCase(m_settings.excludedNames, value))
        return false;
    m_settings.excludedNames.append(value);
    commit();
    return true;
}

void SettingsEditor::removeExcludedName(int index)
{
    if (index < 0 || index >= m_settings.excludedNames.size())
        return;
    m_settings.excludedNames.removeAt(index);
    commit();
}

bool SettingsEditor::addContentExtensions(const QString& text)
{
    static const QRegularExpression separators(u"[\\s,;，；、]+"_s);
    bool added = false;
    for (QString ext : text.split(separators, Qt::SkipEmptyParts)) {
        while (ext.startsWith(u'*') || ext.startsWith(u'.'))
            ext.remove(0, 1);
        ext = ext.toLower();
        if (ext.isEmpty() || containsIgnoreCase(m_settings.contentExtensions, ext))
            continue;
        m_settings.contentExtensions.append(ext);
        added = true;
    }
    if (added)
        commit();
    return added;
}

void SettingsEditor::removeContentExtension(int index)
{
    if (index < 0 || index >= m_settings.contentExtensions.size())
        return;
    m_settings.contentExtensions.removeAt(index);
    commit();
}

void SettingsEditor::restoreDefaults()
{
    if (isDefault())
        return;
    m_settings = Settings::defaults();
    commit();
}

void SettingsEditor::openDataFolder()
{
    shell::open(Settings::dataDir());
}

void SettingsEditor::openUrl(const QString& url)
{
    shell::openUrl(url);
}

void SettingsEditor::copyText(const QString& text)
{
    shell::copyText(text);
}

} // namespace ws
