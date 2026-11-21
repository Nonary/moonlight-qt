#pragma once

#include "backend/boxartmanager.h"
#include "backend/computermanager.h"
#include "streaming/session.h"

#include <QAbstractListModel>

class AppModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool appListLoading READ appListLoading NOTIFY appListStateChanged)
    Q_PROPERTY(bool appListUnavailable READ appListUnavailable NOTIFY appListStateChanged)
    Q_PROPERTY(bool includeHostActions READ includeHostActions WRITE setIncludeHostActions)
    Q_PROPERTY(bool hostSleepAvailable READ hostSleepAvailable NOTIFY hostSleepAvailableChanged)
    Q_PROPERTY(bool frameLimiterSupported READ frameLimiterSupported NOTIFY frameLimiterChanged)
    Q_PROPERTY(bool frameLimiterEnabled READ frameLimiterEnabled NOTIFY frameLimiterChanged)
    Q_PROPERTY(bool virtualDisplayFrameLimiterEnabled READ virtualDisplayFrameLimiterEnabled NOTIFY frameLimiterChanged)
    Q_PROPERTY(double frameLimiterFpsLimit READ frameLimiterFpsLimit NOTIFY frameLimiterChanged)

    enum Roles
    {
        NameRole = Qt::UserRole,
        RunningRole,
        BoxArtRole,
        HiddenRole,
        AppIdRole,
        DirectLaunchRole,
        AppCollectorGameRole,
        SleepActionRole,
    };

public:
    explicit AppModel(QObject *parent = nullptr);
    bool appListLoading() const { return m_AppListLoading; }
    bool appListUnavailable() const { return m_AppListUnavailable; }
    bool includeHostActions() const { return m_IncludeHostActions; }
    void setIncludeHostActions(bool include);
    bool hostSleepAvailable() const { return m_HostSleepAvailable; }
    Q_INVOKABLE QVariantMap get(int appIndex) const;
    Q_INVOKABLE int indexOfApp(int appId) const;
    bool frameLimiterSupported() const;
    bool frameLimiterEnabled() const;
    bool virtualDisplayFrameLimiterEnabled() const;
    double frameLimiterFpsLimit() const;

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager, int computerIndex, bool showHiddenGames);

    Q_INVOKABLE Session* createSessionForApp(int appIndex);

    Q_INVOKABLE int getDirectLaunchAppIndex();

    Q_INVOKABLE int getRunningAppId();

    Q_INVOKABLE QString getRunningAppName();

    Q_INVOKABLE void quitRunningApp();
    Q_INVOKABLE void retryAppList();

    Q_INVOKABLE void requestHostSleep();

    Q_INVOKABLE void setAppHidden(int appIndex, bool hidden);

    Q_INVOKABLE void setAppDirectLaunch(int appIndex, bool directLaunch);

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handleBoxArtLoaded(NvComputer* computer, NvApp app, QUrl image);

signals:
    void computerLost();
    void appListStateChanged();
    void hostSleepAvailableChanged();
    void frameLimiterChanged();
    void hostSleepCompleted(QString error);

private:
    void updateAppList(QVector<NvApp> newList);

    QVector<NvApp> getVisibleApps(const QVector<NvApp>& appList);

    bool isAppCurrentlyVisible(const NvApp& app);

    void updateSleepAction();
    void updateAppListState();
    bool m_AppListLoading = true;
    bool m_AppListUnavailable = false;
    bool m_SleepActionVisible = false;
    bool m_IncludeHostActions = true;
    bool m_HostSleepAvailable = false;
    void updateFrameLimiterCapabilities();
    bool m_FrameLimiterSupported = false;
    bool m_FrameLimiterEnabled = false;
    bool m_VirtualDisplayFrameLimiterEnabled = false;
    double m_FrameLimiterFpsLimit = 0;
    NvComputer* m_Computer = nullptr;
    BoxArtManager m_BoxArtManager;
    ComputerManager* m_ComputerManager;
    QVector<NvApp> m_VisibleApps, m_AllApps;
    int m_CurrentGameId;
    bool m_ShowHiddenGames;
};
