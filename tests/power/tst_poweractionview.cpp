#include <QtTest>
#include <QFontDatabase>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QAbstractListModel>
#include <QDir>
#include "../../app/gui/recentappsmodel.h"

// The real launcher is exercised against an isolated backend. A session request
// is counted and returns null, so none of these tests contacts a host.
class ViewModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool includeHostActions MEMBER includeHostActions)
    Q_PROPERTY(bool appListLoading READ appListLoading CONSTANT)
    Q_PROPERTY(bool appListUnavailable READ appListUnavailable CONSTANT)
    Q_PROPERTY(bool hostSleepAvailable READ hostSleepAvailable CONSTANT)
public:
    enum { Name=Qt::UserRole, Running, Art, Hidden, AppId, Direct, Collector, Sleep };
    static ViewModel* instance;
    static bool withGames;
    bool includeHostActions = true;
    QList<int> ids;
    int requests=0, sessions=0, preferenceWrites=0, requestedId=0;
    explicit ViewModel(QObject* p=nullptr) : QAbstractListModel(p) { instance=this; if (withGames) ids={101,202,303}; }
    bool hostSleepAvailable() const { return true; }
    bool appListLoading() const { return false; }
    bool appListUnavailable() const { return false; }
    int rowCount(const QModelIndex& p = {}) const override { return p.isValid()?0:ids.size(); }
    QVariant data(const QModelIndex& idx,int role) const override {
        if (!idx.isValid() || idx.row()<0 || idx.row()>=ids.size()) return {};
        const int id=ids[idx.row()];
        if(role==Name) return id==101 ? QString("Forza Horizon 5") : id==202 ? QString("Hollow Knight") : QString("Desktop");
        if(role==Art) return QUrl(id==101 ? "qrc:/res/fixture-racing.svg" : id==202 ? "qrc:/res/fixture-hollow.svg" : "qrc:/res/no_app_image.png");
        if(role==AppId) return id;
        if(role==Running) return id==202;
        return false;
    }
    QHash<int,QByteArray> roleNames() const override {
        return {{Name,"name"},{Running,"running"},{Art,"boxart"},{Hidden,"hidden"},
                {AppId,"appid"},{Direct,"directLaunch"},{Collector,"appCollectorGame"},{Sleep,"sleepAction"}};
    }
    Q_INVOKABLE QVariantMap get(int row) const {
        QVariantMap result;
        if (row<0 || row>=ids.size()) return result;
        const auto roles=roleNames();
        for (auto it=roles.cbegin();it!=roles.cend();++it) result[QString::fromUtf8(it.value())]=data(index(row),it.key());
        return result;
    }
    Q_INVOKABLE int indexOfApp(int id) const { return ids.indexOf(id); }
    Q_INVOKABLE void initialize(QObject*,int,bool) {}
    Q_INVOKABLE int getDirectLaunchAppIndex() { return -1; }
    Q_INVOKABLE int getRunningAppId() { return 0; }
    Q_INVOKABLE QString getRunningAppName() { return {}; }
    Q_INVOKABLE QObject* createSessionForApp(int row) { ++sessions; requestedId=ids.value(row); return nullptr; }
    Q_INVOKABLE void requestHostSleep() { ++requests; }
    Q_INVOKABLE void setAppHidden(int,bool) { ++preferenceWrites; }
    Q_INVOKABLE void setAppDirectLaunch(int,bool) { ++preferenceWrites; }
    Q_INVOKABLE void quitRunningApp() { QFAIL("Power UI must not quit an application"); }
    void complete(QString error={}) { emit hostSleepCompleted(error); }
    void insertBefore() { beginInsertRows({},0,0); ids.prepend(404); endInsertRows(); }
    void removeSelected(int id) { int row=ids.indexOf(id); beginRemoveRows({},row,row); ids.removeAt(row); endRemoveRows(); }
signals:
    void computerLost();
    void hostSleepCompleted(QString error);
};
ViewModel* ViewModel::instance=nullptr;
bool ViewModel::withGames=false;
class FakeNavigation : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString inputMethod MEMBER inputMethod CONSTANT)
public:
    QString inputMethod="controller";
    bool mode=false;
    Q_INVOKABLE int getConnectedGamepads() { return 1; }
    Q_INVOKABLE void setUiNavMode(bool value) { mode=value; }
    Q_INVOKABLE bool currentUiNavMode() const { return mode; }
    Q_INVOKABLE void forceActiveFocus() {}
    Q_INVOKABLE void pop() {}
};
class FakePreferences : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool uiReducedMotion READ reduced CONSTANT)
    Q_PROPERTY(int width READ width CONSTANT)
    Q_PROPERTY(int height READ height CONSTANT)
    Q_PROPERTY(int fps READ fps CONSTANT)
    Q_PROPERTY(int bitrateKbps READ bitrate CONSTANT)
public:
    bool reduced() const { return true; }
    int width() const { return 1920; }
    int height() const { return 1080; }
    int fps() const { return 120; }
    int bitrate() const { return 50000; }
    Q_INVOKABLE int selectedUiApp(const QString&) const { return selected; }
    Q_INVOKABLE void rememberUiSelection(const QString&,int id) { selected=id; }
    Q_INVOKABLE QVariantList recentUiApps(const QString&) const { return {}; }
    int selected=0;
signals:
    void recentUiAppsChanged(QString hostUuid);
};
class PowerViewTest : public QObject {
    Q_OBJECT
    void load(QQmlApplicationEngine& engine, FakeNavigation& stack, bool games) {
        ViewModel::withGames=games;
        engine.rootContext()->setContextProperty("stackView",&stack);
        const QUrl viewUrl=QUrl::fromLocalFile(QStringLiteral(APP_VIEW_DIRECTORY)+"/");
        QByteArray wrapper="import QtQuick 2.9\nimport QtQuick.Controls 2.5\nimport \""+viewUrl.toEncoded()+"\" as Views\nApplicationWindow { visible:true; width:1280; height:600; Views.AppView { objectName:\"Gaming PC\"; anchors.fill:parent; computerIndex:0; hostUuid:\"test-host\"; showGames:true; showHiddenGames:false } }";
        engine.loadData(wrapper,QUrl::fromLocalFile(QStringLiteral(APP_VIEW_DIRECTORY)+"/PowerViewTest.qml"));
    }
private slots:
    void initTestCase() {
        QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
        QGuiApplication::setFont(QFont("Inter"));
        qmlRegisterType<ViewModel>("AppModel",1,0,"AppModel");
        qmlRegisterType<RecentAppsModel>("AppModel",1,0,"RecentAppsModel");
        qmlRegisterSingletonType<FakePreferences>("StreamingPreferences",1,0,"StreamingPreferences",[](QQmlEngine*,QJSEngine*)->QObject* { return new FakePreferences; });
        qmlRegisterSingletonType<FakeNavigation>("ComputerManager",1,0,"ComputerManager",[](QQmlEngine*,QJSEngine*)->QObject* { return new FakeNavigation; });
        qmlRegisterSingletonType<FakeNavigation>("SdlGamepadKeyNavigation",1,0,"SdlGamepadKeyNavigation",[](QQmlEngine*,QJSEngine*)->QObject* { return new FakeNavigation; });
    }
    void confirmationCancellationAndDedicatedDispatch() {
        QQmlApplicationEngine engine; FakeNavigation stack; load(engine,stack,false);
        QVERIFY2(!engine.rootObjects().isEmpty(),"Production AppView failed to instantiate");
        auto* root=engine.rootObjects()[0];
        auto* button=root->findChild<QObject*>("emptyHostSleepAction"); QVERIFY(button);
        auto* confirmation=root->findChild<QObject*>("hostSleepConfirmation"); QVERIFY(confirmation);
        auto* model=ViewModel::instance; QVERIFY(model);
        QCOMPARE(model->requests,0); QCOMPARE(model->sessions,0);
        QVERIFY(QMetaObject::invokeMethod(button,"clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QCOMPARE(model->requests,0); QCOMPARE(model->sessions,0);
        QVERIFY(QMetaObject::invokeMethod(confirmation,"reject"));
        QTRY_VERIFY(!confirmation->property("visible").toBool());
        QCOMPARE(model->requests,0);
        QVERIFY(QMetaObject::invokeMethod(button,"clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(confirmation,"accept"));
        QCOMPARE(model->requests,1); QCOMPARE(model->sessions,0);
        QCOMPARE(model->preferenceWrites,0);
        model->complete("Lost acknowledgement; confirm again before retrying.");
        auto* result=root->findChild<QObject*>("hostSleepResult"); QVERIFY(result);
        QTRY_VERIFY(result->property("visible").toBool());
        QTest::qWait(20); QCOMPARE(model->requests,1); QCOMPARE(model->sessions,0);
    }
    void shelfDetailsResumeAndIdentityRestoration() {
        QQmlApplicationEngine engine; FakeNavigation stack; load(engine,stack,true);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root=engine.rootObjects()[0];
        auto* page=root->findChild<QObject*>("Gaming PC"); QVERIFY(page);
        auto* window=qobject_cast<QQuickWindow*>(root); QVERIFY(window);
        auto* shelf=root->findChild<QQuickItem*>("gameShelf"); QVERIFY(shelf);
        auto* model=ViewModel::instance;
        QTRY_COMPARE(shelf->property("count").toInt(),3);
        shelf->setProperty("currentIndex",1); shelf->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Menu);
        QTRY_VERIFY(page->property("detailsExpanded").toBool());
        auto* play=root->findChild<QQuickItem*>("gamePlayButton"); QVERIFY(play);
        QCOMPARE(play->property("text").toString(),QString("Resume"));
        QTRY_VERIFY(play->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Up);
        QTRY_VERIFY(!page->property("detailsExpanded").toBool());
        QVERIFY(shelf->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Return);
        QCOMPARE(model->sessions,1); QCOMPARE(model->requestedId,202);
        model->insertBefore();
        QTRY_COMPARE(shelf->property("currentIndex").toInt(),2);
        QTest::keyClick(window,Qt::Key_Return);
        QCOMPARE(model->requestedId,202);
        model->removeSelected(202);
        QTRY_VERIFY(page->property("selectedApp").toMap().value("appid").toInt()!=202);
        QCOMPARE(model->sessions,2);
        const QString capture=qEnvironmentVariable("CONSOLE_UI_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            shelf->setProperty("currentIndex",1);
            QTest::qWait(200);
            QVERIFY(window->grabWindow().save(capture+"/library.png"));
            QMetaObject::invokeMethod(page,"expandDetails"); QTest::qWait(60);
            QVERIFY(window->grabWindow().save(capture+"/details.png"));
        }
    }
};
QTEST_MAIN(PowerViewTest)
#include "tst_poweractionview.moc"
