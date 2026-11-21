#pragma once
#include <QAbstractListModel>
#include <QUrl>

// Deterministic host/app boundaries for exercising the production QML shell.
// These fixtures never perform network, power, pairing, or streaming operations.
class ConsoleHostModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QVariantList onlineHosts READ onlineHosts CONSTANT)
public:
    using QAbstractListModel::QAbstractListModel;
    inline static int hostCount=2;
    bool secondHostWakeable = true;
    bool secondHostOnline = false;
    int wakeRequests = 0;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : hostCount; }
    QHash<int,QByteArray> roleNames() const override {
        return {{Qt::UserRole,"name"},{Qt::UserRole+1,"online"},{Qt::UserRole+2,"paired"},
            {Qt::UserRole+3,"busy"},{Qt::UserRole+4,"wakeable"},{Qt::UserRole+5,"statusUnknown"},
            {Qt::UserRole+6,"serverSupported"},{Qt::UserRole+7,"details"}};
    }
    QVariant data(const QModelIndex& index, int role) const override {
        return get(index.row()).value(QString::fromUtf8(roleNames().value(role)));
    }
    Q_INVOKABLE QVariantMap get(int row) const {
        if (row<0 || row>=hostCount) return {};
        return {{"name",row ? "Living Room PC" : "Gaming PC"},{"uuid",row ? "host-two" : "host-one"},
            {"online",row==0 || secondHostOnline},{"paired",true},{"busy",false},{"wakeable",row==0 || secondHostWakeable},
            {"statusUnknown",false},{"serverSupported",true},{"details","Test host"}};
    }
    Q_INVOKABLE void initialize(QObject*) {}
    Q_INVOKABLE QString uuidAt(int row) const { return get(row).value("uuid").toString(); }
    Q_INVOKABLE int indexOfUuid(const QString& uuid) const { return uuid=="host-one" ? 0 : uuid=="host-two" ? 1 : -1; }
    QVariantList onlineHosts() const { return {QVariantMap{{"name","Gaming PC"},{"uuid","host-one"}}}; }
    Q_INVOKABLE void wakeComputer(int) { ++wakeRequests; }
    Q_INVOKABLE void renameComputer(int,const QString&) {}
    Q_INVOKABLE void deleteComputer(int) {}
    Q_INVOKABLE void testConnectionForComputer(int) {}
    Q_INVOKABLE QString generatePinString() const { return "1234"; }
    Q_INVOKABLE void pairComputer(int,const QString&) {}
signals:
    void wakeRequestFinished(QString uuid, bool sent);
    void pairingCompleted(QVariant error);
    void connectionTestCompleted(int result, QString blockedPorts);
};

class ConsoleAppModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool includeHostActions MEMBER includeHostActions)
    Q_PROPERTY(bool appListLoading MEMBER appListLoading NOTIFY appListStateChanged)
    Q_PROPERTY(bool appListUnavailable MEMBER appListUnavailable NOTIFY appListStateChanged)
    Q_PROPERTY(bool hostSleepAvailable READ sleepAvailable CONSTANT)
public:
    using QAbstractListModel::QAbstractListModel;
    bool includeHostActions=false;
    bool appListLoading=false;
    bool appListUnavailable=false;
    QList<int> ids = {1, 2, 3, 4, 5};
    QSet<int> hiddenIds;
    int runningId = 0;
    int retries = 0;
    bool sleepAvailable() const { return false; }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : ids.size(); }
    QHash<int,QByteArray> roleNames() const override {
        return {{Qt::UserRole,"name"},{Qt::UserRole+1,"appid"},{Qt::UserRole+2,"boxart"},
            {Qt::UserRole+3,"running"},{Qt::UserRole+4,"hidden"},{Qt::UserRole+5,"directLaunch"}};
    }
    QVariant data(const QModelIndex& index, int role) const override {
        return get(index.row()).value(QString::fromUtf8(roleNames().value(role)));
    }
    Q_INVOKABLE QVariantMap get(int row) const {
        if (row<0 || row>=ids.size()) return {};
        const int game = ids[row] - 1;
        const QStringList names={"Forza Horizon 5","Hollow Knight","No Man’s Sky","Control","Desktop"};
        const QStringList art={"fixture-racing.svg","fixture-hollow.svg","fixture-space.svg","fixture-control.svg","no_app_image.png"};
        return {{"name",names.value(game, QStringLiteral("Game %1").arg(ids[row]))},{"appid",ids[row]},{"boxart",QUrl("qrc:/res/"+art.value(game, QStringLiteral("no_app_image.png")))},
            {"running",ids[row] == runningId},{"hidden",hiddenIds.contains(ids[row])},{"directLaunch",false}};
    }
    Q_INVOKABLE void initialize(QObject*,int,bool) {}
    Q_INVOKABLE int indexOfApp(int id) const { return ids.indexOf(id); }
    Q_INVOKABLE int getDirectLaunchAppIndex() const { return -1; }
    Q_INVOKABLE int getRunningAppId() const { return runningId; }
    Q_INVOKABLE QString getRunningAppName() const { return get(indexOfApp(runningId)).value("name").toString(); }
    Q_INVOKABLE QObject* createSessionForApp(int) { return nullptr; }
    Q_INVOKABLE void requestHostSleep() {}
    Q_INVOKABLE void retryAppList() {
        ++retries;
        appListUnavailable = false;
        appListLoading = true;
        emit appListStateChanged();
    }
    Q_INVOKABLE void setAppHidden(int row,bool hidden) {
        if (row < 0 || row >= ids.size()) return;
        if (hidden) hiddenIds.insert(ids[row]); else hiddenIds.remove(ids[row]);
        emit dataChanged(index(row), index(row), {Qt::UserRole+4});
    }
    void removeApp(int id) {
        const int row = ids.indexOf(id);
        if (row < 0) return;
        beginRemoveRows({}, row, row); ids.removeAt(row); endRemoveRows();
    }
    void insertApp(int id) {
        beginInsertRows({}, 0, 0); ids.prepend(id); endInsertRows();
    }
    void refreshArtwork(int id) {
        const int row = ids.indexOf(id);
        emit dataChanged(index(row), index(row), {Qt::UserRole+2});
    }
    Q_INVOKABLE void setAppDirectLaunch(int,bool) {}
    Q_INVOKABLE void quitRunningApp() {}
signals:
    void appListStateChanged();
    void computerLost();
    void hostSleepCompleted(QString error);
};
