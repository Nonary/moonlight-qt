#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include <memory>

class NavigationTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_Engine;
    std::unique_ptr<QQuickWindow> m_Window;

    QVariant evaluate(const QString& code, QObject* scope = nullptr)
    {
        if (!scope) scope = m_Window.get();
        QQmlExpression expression(qmlContext(scope), scope, code);
        const auto result = expression.evaluate();
        if (expression.hasError()) qFatal("%s", qPrintable(expression.error().toString()));
        return result;
    }

    bool busy() { return evaluate("stackView.busy").toBool(); }
    int depth() { return evaluate("stackView.depth").toInt(); }
    QObject* page() { return evaluate("stackView.currentItem").value<QObject*>(); }

    void openGames()
    {
        evaluate("stackView.currentItem.openComputer(0, 'Test PC', false)");
        QTRY_VERIFY(!busy());
        QCOMPARE(depth(), 3);
    }

private slots:
    void initTestCase()
    {
        const QUrl backend("qrc:/navigation-tests/mocks/Backend.qml");
        for (const auto name : {"ProfileManager", "ComputerManager", "StreamingPreferences",
                                "SdlGamepadKeyNavigation", "SystemProperties", "AutoUpdateChecker",
                                "PyroWaveCalibrator", "NetworkBuffers"}) {
            qmlRegisterSingletonType(backend, name, 1, 0, name);
        }
        qmlRegisterType(QUrl("qrc:/navigation-tests/mocks/ComputerModel.qml"), "ComputerModel", 1, 0, "ComputerModel");
        qmlRegisterType(QUrl("qrc:/navigation-tests/mocks/AppModel.qml"), "AppModel", 1, 0, "AppModel");
    }

    void init()
    {
        m_Engine = std::make_unique<QQmlEngine>();
        m_Engine->rootContext()->setContextProperty("initialView", "qrc:/gui/ProfileSelectionView.qml");
        m_Engine->rootContext()->setContextProperty("runConfigChecks", false);
        QQmlComponent component(m_Engine.get(), QUrl("qrc:/gui/main.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        m_Window.reset(qobject_cast<QQuickWindow*>(component.create()));
        QVERIFY(m_Window);
        m_Window->requestActivate();
        QTRY_VERIFY(m_Window->isActive());
        QTRY_VERIFY(!busy());
        QCOMPARE(depth(), 2);
    }

    void cleanup()
    {
        m_Window.reset();
        m_Engine.reset();
    }

    void initialHostHasFocus()
    {
        auto hostPage = qobject_cast<QQuickItem*>(page());
        QVERIFY(hostPage);
        QTRY_VERIFY(hostPage->hasActiveFocus());
        QTest::keyClick(m_Window.get(), Qt::Key_Right);
        QCOMPARE(hostPage->property("currentIndex").toInt(), 1);
    }

    void rapidBack_data()
    {
        QTest::addColumn<int>("key");
        QTest::newRow("controller-B") << int(Qt::Key_Escape);
        QTest::newRow("platform-back") << int(Qt::Key_Back);
    }

    void rapidBack()
    {
        QFETCH(int, key);
        openGames();
        for (int i = 0; i < 12; i++) {
            QTest::keyClick(m_Window.get(), Qt::Key(key));
        }
        QTRY_VERIFY(!busy());
        QCOMPARE(depth(), 2);
        QCOMPARE(page()->objectName(), QString("Computers"));
        QTest::keyClick(m_Window.get(), Qt::Key_Right);
        QCOMPARE(page()->property("currentIndex").toInt(), 1);

        QTest::keyClick(m_Window.get(), Qt::Key(key));
        QTRY_VERIFY(!busy());
        QCOMPARE(depth(), 1);
        QCOMPARE(page()->objectName(), QString("Profiles"));
        QTest::keyClick(m_Window.get(), Qt::Key_Right);
        QTest::keyClick(m_Window.get(), Qt::Key_Return);
        QTRY_VERIFY(!busy());
        QCOMPARE(depth(), 2);
        QCOMPARE(evaluate("ProfileManager.activeProfileId").toString(), QString("second"));
    }

    void hiddenProfileCannotStealFocusOrActivate()
    {
        openGames();
        auto profilePage = evaluate("stackView.get(0)").value<QObject*>();
        QVERIFY(profilePage);
        evaluate("profileRoot.restoreGridFocusSoon()", profilePage);
        QTest::qWait(40);
        QVERIFY(qobject_cast<QQuickItem*>(page())->hasActiveFocus());
        evaluate("profileRoot.activateProfile('second')", profilePage);
        QCOMPARE(depth(), 3);
        QCOMPARE(evaluate("ProfileManager.activeProfileId").toString(), QString("default"));
    }

    void staleComputerLostCannotPopAnotherPage()
    {
        openGames();
        QPointer<QObject> gamesPage = page();
        QPointer<QObject> gamesModel = evaluate("appModel", gamesPage).value<QObject*>();
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        evaluate("computerLost()", gamesPage);
        QTRY_VERIFY(!busy());
        QCOMPARE(depth(), 2);
        QTRY_VERIFY(gamesPage.isNull());
        QTRY_VERIFY(gamesModel.isNull());
    }

    void navigationDuringTransitionIsIgnored()
    {
        openGames();
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QVERIFY(busy());
        evaluate("window.returnToProfileSelection()");
        QVERIFY(!evaluate("window.enterProfile('second', true)").toBool());
        evaluate("window.navigateTo('qrc:/gui/SettingsView.qml', SettingsView)");
        evaluate("stackView.currentItem.openComputer(0, 'Test PC', false)");
        QTRY_VERIFY(!busy());
        QCOMPARE(depth(), 2);
        QCOMPARE(evaluate("ProfileManager.activeProfileId").toString(), QString("default"));
    }

    void dismissQuitDialogRestoresProfileFocus()
    {
        evaluate("window.returnToProfileSelection()");
        QTRY_VERIFY(!busy());
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QTRY_VERIFY(evaluate("quitConfirmationDialog.opened").toBool());
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!evaluate("quitConfirmationDialog.visible").toBool());
        QTest::keyClick(m_Window.get(), Qt::Key_Right);
        QTest::keyClick(m_Window.get(), Qt::Key_Return);
        QTRY_VERIFY(!busy());
        QCOMPARE(depth(), 2);
        QCOMPARE(evaluate("ProfileManager.activeProfileId").toString(), QString("second"));
    }

    void defaultHostDoesNotReopenOnBack()
    {
        evaluate("ComputerManager.defaultHostUuid = 'pc-1'");
        evaluate("window.returnToProfileSelection()");
        QTRY_VERIFY(!busy());
        evaluate("window.enterProfile('default', true, true)");
        QTRY_COMPARE(depth(), 3);
        QTRY_VERIFY(!busy());
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!busy());
        QTest::qWait(40);
        QCOMPARE(depth(), 2);
        QVERIFY(qobject_cast<QQuickItem*>(page())->hasActiveFocus());
    }

    void defaultHostWaitsForStatus_data()
    {
        QTest::addColumn<bool>("selectAnotherHost");
        QTest::newRow("default-host") << false;
        QTest::newRow("user-selected-another-host") << true;
    }

    void defaultHostWaitsForStatus()
    {
        QFETCH(bool, selectAnotherHost);
        evaluate("window.returnToProfileSelection()");
        QTRY_VERIFY(!busy());
        evaluate("ComputerManager.defaultHostUuid = 'pc-1'");
        evaluate("window.enterProfile('default', true, true)");
        auto hostPage = page();
        evaluate("computerModel.setProperty(0, 'statusUnknown', true)", hostPage);
        QTRY_VERIFY(!busy());
        QCOMPARE(depth(), 2);

        if (selectAnotherHost) {
            QTest::keyClick(m_Window.get(), Qt::Key_Right);
            QCOMPARE(hostPage->property("currentIndex").toInt(), 1);
        }
        evaluate("computerModel.setProperty(0, 'statusUnknown', false)", hostPage);

        if (selectAnotherHost) {
            QCOMPARE(hostPage->property("currentIndex").toInt(), 1);
            QTest::qWait(40);
            QCOMPARE(depth(), 2);
        }
        else {
            QTRY_COMPARE(depth(), 3);
            QTRY_VERIFY(!busy());
        }
    }

};

QTEST_MAIN(NavigationTest)
#include "tst_navigation.moc"
