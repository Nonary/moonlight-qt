#include <QtTest>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QAbstractListModel>

// The production AppView is loaded unchanged. Only the backend boundary is fake;
// the separate NvHTTP test executes the actual transport/protocol implementation.
class ViewModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum { Name=Qt::UserRole, Running, Art, Hidden, AppId, Direct, Collector, Sleep };
    static ViewModel* instance;
    int requests=0, sessions=0, preferenceWrites=0;
    explicit ViewModel(QObject* p=nullptr) : QAbstractListModel(p) { instance=this; }
    int rowCount(const QModelIndex& p = {}) const override { return p.isValid()?0:1; }
    QVariant data(const QModelIndex&,int role) const override {
        if(role==Name) return "Put PC to Sleep";
        if(role==Art) return QUrl("qrc:/res/no_app_image.png");
        if(role==Sleep) return true;
        if(role==AppId) return 0;
        return false;
    }
    QHash<int,QByteArray> roleNames() const override {
        return {{Name,"name"},{Running,"running"},{Art,"boxart"},{Hidden,"hidden"},
                {AppId,"appid"},{Direct,"directLaunch"},{Collector,"appCollectorGame"},{Sleep,"sleepAction"}};
    }
    Q_INVOKABLE void initialize(QObject*,int,bool) {}
    Q_INVOKABLE int getDirectLaunchAppIndex() { return -1; }
    Q_INVOKABLE int getRunningAppId() { return 0; }
    Q_INVOKABLE QString getRunningAppName() { return {}; }
    Q_INVOKABLE QObject* createSessionForApp(int) { ++sessions; return nullptr; }
    Q_INVOKABLE void requestHostSleep() { ++requests; }
    Q_INVOKABLE void setAppHidden(int,bool) { ++preferenceWrites; }
    Q_INVOKABLE void setAppDirectLaunch(int,bool) { ++preferenceWrites; }
    Q_INVOKABLE void quitRunningApp() { QFAIL("Power UI must not quit an application"); }
    void complete(QString error={}) { emit hostSleepCompleted(error); }
signals:
    void computerLost();
    void hostSleepCompleted(QString error);
};
ViewModel* ViewModel::instance=nullptr;
class FakeNavigation : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE int getConnectedGamepads() { return 0; }
    Q_INVOKABLE void forceActiveFocus() {}
    Q_INVOKABLE void pop() {}
};
class PowerViewTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qmlRegisterType<ViewModel>("AppModel",1,0,"AppModel");
        qmlRegisterType<QObject>("StreamingPreferences",1,0,"StreamingPreferences");
        qmlRegisterSingletonType<FakeNavigation>("ComputerManager",1,0,"ComputerManager",[](QQmlEngine*,QJSEngine*)->QObject* { return new FakeNavigation; });
        qmlRegisterSingletonType<FakeNavigation>("SdlGamepadKeyNavigation",1,0,"SdlGamepadKeyNavigation",[](QQmlEngine*,QJSEngine*)->QObject* { return new FakeNavigation; });
    }
    void confirmationCancellationAndDedicatedDispatch() {
        QQmlApplicationEngine engine;
        FakeNavigation stack;
        engine.rootContext()->setContextProperty("stackView",&stack);
        const QUrl viewUrl = QUrl::fromLocalFile(QStringLiteral(APP_VIEW_DIRECTORY) + "/");
        QByteArray wrapper = "import QtQuick 2.9\nimport QtQuick.Controls 2.2\nimport \"" + viewUrl.toEncoded() + "\" as Views\nApplicationWindow { visible:true; width:900; height:650; Views.AppView { objectName: \"grid\"; anchors.fill:parent; computerIndex:0; showGames:true; showHiddenGames:false } }";
        engine.loadData(wrapper, QUrl::fromLocalFile(QStringLiteral(APP_VIEW_DIRECTORY)+"/PowerViewTest.qml"));
        QVERIFY2(!engine.rootObjects().isEmpty(), "Production AppView failed to instantiate");
        auto* root=engine.rootObjects()[0];
        auto* grid=root->findChild<QObject*>("grid"); QVERIFY(grid);
        grid->setProperty("currentIndex",0);
        QTRY_VERIFY(grid->property("currentItem").value<QObject*>());
        auto* tile=grid->property("currentItem").value<QObject*>();
        QCOMPARE(tile->objectName(), QString("hostSleepTile"));
        auto* confirmation=root->findChild<QObject*>("hostSleepConfirmation"); QVERIFY(confirmation);
        auto* model=ViewModel::instance; QVERIFY(model);
        QCOMPARE(model->requests,0); QCOMPARE(model->sessions,0);
        QVERIFY(QMetaObject::invokeMethod(tile,"clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QCOMPARE(model->requests,0); QCOMPARE(model->sessions,0);
        if (!qEnvironmentVariable("POWER_ACTION_VIEW_CAPTURE").isEmpty()) {
            QTest::qWait(100);
            auto* window=qobject_cast<QQuickWindow*>(root); QVERIFY(window);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("POWER_ACTION_VIEW_CAPTURE")));
        }
        QVERIFY(QMetaObject::invokeMethod(confirmation,"reject"));
        QTRY_VERIFY(!confirmation->property("visible").toBool());
        QCOMPARE(model->requests,0);
        QVERIFY(QMetaObject::invokeMethod(tile,"clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(confirmation,"accept"));
        QCOMPARE(model->requests,1); QCOMPARE(model->sessions,0);
        QVERIFY(QMetaObject::invokeMethod(tile,"clicked"));
        QCOMPARE(model->requests,1); QCOMPARE(model->preferenceWrites,0);
        model->complete("Lost acknowledgement; confirm again before retrying.");
        auto* result=root->findChild<QObject*>("hostSleepResult"); QVERIFY(result);
        QTRY_VERIFY(result->property("visible").toBool());
        QTest::qWait(20); QCOMPARE(model->requests,1); QCOMPARE(model->sessions,0);
    }
};
QTEST_MAIN(PowerViewTest)
#include "tst_poweractionview.moc"
