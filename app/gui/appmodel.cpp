#include "appmodel.h"
#include "backend/profilemanager.h"

AppModel::AppModel(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(&m_BoxArtManager, &BoxArtManager::boxArtLoadComplete,
            this, &AppModel::handleBoxArtLoaded);
}

bool AppModel::frameLimiterSupported() const { return m_FrameLimiterSupported; }
bool AppModel::frameLimiterEnabled() const { return m_FrameLimiterEnabled; }
bool AppModel::virtualDisplayFrameLimiterEnabled() const { return m_VirtualDisplayFrameLimiterEnabled; }
double AppModel::frameLimiterFpsLimit() const { return m_FrameLimiterFpsLimit; }

void AppModel::updateFrameLimiterCapabilities()
{
    if (!m_Computer) return;
    // Snapshot discovery state; QML getters use only this GUI-thread cache.
    QReadLocker locker(&m_Computer->lock);
    const bool changed = m_FrameLimiterSupported != m_Computer->frameLimiterSupported ||
        m_FrameLimiterEnabled != m_Computer->frameLimiterEnabled ||
        m_VirtualDisplayFrameLimiterEnabled != m_Computer->virtualDisplayFrameLimiterEnabled ||
        m_FrameLimiterFpsLimit != m_Computer->frameLimiterFpsLimitMilliHz / 1000.0;
    m_FrameLimiterSupported = m_Computer->frameLimiterSupported;
    m_FrameLimiterEnabled = m_Computer->frameLimiterEnabled;
    m_VirtualDisplayFrameLimiterEnabled = m_Computer->virtualDisplayFrameLimiterEnabled;
    m_FrameLimiterFpsLimit = m_Computer->frameLimiterFpsLimitMilliHz / 1000.0;
    locker.unlock();
    if (changed) emit frameLimiterChanged();
}

void AppModel::initialize(ComputerManager* computerManager, int computerIndex, bool showHiddenGames)
{
    if (!computerManager) {
        return;
    }
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &AppModel::handleComputerStateChanged);
    connect(m_ComputerManager, &ComputerManager::hostRemoved,
            this, &AppModel::handleHostRemoved);
    connect(ProfileManager::get(), &ProfileManager::activeProfileAboutToChange,
            this, &AppModel::invalidateComputer);

    const auto computers = m_ComputerManager->getComputers();
    if (computerIndex < 0 || computerIndex >= computers.count()) {
        return;
    }
    m_Computer = computers.at(computerIndex);
    m_CurrentGameId = m_Computer->currentGameId;
    m_ShowHiddenGames = showHiddenGames;

    updateAppList(m_Computer->appList);
    updateFrameLimiterCapabilities();
}

int AppModel::getRunningAppId()
{
    return m_CurrentGameId;
}

QString AppModel::getRunningAppName()
{
    if (m_CurrentGameId != 0) {
        for (int i = 0; i < m_AllApps.count(); i++) {
            if (m_AllApps[i].id == m_CurrentGameId) {
                return m_AllApps[i].name;
            }
        }
    }

    return nullptr;
}

Session* AppModel::createSessionForApp(int appIndex)
{
    if (!m_Computer || appIndex < 0 || appIndex >= m_VisibleApps.count()) return nullptr;
    NvApp app = m_VisibleApps.at(appIndex);

    auto preferences = GameStreamingSettings::resolve(*StreamingPreferences::get(),
        m_ComputerManager->profileId(), m_Computer->uuid, app.id);
    return new Session(m_Computer, app, preferences.get());
}

int AppModel::indexOfApp(int appId) const
{
    for (int i = 0; i < m_VisibleApps.count(); ++i) {
        if (m_VisibleApps[i].id == appId) return i;
    }
    return -1;
}

GameStreamingSettings* AppModel::createGameSettings(int appId)
{
    if (!m_Computer || indexOfApp(appId) < 0) return nullptr;
    auto editor = new GameStreamingSettings(m_ComputerManager->profileId(), m_Computer->uuid, appId, this);
    const auto hostUuid = m_Computer->uuid;
    connect(m_ComputerManager, &ComputerManager::hostRemoved, editor, [editor, hostUuid](const QString& uuid) {
        if (uuid == hostUuid) editor->invalidate();
    });
    connect(editor, &GameStreamingSettings::saved, this, [this, appId]() {
        const int row = indexOfApp(appId);
        if (row >= 0) emit dataChanged(index(row), index(row), {CustomStreamingSettingsRole});
    });
    return editor;
}

bool AppModel::removeGameSettings(int appId)
{
    const int row = indexOfApp(appId);
    if (!m_Computer || row < 0) return false;
    const bool removed = GameStreamingSettings::remove(m_ComputerManager->profileId(), m_Computer->uuid, appId);
    if (removed) emit dataChanged(index(row), index(row), {CustomStreamingSettingsRole});
    return removed;
}

int AppModel::getDirectLaunchAppIndex()
{
    for (int i = 0; i < m_VisibleApps.count(); i++) {
        if (m_VisibleApps[i].directLaunch) {
            return i;
        }
    }

    return -1;
}

int AppModel::rowCount(const QModelIndex &parent) const
{
    // For list models only the root node (an invalid parent) should return the list's size. For all
    // other (valid) parents, rowCount() should return 0 so that it does not become a tree model.
    if (parent.isValid())
        return 0;

    return m_VisibleApps.count();
}

QVariant AppModel::data(const QModelIndex &index, int role) const
{
    if (!m_Computer || !index.isValid() || index.row() < 0 || index.row() >= m_VisibleApps.count())
        return QVariant();
    NvApp app = m_VisibleApps.at(index.row());

    switch (role)
    {
    case NameRole:
        return app.name;
    case RunningRole:
        return m_Computer->currentGameId == app.id;
    case BoxArtRole:
        // FIXME: const-correctness
        return const_cast<BoxArtManager&>(m_BoxArtManager).loadBoxArt(m_Computer, app);
    case HiddenRole:
        return app.hidden;
    case AppIdRole:
        return app.id;
    case DirectLaunchRole:
        return app.directLaunch;
    case AppCollectorGameRole:
        return app.isAppCollectorGame;
    case CustomStreamingSettingsRole:
        return !GameStreamingSettings::load(m_ComputerManager->profileId(), m_Computer->uuid, app.id).isEmpty();
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> AppModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[RunningRole] = "running";
    names[BoxArtRole] = "boxart";
    names[HiddenRole] = "hidden";
    names[AppIdRole] = "appid";
    names[DirectLaunchRole] = "directLaunch";
    names[AppCollectorGameRole] = "appCollectorGame";
    names[CustomStreamingSettingsRole] = "customStreamingSettings";

    return names;
}

void AppModel::quitRunningApp()
{
    if (m_Computer) {
        m_ComputerManager->quitRunningApp(m_Computer);
    }
}

bool AppModel::isAppCurrentlyVisible(const NvApp& app)
{
    for (const NvApp& visibleApp : std::as_const(m_VisibleApps)) {
        if (app.id == visibleApp.id) {
            return true;
        }
    }

    return false;
}

QVector<NvApp> AppModel::getVisibleApps(const QVector<NvApp>& appList)
{
    QVector<NvApp> visibleApps;

    for (const NvApp& app : appList) {
        // Don't immediately hide games that were previously visible. This
        // allows users to easily uncheck the "Hide App" checkbox if they
        // check it by mistake.
        if (m_ShowHiddenGames || !app.hidden || isAppCurrentlyVisible(app)) {
            visibleApps.append(app);
        }
    }

    return visibleApps;
}

void AppModel::updateAppList(QVector<NvApp> newList)
{
    m_AllApps = newList;

    QVector<NvApp> newVisibleList = getVisibleApps(newList);

    // Process removals and updates first
    for (int i = 0; i < m_VisibleApps.count(); i++) {
        const NvApp& existingApp = m_VisibleApps.at(i);

        bool found = false;
        for (const NvApp& newApp : std::as_const(newVisibleList)) {
            if (existingApp.id == newApp.id) {
                // If the data changed, update it in our list
                if (existingApp != newApp) {
                    m_VisibleApps.replace(i, newApp);
                    emit dataChanged(createIndex(i, 0), createIndex(i, 0));
                }

                found = true;
                break;
            }
        }

        if (!found) {
            beginRemoveRows(QModelIndex(), i, i);
            m_VisibleApps.removeAt(i);
            endRemoveRows();
            i--;
        }
    }

    // Process additions now
    for (const NvApp& newApp : std::as_const(newVisibleList)) {
        int insertionIndex = m_VisibleApps.size();
        bool found = false;

        for (int i = 0; i < m_VisibleApps.count(); i++) {
            const NvApp& existingApp = m_VisibleApps.at(i);

            if (existingApp.id == newApp.id) {
                found = true;
                break;
            }
            else if (existingApp.name.toLower() > newApp.name.toLower()) {
                insertionIndex = i;
                break;
            }
        }

        if (!found) {
            beginInsertRows(QModelIndex(), insertionIndex, insertionIndex);
            m_VisibleApps.insert(insertionIndex, newApp);
            endInsertRows();
        }
    }

    Q_ASSERT(newVisibleList == m_VisibleApps);
}

void AppModel::setAppHidden(int appIndex, bool hidden)
{
    if (!m_Computer || appIndex < 0 || appIndex >= m_VisibleApps.count()) {
        return;
    }
    int appId = m_VisibleApps.at(appIndex).id;

    {
        QWriteLocker lock(&m_Computer->lock);

        for (NvApp& app : m_Computer->appList) {
            if (app.id == appId) {
                app.hidden = hidden;
                break;
            }
        }
    }

    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
}

void AppModel::setAppDirectLaunch(int appIndex, bool directLaunch)
{
    if (!m_Computer || appIndex < 0 || appIndex >= m_VisibleApps.count()) {
        return;
    }
    int appId = m_VisibleApps.at(appIndex).id;

    {
        QWriteLocker lock(&m_Computer->lock);

        for (NvApp& app : m_Computer->appList) {
            if (directLaunch) {
                // We must clear direct launch from all other apps
                // to set it on the new app.
                app.directLaunch = app.id == appId;
            }
            else if (app.id == appId) {
                // If we're clearing direct launch, we're done once we
                // find our matching app ID.
                app.directLaunch = false;
                break;
            }
        }
    }

    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
}

void AppModel::handleHostRemoved(const QString& uuid)
{
    if (!m_Computer || m_Computer->uuid != uuid) {
        return;
    }
    invalidateComputer();
}

void AppModel::invalidateComputer()
{
    if (!m_Computer) {
        return;
    }

    // A popped QML page can outlive its host until deferred destruction.
    m_BoxArtManager.stop();
    beginResetModel();
    m_Computer = nullptr;
    m_CurrentGameId = 0;
    m_VisibleApps.clear();
    m_AllApps.clear();
    m_FrameLimiterSupported = false;
    m_FrameLimiterEnabled = false;
    m_VirtualDisplayFrameLimiterEnabled = false;
    m_FrameLimiterFpsLimit = 0;
    endResetModel();
    emit frameLimiterChanged();
    emit computerLost();
}

void AppModel::handleComputerStateChanged(NvComputer* computer)
{
    // Ignore updates for computers that aren't ours
    if (!m_Computer || computer != m_Computer) {
        return;
    }

    updateFrameLimiterCapabilities();

    // If the computer has gone offline or we've been unpaired,
    // signal the UI so we can go back to the PC view.
    if (m_Computer->state == NvComputer::CS_OFFLINE ||
            m_Computer->pairState == NvComputer::PS_NOT_PAIRED) {
        emit computerLost();
        return;
    }

    // First, process additions/removals from the app list. This
    // is required because the new game may now be running, so
    // we can't check that first.
    if (computer->appList != m_AllApps) {
        updateAppList(computer->appList);
    }

    // Finally, process changes to the active app
    if (computer->currentGameId != m_CurrentGameId) {
        // First, invalidate the running state of newly running game
        for (int i = 0; i < m_VisibleApps.count(); i++) {
            if (m_VisibleApps[i].id == computer->currentGameId) {
                emit dataChanged(createIndex(i, 0),
                                 createIndex(i, 0),
                                 QVector<int>() << RunningRole);
                break;
            }
        }

        // Next, invalidate the running state of the old game (if it exists)
        if (m_CurrentGameId != 0) {
            for (int i = 0; i < m_VisibleApps.count(); i++) {
                if (m_VisibleApps[i].id == m_CurrentGameId) {
                    emit dataChanged(createIndex(i, 0),
                                     createIndex(i, 0),
                                     QVector<int>() << RunningRole);
                    break;
                }
            }
        }

        // Now update our internal state
        m_CurrentGameId = m_Computer->currentGameId;
    }
}

void AppModel::handleBoxArtLoaded(NvComputer* computer, NvApp app, QUrl /* image */)
{
    if (!m_Computer || computer != m_Computer) {
        return;
    }

    int index = m_VisibleApps.indexOf(app);

    // Make sure we're not delivering a callback to an app that's already been removed
    if (index >= 0) {
        // Let our view know the box art data has changed for this app
        emit dataChanged(createIndex(index, 0),
                         createIndex(index, 0),
                         QVector<int>() << BoxArtRole);
    }
    else {
        qWarning() << "App not found for box art callback:" << app.name;
    }
}
