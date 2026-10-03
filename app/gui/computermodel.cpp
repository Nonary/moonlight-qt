#include "computermodel.h"
#include "backend/profilemanager.h"

#include <QThreadPool>

ComputerModel::ComputerModel(QObject* object)
    : QAbstractListModel(object) {}

void ComputerModel::initialize(ComputerManager* computerManager)
{
    if (!computerManager) return;
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &ComputerModel::handleComputerStateChanged);
    connect(m_ComputerManager, &ComputerManager::pairingCompleted,
            this, &ComputerModel::handlePairingCompleted);
    connect(m_ComputerManager, &ComputerManager::hostRemoved, this, [this](const QString& uuid) {
        for (int row = 0; row < m_Computers.count(); ++row) {
            if (uuidAt(row) == uuid) {
                beginRemoveRows(QModelIndex(), row, row);
                m_Computers.removeAt(row);
                endRemoveRows();
                return;
            }
        }
    });
    connect(ProfileManager::get(), &ProfileManager::activeProfileAboutToChange,
            this, [this]() {
        beginResetModel();
        m_Computers.clear();
        endResetModel();
    });

    beginResetModel();
    m_Computers = m_ComputerManager->getComputers();
    endResetModel();
}

QVariant ComputerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_Computers.count()) {
        return QVariant();
    }

    NvComputer* computer = m_Computers[index.row()];
    QReadLocker lock(&computer->lock);

    switch (role) {
    case NameRole:
        return computer->name;
    case OnlineRole:
        return computer->state == NvComputer::CS_ONLINE;
    case PairedRole:
        return computer->pairState == NvComputer::PS_PAIRED;
    case BusyRole:
        return computer->currentGameId != 0;
    case WakeableRole:
        return !computer->macAddress.isEmpty();
    case StatusUnknownRole:
        return computer->state == NvComputer::CS_UNKNOWN;
    case ServerSupportedRole:
        return computer->isSupportedServerVersion;
    case DetailsRole: {
        QString state, pairState;

        switch (computer->state) {
        case NvComputer::CS_ONLINE:
            state = tr("Online");
            break;
        case NvComputer::CS_OFFLINE:
            state = tr("Offline");
            break;
        default:
            state = tr("Unknown");
            break;
        }

        switch (computer->pairState) {
        case NvComputer::PS_PAIRED:
            pairState = tr("Paired");
            break;
        case NvComputer::PS_NOT_PAIRED:
            pairState = tr("Unpaired");
            break;
        default:
            pairState = tr("Unknown");
            break;
        }

        return tr("Name: %1").arg(computer->name) + '\n' +
               tr("Status: %1").arg(state) + '\n' +
               tr("Active Address: %1").arg(computer->activeAddress.toString()) + '\n' +
               tr("UUID: %1").arg(computer->uuid) + '\n' +
               tr("Local Address: %1").arg(computer->localAddress.toString()) + '\n' +
               tr("Remote Address: %1").arg(computer->remoteAddress.toString()) + '\n' +
               tr("IPv6 Address: %1").arg(computer->ipv6Address.toString()) + '\n' +
               tr("Manual Address: %1").arg(computer->manualAddress.toString()) + '\n' +
               tr("MAC Address: %1").arg(computer->macAddress.isEmpty() ? tr("Unknown") : QString(computer->macAddress.toHex(':'))) + '\n' +
               tr("Pair State: %1").arg(pairState) + '\n' +
               tr("Running Game ID: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->currentGameId) : tr("Unknown")) + '\n' +
               tr("HTTPS Port: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->activeHttpsPort) : tr("Unknown"));
    }
    default:
        return QVariant();
    }
}

int ComputerModel::rowCount(const QModelIndex& parent) const
{
    // We should not return a count for valid index values,
    // only the parent (which will not have a "valid" index).
    if (parent.isValid()) {
        return 0;
    }

    return m_Computers.count();
}

QString ComputerModel::uuidAt(int computerIndex) const
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) return {};
    QReadLocker lock(&m_Computers[computerIndex]->lock);
    return m_Computers[computerIndex]->uuid;
}

QHash<int, QByteArray> ComputerModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[OnlineRole] = "online";
    names[PairedRole] = "paired";
    names[BusyRole] = "busy";
    names[WakeableRole] = "wakeable";
    names[StatusUnknownRole] = "statusUnknown";
    names[ServerSupportedRole] = "serverSupported";
    names[DetailsRole] = "details";

    return names;
}

Session* ComputerModel::createSessionForCurrentGame(int computerIndex)
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) return nullptr;

    NvComputer* computer = m_Computers[computerIndex];

    for (NvApp& app : computer->appList) {
        if (app.id == computer->currentGameId) {
            return new Session(computer, app);
        }
    }

    return nullptr;
}

void ComputerModel::deleteComputer(int computerIndex)
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return;
    }

    NvComputer* computer = m_Computers[computerIndex];
    beginRemoveRows(QModelIndex(), computerIndex, computerIndex);
    m_Computers.removeAt(computerIndex);
    endRemoveRows();

    // Other models also remove this host when deleteHost emits hostRemoved.
    m_ComputerManager->deleteHost(computer);
}

class DeferredWakeHostTask : public QRunnable
{
public:
    DeferredWakeHostTask(NvComputer* computer)
    {
        QReadLocker lock(&computer->lock);
        m_Computer = *computer;
    }

    void run()
    {
        m_Computer.wake();
    }

private:
    NvComputer m_Computer;
};

void ComputerModel::wakeComputer(int computerIndex)
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return;
    }

    DeferredWakeHostTask* wakeTask = new DeferredWakeHostTask(m_Computers[computerIndex]);
    QThreadPool::globalInstance()->start(wakeTask);
}

void ComputerModel::renameComputer(int computerIndex, QString name)
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return;
    }

    m_ComputerManager->renameHost(m_Computers[computerIndex], name);
}

QString ComputerModel::generatePinString()
{
    return m_ComputerManager->generatePinString();
}

class DeferredTestConnectionTask : public QObject, public QRunnable
{
    Q_OBJECT
public:
    void run()
    {
        unsigned int portTestResult = LiTestClientConnectivity("qt.conntest.moonlight-stream.org", 443, ML_PORT_FLAG_ALL);
        if (portTestResult == ML_TEST_RESULT_INCONCLUSIVE) {
            emit connectionTestCompleted(-1, QString());
        }
        else {
            char blockedPorts[512];
            LiStringifyPortFlags(portTestResult, "\n", blockedPorts, sizeof(blockedPorts));
            emit connectionTestCompleted(portTestResult, QString(blockedPorts));
        }
    }

signals:
    void connectionTestCompleted(int result, QString blockedPorts);
};

void ComputerModel::testConnectionForComputer(int)
{
    DeferredTestConnectionTask* testConnectionTask = new DeferredTestConnectionTask();
    QObject::connect(testConnectionTask, &DeferredTestConnectionTask::connectionTestCompleted,
                     this, &ComputerModel::connectionTestCompleted);
    QThreadPool::globalInstance()->start(testConnectionTask);
}

void ComputerModel::pairComputer(int computerIndex, QString pin)
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return;
    }

    m_ComputerManager->pairHost(m_Computers[computerIndex], pin);
}

void ComputerModel::handlePairingCompleted(NvComputer*, QString error)
{
    emit pairingCompleted(error.isEmpty() ? QVariant() : error);
}

void ComputerModel::handleComputerStateChanged(NvComputer* computer)
{
    QVector<NvComputer*> newComputerList = m_ComputerManager->getComputers();

    // Reset the model if the structural layout of the list has changed
    if (m_Computers != newComputerList) {
        beginResetModel();
        m_Computers = newComputerList;
        endResetModel();
    }
    else {
        // Let the view know that this specific computer changed
        int index = m_Computers.indexOf(computer);
        if (index >= 0) {
            emit dataChanged(createIndex(index, 0), createIndex(index, 0));
        }
    }
}

#include "computermodel.moc"
