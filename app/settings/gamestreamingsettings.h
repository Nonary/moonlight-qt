#pragma once

#include "streamingpreferences.h"
#include <QSet>

class GameStreamingSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(StreamingPreferences* preferences READ preferences CONSTANT)
    Q_PROPERTY(QVariantList customSettings READ customSettings NOTIFY draftChanged)

public:
    GameStreamingSettings(QString profileId, QString hostUuid, int appId, QObject* parent = nullptr);
    StreamingPreferences* preferences() const { return m_Draft.get(); }
    QVariantList customSettings() const;
    Q_INVOKABLE bool save();
    Q_INVOKABLE void reset(const QString& key = QString());
    Q_INVOKABLE void dispose() { deleteLater(); }
    void invalidate() { m_Valid = false; }

    static QVariantMap load(const QString& profileId, const QString& hostUuid, int appId);
    static bool remove(const QString& profileId, const QString& hostUuid, int appId);
    static void removeHost(const QString& profileId, const QString& hostUuid);
    static std::unique_ptr<StreamingPreferences> resolve(const StreamingPreferences& base,
        const QString& profileId, const QString& hostUuid, int appId);

signals:
    void saved();
    void draftChanged();

private slots:
    void observeChanges();

private:
    QVariantMap pendingOverrides() const;
    void refreshDraft();
    static void apply(StreamingPreferences& preferences, const QVariantMap& overrides);
    static QString settingsGroup(const QString& profileId, const QString& hostUuid, int appId);

    QString m_ProfileId, m_HostUuid;
    int m_AppId;
    bool m_Valid;
    std::unique_ptr<StreamingPreferences> m_Base, m_Draft;
    QVariantMap m_Overrides, m_SavedOverrides, m_Baseline;
    QVariantMap m_LastObserved;
    QSet<QString> m_EditedFields;
};
