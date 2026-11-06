#pragma once

#include "ClipboardDisplayProtocol.h"

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QJSValue>
#include <QLocalSocket>
#include <QQmlPropertyMap>
#include <QQuickImageProvider>
#include <QTimer>
#include <QTranslator>
#include <windows.h>

class QQmlEngine;
class QQuickWindow;

namespace ws {
class Placement;
class WindowFrame;

class DisplayRows : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(QColor highlightColor MEMBER m_highlight NOTIFY highlightColorChanged)
    Q_PROPERTY(int highlightWeight MEMBER m_highlightWeight NOTIFY highlightColorChanged)
public:
    explicit DisplayRows(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return m_roles; }
    int count() const { return int(m_rows.size()); }
    int selectedCount() const { return m_selected; }
    Q_INVOKABLE qint64 idAt(int row) const;
    Q_INVOKABLE bool isSelected(int row) const;
    bool begin(int count, int selected);
    bool append(int from, const QJsonArray& rows);
    bool commit();
signals:
    void countChanged();
    void selectionChanged();
    void highlightColorChanged();
private:
    QHash<int, QByteArray> m_roles;
    std::vector<QJsonObject> m_rows, m_pending;
    QColor m_highlight;
    int m_highlightWeight = 700;
    int m_selected = 0, m_pendingSelected = 0, m_expected = -1;
};

class DisplayImage : public QQuickImageResponse {
    Q_OBJECT
public:
    QQuickTextureFactory* textureFactory() const override;
    void finish(QImage image);
private:
    QImage m_image;
};

class DisplayReplica : public QQmlPropertyMap {
    Q_OBJECT
public:
    explicit DisplayReplica(QQmlEngine* engine);
    ~DisplayReplica() override;
    bool connectTo(const QString& pipe, DWORD parentPid);
    void setWindow(QQuickWindow* window, Placement* placement, WindowFrame* frame);
    void announce(bool uiAccess, DWORD band);
    Q_INVOKABLE QVariant invoke(const QString& name, const QVariantList& arguments);
    void requestImage(const QString& key, QSize size, DisplayImage* response);
    void send(const QJsonObject& object);

signals:
    void shown();
    void placed();
    void listChanged();
    void stepped(int row);
    void browseStepRequested();
    void queryChanged();
    void keysRoutedChanged();
    void contextMenuKeyPressed();
    void pinyinHintRequested();
    void groupNameRequested(int row, qint64 group);
    void pinnedChanged();
    void previewOpenChanged();
    void previewWidthChanged();
    void listWidthChanged();
    void windowHeightChanged();
    void pinnedFirstChanged();
    void categoriesChanged();
    void categoryChanged();
    void stateChanged();
    void statusChanged();
    void undoChanged();
    void holdChanged();
    void activeChanged();
    void numberKeysChanged();
    void rowOptionsChanged();
    void layoutOptionsChanged();
    void separatorChanged();
    void fieldChanged();

private:
    QVariant updateValue(const QString& key, const QVariant& input) override;
    void receive();
    void dispatch(const QJsonObject& object);
    void applyState(const QJsonObject& object);
    void deliverKey(const QJsonObject& object);
    void failed();

    QQmlEngine* m_engine;
    QLocalSocket m_socket;
    clipdisplay::Decoder m_decoder;
    DisplayRows m_rows;
    QPointer<QQuickWindow> m_window;
    Placement* m_placement = nullptr;
    WindowFrame* m_frame = nullptr;
    QHash<qint64, QVariant> m_replies;
    QHash<qint64, QPointer<DisplayImage>> m_images;
    QTimer m_watch;
    QTranslator m_translator;
    QString m_language;
    HANDLE m_parent = nullptr;
    DWORD m_parentPid = 0;
    qint64 m_serial = 0;
    bool m_applying = false;
    bool m_failed = false;
    bool m_fontReady = false;
};

} // namespace ws
