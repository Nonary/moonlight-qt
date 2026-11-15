#include "gui/sdlgamepadkeynavigation.h"
#include "consolefixtures.h"
#include "gui/recentappsmodel.h"
#include "settings/mappingmanager.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QDir>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QWindow>
#include <QtTest>
#include <QFontDatabase>
#include <functional>
#include <memory>

// Mapping downloads and display discovery are unrelated to key delivery.
MappingManager::MappingManager() {}
void MappingManager::applyMappings() {}
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class KeyWindow : public QWindow {
public:
    struct Key { QEvent::Type type; int key; Qt::KeyboardModifiers modifiers; };
    QList<Key> keys;
    std::function<void()> onPress;
    bool event(QEvent* event) override {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            const auto* key = static_cast<QKeyEvent*>(event);
            keys.append({event->type(), key->key(), key->modifiers()});
            if (event->type() == QEvent::KeyPress && onPress) onPress();
            return true;
        }
        return QWindow::event(event);
    }
};

class ControllerNavigationTest : public QObject {
    Q_OBJECT
    QTemporaryDir config;
    KeyWindow window;
    StreamingPreferences* prefs = nullptr;
    std::unique_ptr<SdlGamepadKeyNavigation> navigation;
    QList<SDL_Joystick*> virtualControllers;
    void button(Uint32 type, Uint8 button, Sint32 controller = 1) {
        SDL_Event event {};
        event.type = type;
        event.cbutton.type = type;
        event.cbutton.button = button;
        event.cbutton.which = virtualControllers.isEmpty() ? controller :
            SDL_JoystickInstanceID(virtualControllers[controller - 1]);
        QCOMPARE(SDL_PushEvent(&event), 1);
        QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
    }
private slots:
    void initTestCase() {
        QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
        QGuiApplication::setFont(QFont("Inter"));
        qmlRegisterType<ConsoleHostModel>("ComputerModel",1,0,"ComputerModel");
        qmlRegisterType<ConsoleAppModel>("AppModel",1,0,"AppModel");
        qmlRegisterType<RecentAppsModel>("AppModel",1,0,"RecentAppsModel");
        QVERIFY(config.isValid());
        QCoreApplication::setOrganizationName("MoonlightControllerNavigationTest");
        QCoreApplication::setApplicationName("IsolatedPreferences");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, config.path());
        prefs = StreamingPreferences::get();
#ifdef Q_OS_WIN
        // Keep real controllers out of deterministic injected-input tests.
        // These hints affect only this test process; virtual devices remain available.
        SDL_SetHint("SDL_JOYSTICK_HIDAPI", "0");
        SDL_SetHint("SDL_JOYSTICK_RAWINPUT", "0");
        SDL_SetHint("SDL_JOYSTICK_DINPUT", "0");
        SDL_SetHint("SDL_JOYSTICK_WGI", "0");
        SDL_SetHint("SDL_XINPUT_ENABLED", "0");
#endif
        navigation = std::make_unique<SdlGamepadKeyNavigation>(prefs);
        qmlRegisterSingletonType<StreamingPreferences>("StreamingPreferences", 1, 0, "StreamingPreferences",
            [](QQmlEngine*, QJSEngine*) -> QObject* {
                auto* value=StreamingPreferences::get();
                QQmlEngine::setObjectOwnership(value, QQmlEngine::CppOwnership);
                return value;
            });
        qmlRegisterSingletonType<SdlGamepadKeyNavigation>("SdlGamepadKeyNavigation", 1, 0, "SdlGamepadKeyNavigation",
            [this](QQmlEngine*, QJSEngine*) -> QObject* { QQmlEngine::setObjectOwnership(navigation.get(), QQmlEngine::CppOwnership); return navigation.get(); });
        navigation->enable();
#if SDL_VERSION_ATLEAST(2, 0, 14)
        // Keep virtual devices alive across the navigation bridge's normal
        // disable/enable cycle when streaming takes ownership of controllers.
        QCOMPARE(SDL_InitSubSystem(SDL_INIT_JOYSTICK), 0);
        // sdl2-compat translates injected events through SDL3 and requires
        // real instance IDs to distinguish controllers during that conversion.
        for (int i = 0; i < 2; ++i) {
            const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
            QVERIFY(index >= 0);
            auto* joystick = SDL_JoystickOpen(index);
            QVERIFY(joystick);
            virtualControllers.append(joystick);
        }
#endif
        window.show();
        window.requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), &window);
    }
    void init() {
        navigation->notifyWindowFocus(false);
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        prefs->swapFaceButtons = false;
        // Most shell cases verify a specific desktop geometry. Fullscreen's
        // default and explicit-exit contract have their own window tests below.
        prefs->uiControllerFullscreen = false;
        window.keys.clear();
        window.onPress = {};
    }
    void modeChangeDuringPress_data() {
        QTest::addColumn<int>("buttonCode");
        QTest::addColumn<bool>("uiMode");
        QTest::addColumn<int>("expectedKey");
        QTest::addColumn<int>("expectedModifiers");
        QTest::newRow("open-menu") << int(SDL_CONTROLLER_BUTTON_A) << true << int(Qt::Key_Space) << int(Qt::NoModifier);
        QTest::newRow("confirm-menu") << int(SDL_CONTROLLER_BUTTON_A) << false << int(Qt::Key_Return) << int(Qt::NoModifier);
        QTest::newRow("focus-up") << int(SDL_CONTROLLER_BUTTON_DPAD_UP) << true << int(Qt::Key_Backtab) << int(Qt::NoModifier);
        QTest::newRow("menu-up") << int(SDL_CONTROLLER_BUTTON_DPAD_UP) << false << int(Qt::Key_Up) << int(Qt::NoModifier);
        QTest::newRow("focus-down") << int(SDL_CONTROLLER_BUTTON_DPAD_DOWN) << true << int(Qt::Key_Tab) << int(Qt::NoModifier);
        QTest::newRow("menu-down") << int(SDL_CONTROLLER_BUTTON_DPAD_DOWN) << false << int(Qt::Key_Down) << int(Qt::NoModifier);
    }
    void modeChangeDuringPress() {
        QFETCH(int, buttonCode); QFETCH(bool, uiMode);
        QFETCH(int, expectedKey); QFETCH(int, expectedModifiers);
        navigation->setUiNavMode(uiMode);
        window.onPress = [this, uiMode] { navigation->setUiNavMode(!uiMode); };
        button(SDL_CONTROLLERBUTTONDOWN, Uint8(buttonCode));
        button(SDL_CONTROLLERBUTTONUP, Uint8(buttonCode));
        QCOMPARE(window.keys.size(), 2);
        QCOMPARE(window.keys[0].type, QEvent::KeyPress);
        QCOMPARE(window.keys[1].type, QEvent::KeyRelease);
        for (const auto& key : window.keys) {
            QCOMPARE(key.key, expectedKey);
            QCOMPARE(int(key.modifiers), expectedModifiers);
        }
    }
    void controllersKeepIndependentPresses() {
        navigation->setUiNavMode(true);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A, 1);
        navigation->setUiNavMode(false);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A, 2);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A, 1);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A, 2);
        QCOMPARE(window.keys.size(), 4);
        QCOMPARE(window.keys[2].key, int(Qt::Key_Space));
        QCOMPARE(window.keys[3].key, int(Qt::Key_Return));
    }
    void swapChangeStillReleasesOriginalKey() {
        navigation->setUiNavMode(true);
        prefs->swapFaceButtons = true;
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_B);
        prefs->swapFaceButtons = false;
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_B);
        QCOMPARE(window.keys.size(), 2);
        QCOMPARE(window.keys[0].key, int(Qt::Key_Space));
        QCOMPARE(window.keys[1].key, int(Qt::Key_Space));
    }
    void dpadOnePressOneRelease() {
        navigation->setUiNavMode(true);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        QCOMPARE(window.keys.size(), 0);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        QCOMPARE(window.keys.size(), 1);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        QCOMPARE(window.keys.size(), 2);
        QCOMPARE(window.keys[0].key, int(Qt::Key_Left));
        QCOMPARE(window.keys[1].key, int(Qt::Key_Left));
        for (int i = 0; i < 10; ++i) {
            button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        }
        QCOMPARE(window.keys.size(), 22);
    }
    void focusResetClosesPressAndDiscardsStrayRelease() {
        navigation->setUiNavMode(true);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_UP);
        navigation->setUiNavMode(false);
        navigation->notifyWindowFocus(false);
        QCOMPARE(window.keys.size(), 2);
        QCOMPARE(window.keys[1].type, QEvent::KeyRelease);
        QCOMPARE(window.keys[1].key, int(Qt::Key_Backtab));
        QCOMPARE(window.keys[1].modifiers, Qt::KeyboardModifiers(Qt::NoModifier));
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_UP);
        QCOMPARE(window.keys.size(), 2);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_UP);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_UP);
        QCOMPARE(window.keys.size(), 4);
        QCOMPARE(window.keys[2].key, int(Qt::Key_Up));
    }
    void shouldersAndInputMethod() {
        navigation->setUiNavMode(true);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        navigation->setUiNavMode(false);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        QCOMPARE(window.keys.size(), 2);
        QCOMPARE(window.keys[0].key, int(Qt::Key_PageUp));
        QCOMPARE(window.keys[1].key, int(Qt::Key_PageUp));
        QCOMPARE(navigation->inputMethod(), QString("controller"));
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        QCOMPARE(window.keys[2].key, int(Qt::Key_PageDown));
        QCOMPARE(window.keys[3].key, int(Qt::Key_PageDown));
        QKeyEvent physical(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &physical);
        QCOMPARE(navigation->inputMethod(), QString("keyboard"));
    }
    void heldDpadRepeatStopsOnModeChange() {
        navigation->notifyWindowFocus(true);
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        navigation->setUiNavMode(false);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        QTest::qWait(470);
        QVERIFY(window.keys.size() >= 2);
        navigation->setUiNavMode(true);
        const int heldCount = window.keys.size();
        QTest::qWait(240);
        QCOMPARE(window.keys.size(), heldCount);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        QCOMPARE(window.keys.last().type, QEvent::KeyRelease);
        navigation->notifyWindowFocus(false);
    }
    void uiSelectionsPersistByIdentity() {
        prefs->rememberUiSelection("host-one", 101);
        prefs->rememberUiSelection("host-two", 202);
        QCOMPARE(prefs->selectedUiApp("host-one"), 101);
        QCOMPARE(prefs->selectedUiApp("host-two"), 202);
        QCOMPARE(prefs->selectedUiApp("missing-host"), 0);
        prefs->uiReducedMotion = true;
        prefs->save();
        prefs->reload();
        QCOMPARE(prefs->uiLastHostUuid, QString("host-two"));
        QVERIFY(prefs->uiReducedMotion);
    }
    void playHistoryPersistsAndStaysSeparateFromSelection() {
        QSettings().remove("ui/recentApps");
        prefs->rememberUiSelection("history-one", 99);
        QVERIFY(prefs->recentUiApps("history-one").isEmpty());
        prefs->recordUiPlay("history-one", 4);
        prefs->recordUiPlay("history-one", 2);
        prefs->recordUiPlay("history-one", 4);
        prefs->recordUiPlay("history-two", 7);
        prefs->recordUiPlay("", 8);
        prefs->recordUiPlay("history-one", 0);
        QCOMPARE(prefs->recentUiApps("history-one"), QVariantList({4, 2}));
        QCOMPARE(prefs->recentUiApps("history-two"), QVariantList({QVariant(7)}));
        QVERIFY(prefs->recentUiApps("").isEmpty());
        prefs->save(); prefs->reload();
        QCOMPARE(prefs->recentUiApps("history-one"), QVariantList({4, 2}));
        for (int id = 1; id <= 30; ++id) prefs->recordUiPlay("history-cap", id);
        const auto capped = prefs->recentUiApps("history-cap");
        QCOMPARE(capped.size(), 20);
        QCOMPARE(capped.first().toInt(), 30);
        QCOMPARE(capped.last().toInt(), 11);
        QSettings().remove("ui/recentApps");
    }
    void controllerFullscreenPreferenceDefaultsOnAndPersists() {
        QSettings().remove("ui/controllerFullscreen");
        prefs->reload();
        QVERIFY(prefs->uiControllerFullscreen);
        prefs->uiControllerFullscreen = false;
        prefs->save(); prefs->reload();
        QVERIFY(!prefs->uiControllerFullscreen);
    }
    void controllerFullscreenStaysUntilExplicitExit_data() {
        QTest::addColumn<int>("desktopMode");
        QTest::newRow("windowed") << int(StreamingPreferences::UI_WINDOWED);
        QTest::newRow("maximized") << int(StreamingPreferences::UI_MAXIMIZED);
    }
    void controllerFullscreenStaysUntilExplicitExit() {
        QFETCH(int, desktopMode);
        prefs->uiControllerFullscreen = true;
        prefs->uiDisplayMode = static_cast<StreamingPreferences::UIDisplayMode>(desktopMode);
        prefs->uiLastHostUuid.clear();
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        QTRY_VERIFY(navigation->connectedGamepads() > 0);
        QTRY_COMPARE(root->visibility(), QWindow::FullScreen);
        auto* fullscreenButton = root->findChild<QQuickItem*>("uiFullscreenButton"); QVERIFY(fullscreenButton);
        QCOMPARE(fullscreenButton->property("text").toString(), QString("Exit fullscreen"));
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QTest::keyClick(root, Qt::Key_Shift);
        QTest::qWait(30);
        QCOMPARE(navigation->inputMethod(), QString("keyboard"));
        QCOMPARE(root->visibility(), QWindow::FullScreen);
        navigation->disable(); navigation->enable();
        QTest::qWait(20);
        QCOMPARE(root->visibility(), QWindow::FullScreen);
        navigation->notifyWindowFocus(true);
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        const auto press = [&](Uint8 code) { button(SDL_CONTROLLERBUTTONDOWN, code); button(SDL_CONTROLLERBUTTONUP, code); };
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        QTest::qWait(20);
        QCOMPARE(root->visibility(), QWindow::FullScreen);
        const auto exitPoint = fullscreenButton->mapToScene(QPointF(fullscreenButton->width()/2, fullscreenButton->height()/2)).toPoint();
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, exitPoint);
        const auto desktopVisibility = desktopMode == StreamingPreferences::UI_MAXIMIZED ? QWindow::Maximized : QWindow::Windowed;
        QTRY_COMPARE(root->visibility(), desktopVisibility);
        QCOMPARE(root->property("uiFullscreenOverride").toInt(), 0);
        press(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        QTest::qWait(30);
        QCOMPARE(root->visibility(), desktopVisibility);
        // Device teardown/reconnect and hiding the UI must not undo an exit.
        navigation->disable(); navigation->enable();
        root->hide(); root->show(); root->requestActivate();
        QTest::qWait(30);
        QCOMPARE(root->visibility(), desktopVisibility);
        QCOMPARE(prefs->uiDisplayMode, static_cast<StreamingPreferences::UIDisplayMode>(desktopMode));
        QTest::keyClick(root, Qt::Key_F11);
        QTRY_COMPARE(root->visibility(), QWindow::FullScreen);
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QTest::keyClick(root, Qt::Key_Shift);
        QTest::qWait(20);
        QCOMPARE(root->visibility(), QWindow::FullScreen);
        // A native window action is an explicit exit too.
        root->showNormal();
        QTRY_COMPARE(root->property("uiFullscreenOverride").toInt(), 0);
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        QTest::qWait(20);
        QCOMPARE(root->visibility(), QWindow::Windowed);
        prefs->setProperty("uiDisplayMode", StreamingPreferences::UI_FULLSCREEN);
        QTRY_COMPARE(root->visibility(), QWindow::FullScreen);
        prefs->setProperty("uiDisplayMode", StreamingPreferences::UI_WINDOWED);
        QTRY_COMPARE(root->visibility(), QWindow::Windowed);
        press(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        QTest::qWait(20);
        QCOMPARE(root->visibility(), QWindow::Windowed);
        navigation->notifyWindowFocus(false);
    }
    void controllerFullscreenCanBeDisabled() {
        prefs->uiControllerFullscreen = false;
        prefs->uiDisplayMode = StreamingPreferences::UI_WINDOWED;
        prefs->uiLastHostUuid.clear();
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        QVERIFY(navigation->connectedGamepads() > 0);
        QTRY_COMPARE(root->visibility(), QWindow::Windowed);
        navigation->notifyWindowFocus(true);
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        QTest::qWait(20);
        QCOMPARE(root->visibility(), QWindow::Windowed);
        // The explicit fullscreen action remains available with auto disabled.
        QTest::keyClick(root, Qt::Key_F11);
        QTRY_COMPARE(root->visibility(), QWindow::FullScreen);
        QTest::keyClick(root, Qt::Key_F11);
        QTRY_COMPARE(root->visibility(), QWindow::Windowed);
        navigation->notifyWindowFocus(false);
    }
    void recentModelUsesOnlyAvailableVisibleHostApps() {
        ConsoleAppModel catalogue;
        RecentAppsModel recent;
        recent.setSourceModel(&catalogue);
        recent.setAppIds({4, 99, 2, 1});
        QCOMPARE(recent.rowCount(), 3);
        QCOMPARE(recent.get(0).value("appid").toInt(), 4);
        QCOMPARE(recent.get(1).value("appid").toInt(), 2);
        QCOMPARE(recent.indexOfApp(1), 2);
        catalogue.setAppHidden(catalogue.indexOfApp(2), true);
        QCOMPARE(recent.rowCount(), 2);
        QCOMPARE(recent.indexOfApp(2), -1);
        catalogue.removeApp(4);
        QCOMPARE(recent.rowCount(), 1);
        catalogue.insertApp(4);
        QCOMPARE(recent.get(0).value("appid").toInt(), 4);
        recent.setAppIds({1, 4});
        QCOMPARE(recent.get(0).value("appid").toInt(), 1);
        QCOMPARE(catalogue.get(0).value("appid").toInt(), 4);
    }
    void layeredShelvesNavigateAndKeepTheirSelectedGame() {
        QSettings().remove("ui/recentApps/host-one");
        QSettings().remove("ui/selectedApps/host-one");
        prefs->recordUiPlay("host-one", 1);
        prefs->recordUiPlay("host-one", 4);
        prefs->recordUiPlay("host-one", 2);
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        prefs->uiLastHostUuid.clear();
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        auto* add = root->findChild<QQuickItem*>("addPcTile"); QVERIFY(add);
        add->forceActiveFocus();
        QTest::keyClick(root, Qt::Key_Left);
        QTest::keyClick(root, Qt::Key_Return);
        auto* recent = root->findChild<QQuickItem*>("recentGameShelf"); QTRY_VERIFY(recent);
        auto* all = root->findChild<QQuickItem*>("gameShelf"); QVERIFY(all);
        auto* page = recent->parentItem();
        while (page && !page->property("recentRowActive").isValid()) page = page->parentItem();
        QVERIFY(page);
        auto* model = page->property("appModel").value<ConsoleAppModel*>(); QVERIFY(model);
        auto selectedId = [&]() { return page->property("selectedApp").toMap().value("appid").toInt(); };
        QTRY_VERIFY(recent->hasActiveFocus());
        QTRY_COMPARE(selectedId(), 2);
        QTest::keyClick(root, Qt::Key_Right);
        QCOMPARE(selectedId(), 4);
        QTest::keyClick(root, Qt::Key_Down);
        QVERIFY(all->hasActiveFocus());
        QVERIFY(!page->property("detailsExpanded").toBool());
        QTest::keyClick(root, Qt::Key_Right);
        const int libraryId = selectedId();
        QTest::keyClick(root, Qt::Key_Down);
        QVERIFY(all->hasActiveFocus());
        QVERIFY(!page->property("detailsExpanded").toBool());
        QTest::keyClick(root, Qt::Key_Up);
        QVERIFY(recent->hasActiveFocus());
        QCOMPARE(selectedId(), 4);
        QTest::keyClick(root, Qt::Key_Menu);
        auto* play = root->findChild<QQuickItem*>("gamePlayButton"); QVERIFY(play);
        QTRY_VERIFY(play->hasActiveFocus());
        QTest::keyClick(root, Qt::Key_Escape);
        QTRY_VERIFY(recent->hasActiveFocus());
        QCOMPARE(selectedId(), 4);
        model->refreshArtwork(4);
        QTest::qWait(20);
        QVERIFY(recent->hasActiveFocus()); QCOMPARE(selectedId(), 4);
        prefs->recordUiPlay("host-one", 1);
        QTRY_COMPARE(selectedId(), 4);
        QTRY_COMPARE(recent->property("currentIndex").toInt(), 2);
        QTest::keyClick(root, Qt::Key_Down);
        QCOMPARE(selectedId(), libraryId);
        QTest::keyClick(root, Qt::Key_Menu);
        QTRY_VERIFY(play->hasActiveFocus());
        QTest::keyClick(root, Qt::Key_Escape);
        QTRY_VERIFY(all->hasActiveFocus()); QCOMPARE(selectedId(), libraryId);
        QTest::keyClick(root, Qt::Key_Up);
        model->removeApp(4);
        QTRY_VERIFY(selectedId() != 4);
        QTRY_VERIFY(recent->hasActiveFocus());
        const QString capture = qEnvironmentVariable("CONSOLE_UI_CAPTURE_DIR");
        for (const QSize size : {QSize(800,500), QSize(1280,720), QSize(1920,1080), QSize(3840,2160)}) {
            root->resize(size); QTest::qWait(70);
            if(size == QSize(800,500)) {
                auto* scroll=root->findChild<QQuickItem*>("gameShelfScroll"); QVERIFY(scroll);
                QVERIFY2(scroll->property("contentHeight").toReal() <= scroll->height()+1, "Both game shelves must fit above the footer");
            }
            for (auto key : {Qt::Key_Down, Qt::Key_Up}) {
                QTest::keyClick(root, key); QTest::qWait(30);
                auto* active = key == Qt::Key_Down ? all : recent;
                QVERIFY(active->hasActiveFocus());
                auto* viewport = root->findChild<QQuickItem*>("gameShelfScroll"); QVERIFY(viewport);
                const auto top = active->mapToItem(viewport, QPointF());
                QVERIFY2(top.y() >= -1 && top.y() + active->height() <= viewport->height() + 1,
                         "The focused game row must be fully visible");
            }
            if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture + QString("/shelves-%1.png").arg(size.width())));
        }
        QTest::keyClick(root, Qt::Key_Up);
        QVERIFY(!recent->hasActiveFocus());
        // Emptying Recents skips that row and leaves the complete catalogue usable.
        model->setAppHidden(model->indexOfApp(1), true);
        model->setAppHidden(model->indexOfApp(2), true);
        QTRY_COMPARE(recent->property("count").toInt(), 0);
        all->forceActiveFocus();
        QTest::keyClick(root, Qt::Key_Down);
        QVERIFY(all->hasActiveFocus()); QVERIFY(!page->property("detailsExpanded").toBool());
        QSettings().remove("ui/recentApps/host-one");
    }
    void settingsCategoriesAndFocus() {
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        const auto directory=QUrl::fromLocalFile(QStringLiteral(CONSOLE_GUI_PATH)+"/");
        const QByteArray wrapper="import QtQuick 2.9\nimport QtQuick.Controls 2.5\nimport \""+directory.toEncoded()+"\" as Views\nApplicationWindow { id:window; visible:true; width:1280; height:600; property bool pollingActive:false; Item { id:stackView; property bool focusViewAfterDialog:false } Views.SettingsView { objectName:\"settings\"; anchors.fill:parent } }";
        engine.loadData(wrapper,directory.resolved(QUrl("SettingsTest.qml")));
        QVERIFY2(!engine.rootObjects().isEmpty(),"Production settings failed to instantiate");
        auto* root=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        auto* settings=root->findChild<QObject*>("settings"); QVERIFY(settings);
        auto* categories=root->findChild<QQuickItem*>("settingsCategories"); QVERIFY(categories);
        QTest::qWait(100);
        categories->forceActiveFocus();
        for (int category=0; category<8; ++category) {
            QCOMPARE(settings->property("category").toInt(),category);
            QTest::qWait(30); // Let Columns position the newly visible category.
            QTest::keyClick(root,Qt::Key_Right);
            QVERIFY2(!categories->hasActiveFocus(),qPrintable(QString("Right must enter settings category %1").arg(category)));
            // Walk every visible row forwards and backwards. This catches
            // closed selectors consuming arrows, reversed traversal, and focus
            // escaping into a hidden category or the shell's footer.
            QList<QQuickItem*> rows;
            std::function<void(QQuickItem*)> collect = [&](QQuickItem* item) {
                if (!item->isVisible() || !item->isEnabled()) return;
                if (item->activeFocusOnTab()) { rows.append(item); return; }
                for (auto* child : item->childItems()) collect(child);
            };
            auto* first = root->activeFocusItem();
            auto* content = first;
            while (content && content->parentItem() &&
                   !content->parentItem()->inherits("QQuickFlickable")) content=content->parentItem();
            QVERIFY(content);
            collect(content);
            std::sort(rows.begin(), rows.end(), [](QQuickItem* a, QQuickItem* b) {
                auto pa=a->mapToScene(QPointF()); auto pb=b->mapToScene(QPointF());
                return qAbs(pa.y()-pb.y())>1 ? pa.y()<pb.y() : pa.x()<pb.x();
            });
            QVERIFY(!rows.isEmpty());
            for (auto key : {Qt::Key_Down, Qt::Key_Tab}) {
                rows.first()->forceActiveFocus();
                for (int i=1;i<rows.size();++i) {
                    const auto oldIndex = rows[i-1]->property("currentIndex");
                    const auto oldValue = rows[i-1]->property("value");
                    QTest::keyClick(root,key);
                    QCOMPARE(rows[i-1]->property("currentIndex"), oldIndex);
                    QCOMPARE(rows[i-1]->property("value"), oldValue);
                    QVERIFY2(rows[i]->hasActiveFocus(),qPrintable(QString("category %1 row %2 key %3 expected %4 (%5) actual %6 (%7)").arg(category).arg(i).arg(key).arg(rows[i]->metaObject()->className()).arg(rows[i]->property("text").toString()).arg(root->activeFocusItem()->metaObject()->className()).arg(root->activeFocusItem()->property("text").toString())));
                }
                QTest::keyClick(root,key);
                QVERIFY2(rows.last()->hasActiveFocus(),qPrintable(QString("end category %1 key %2 focus %3").arg(category).arg(key).arg(root->activeFocusItem() ? root->activeFocusItem()->metaObject()->className() : "none")));
                for (int i=rows.size()-2;i>=0;--i) {
                    QTest::keyClick(root,key==Qt::Key_Down ? Qt::Key_Up : Qt::Key_Tab,
                                    key==Qt::Key_Tab ? Qt::ShiftModifier : Qt::NoModifier);
                    QVERIFY2(rows[i]->hasActiveFocus(),qPrintable(QString("reverse category %1 row %2").arg(category).arg(i)));
                }
                QTest::keyClick(root,Qt::Key_Up);
                QVERIFY(rows.first()->hasActiveFocus());
            }
            // Opening a selector must restore normal option navigation. Its
            // Up/Down keys must not move focus to another settings row.
            for (auto* row : rows) {
                auto* popup = row->property("popup").value<QObject*>();
                if (!popup || row->property("count").toInt()<2) continue;
                // Offscreen windows don't drive Material's popup animation.
                popup->setProperty("enter", QVariant::fromValue<QObject*>(nullptr));
                popup->setProperty("exit", QVariant::fromValue<QObject*>(nullptr));
                row->forceActiveFocus();
                QTest::keyClick(root,Qt::Key_Return);
                QTRY_VERIFY2(popup->property("visible").toBool(),qPrintable(QString("category %1 selector %2").arg(category).arg(row->metaObject()->className())));
                const int initial=row->property("highlightedIndex").toInt();
                const auto direction=initial+1<row->property("count").toInt() ? Qt::Key_Down : Qt::Key_Up;
                QTest::keyClick(root,direction);
                QCOMPARE(row->property("highlightedIndex").toInt(),initial+(direction==Qt::Key_Down ? 1 : -1));
                QTest::keyClick(root,Qt::Key_Escape);
                QTRY_VERIFY(!popup->property("visible").toBool());
                QVERIFY(row->hasActiveFocus());
                break;
            }
            categories->forceActiveFocus();
            if (category<7) QTest::keyClick(root,Qt::Key_Tab);
        }
        settings->setProperty("category",0);
        const QString capture=qEnvironmentVariable("CONSOLE_UI_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(root->grabWindow().save(capture+"/settings.png"));
        }
        // Reproduce the user's controller path, including the SDL-generated
        // key code/modifiers rather than substituting a keyboard Up event.
        settings->setProperty("category", 1);
        categories->forceActiveFocus();
        navigation->setUiNavMode(true);
        navigation->notifyWindowFocus(true);
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_UP);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_UP);
        const int categoryAfterUp = settings->property("category").toInt();
        qInfo() << "Reported Up reproducer: starting category 1, expected 0, actual" << categoryAfterUp;
        settings->setProperty("category", 1);
        categories->forceActiveFocus();
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        const bool returnedToMenu = categories->hasActiveFocus();
        qInfo() << "Reported Right/Left reproducer: expected category menu focus true, actual" << returnedToMenu;
        navigation->notifyWindowFocus(false);
        if (categoryAfterUp != 0) QTest::qFail("Controller Up moved the Settings category down", __FILE__, __LINE__);
        if (!returnedToMenu) QTest::qFail("Controller Left did not return to the Settings category menu", __FILE__, __LINE__);
    }
    void shellHostLibrarySettingsAndSmallWindow() {
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        const auto directory=QUrl::fromLocalFile(QStringLiteral(CONSOLE_GUI_PATH)+"/");
        prefs->uiLastHostUuid.clear();
        engine.rootContext()->setContextProperty("initialView",QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks",false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY2(!engine.rootObjects().isEmpty(),"Production shell failed to instantiate");
        auto* root=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        QTest::qWait(100);
        auto* addTile=root->findChild<QQuickItem*>("addPcTile"); QVERIFY(addTile);
        addTile->forceActiveFocus();
        QTest::keyClick(root,Qt::Key_Left);
        QTRY_VERIFY(!addTile->hasActiveFocus());
        const QString capture=qEnvironmentVariable("CONSOLE_UI_CAPTURE_DIR");
        if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+"/pcs.png"));
        QTest::keyClick(root,Qt::Key_Return);
        QTRY_VERIFY(root->findChild<QQuickItem*>("gameShelf"));
        auto* shelf=root->findChild<QQuickItem*>("gameShelf");
        QTRY_VERIFY(shelf->isVisible());
        QKeyEvent settingsPress(QEvent::KeyPress,Qt::Key_Hangup,Qt::NoModifier);
        QCoreApplication::sendEvent(root,&settingsPress);
        QKeyEvent settingsRelease(QEvent::KeyRelease,Qt::Key_Hangup,Qt::NoModifier);
        QCoreApplication::sendEvent(root,&settingsRelease);
        auto* categories=root->findChild<QQuickItem*>("settingsCategories");
        QTRY_VERIFY(categories && categories->isVisible());
        QTest::qWait(60);
        if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+"/settings-main.png"));
        QTest::keyClick(root,Qt::Key_Escape);
        QTRY_VERIFY(shelf->isVisible());
        QTest::keyClick(root,Qt::Key_Menu);
        auto* play=root->findChild<QQuickItem*>("gamePlayButton");
        QTRY_VERIFY(play && play->hasActiveFocus());
        QTest::keyClick(root,Qt::Key_Menu);
        QTRY_VERIFY(shelf->hasActiveFocus());
        QVERIFY(!categories->isVisible());
        QTest::keyClick(root,Qt::Key_Menu);
        QTRY_VERIFY(play->hasActiveFocus());
        for (int i=0;i<10;++i) QTest::keyClick(root,Qt::Key_Down);
        auto* lastAction = root->activeFocusItem();
        QVERIFY(lastAction != play);
        QTest::keyClick(root,Qt::Key_Down); QCOMPARE(root->activeFocusItem(),lastAction);
        QTest::keyClick(root,Qt::Key_Escape);
        QTRY_VERIFY(shelf->hasActiveFocus());
        QTest::keyClick(root,Qt::Key_Menu);
        for (const QSize size : {QSize(800,500),QSize(1280,720),QSize(1920,1080),QSize(3840,2160)}) {
            root->resize(size);
            QTest::qWait(50);
            const QPointF position=play->mapToScene(QPointF(0,0));
            QVERIFY2(position.y()>=0 && position.y()+play->height()<=root->height(),"Play action must remain on screen");
            if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+QString("/details-%1.png").arg(size.width())));
            QTest::keyClick(root,Qt::Key_Escape);
            QTest::qWait(50);
            if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+QString("/home-%1.png").arg(size.width())));
            QTest::keyClick(root,Qt::Key_Menu);
        }
        root->resize(1280,720);
        QTest::keyClick(root,Qt::Key_Escape);
        QTest::keyClick(root,Qt::Key_N,Qt::ControlModifier);
        auto* address=root->findChild<QObject*>("hostAddress"); QVERIFY(address);
        QTRY_VERIFY(address->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(address,"openKeyboard"));
        auto* keyboard=root->findChild<QObject*>("consoleKeyboard"); QVERIFY(keyboard);
        QTRY_VERIFY(keyboard->property("opened").toBool());
        if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+"/keyboard.png"));
        keyboard->setProperty("numeric",false);
        QTest::qWait(60);
        if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+"/keyboard-qwerty.png"));
        QVERIFY(QMetaObject::invokeMethod(keyboard,"reject"));
        QTRY_VERIFY(!keyboard->property("visible").toBool());
        QTest::keyClick(root,Qt::Key_Escape);
        QTRY_VERIFY(!address->property("visible").toBool());
        auto* pairing=root->findChild<QObject*>("pairingDialog"); QVERIFY(pairing);
        pairing->setProperty("pin","8452");
        QVERIFY(QMetaObject::invokeMethod(pairing,"open"));
        QTRY_VERIFY(pairing->property("opened").toBool());
        if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+"/pairing.png"));
        QVERIFY(QMetaObject::invokeMethod(pairing,"reject"));
        QTRY_VERIFY(!pairing->property("visible").toBool());
        auto* error=root->findChild<QObject*>("streamError"); QVERIFY(error);
        error->setProperty("text","Unable to connect to Gaming PC. Check that the host is awake and connected to your network, then try again.");
        QVERIFY(QMetaObject::invokeMethod(error,"open"));
        QTRY_VERIFY(error->property("opened").toBool());
        if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+"/error.png"));
        QVERIFY(QMetaObject::invokeMethod(error,"reject"));
    }
    void settingsControllerBrowseEditAndReturn() {
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        prefs->uiLastHostUuid.clear();
        prefs->enableVsync=true; prefs->enableVrr=true;
        engine.rootContext()->setContextProperty("initialView",QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks",false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(),root);
        navigation->notifyWindowFocus(true);
        QMetaObject::invokeMethod(navigation.get(),"onPollingTimerFired");
        const auto press=[&](Uint8 code) { button(SDL_CONTROLLERBUTTONDOWN,code); button(SDL_CONTROLLERBUTTONUP,code); };
        const auto savedUiMode=prefs->uiDisplayMode;
        prefs->uiDisplayMode=StreamingPreferences::UI_FULLSCREEN;
        press(SDL_CONTROLLER_BUTTON_START);
        auto* rail=root->findChild<QQuickItem*>("settingsCategories");
        QTRY_VERIFY(rail && rail->isVisible());
        auto* page=rail->parentItem(); QVERIFY(page);
        auto* uiDisplayChoice=root->findChild<QObject*>("uiDisplayModeChoice"); QVERIFY(uiDisplayChoice);
        QCOMPARE(uiDisplayChoice->property("currentIndex").toInt(),2);
        QCOMPARE(prefs->uiDisplayMode,StreamingPreferences::UI_FULLSCREEN);
        auto* pane=root->findChild<QQuickItem*>("settingsContent"); QVERIFY(pane);
        const QString capture=qEnvironmentVariable("CONSOLE_UI_CAPTURE_DIR");
        const auto captureState=[&](const QString& name) { if(!capture.isEmpty()) { QTest::qWait(120); root->grabWindow().save(capture+"/"+name+".png"); } };
        QTRY_VERIFY(rail->hasActiveFocus());
        QVERIFY(!navigation->currentUiNavMode());
        for(int i=0;i<3;++i) press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QCOMPARE(page->property("category").toInt(),3);
        press(SDL_CONTROLLER_BUTTON_DPAD_UP);
        QCOMPARE(page->property("category").toInt(),2);
        for(int i=0;i<4;++i) press(SDL_CONTROLLER_BUTTON_DPAD_UP);
        QCOMPARE(page->property("category").toInt(),0);
        // The actual virtual-stick path must agree with the D-pad path.
        const auto stick=[&](Sint16 value) {
            SDL_Delay(160);
            QCOMPARE(SDL_JoystickSetVirtualAxis(virtualControllers.first(),SDL_CONTROLLER_AXIS_LEFTY,value),0);
            QMetaObject::invokeMethod(navigation.get(),"onPollingTimerFired");
            QCOMPARE(SDL_JoystickSetVirtualAxis(virtualControllers.first(),SDL_CONTROLLER_AXIS_LEFTY,0),0);
            QMetaObject::invokeMethod(navigation.get(),"onPollingTimerFired");
        };
        stick(32767); QCOMPARE(page->property("category").toInt(),1);
        stick(-32767); QCOMPARE(page->property("category").toInt(),0);
        auto snapshot=[&]() {
            QVariantList result;
            for(auto key:{"width","height","fps","bitrateKbps","videoCodecConfig","autoAdjustBitrate","vrrBufferPerMille","vrrTargetHundredths","vrrHistorySeconds","vrrToleranceUs"}) result.append(prefs->property(key));
            return result;
        };
        const auto initial=snapshot();
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        QCOMPARE(root->activeFocusItem()->property("settingsLabel").toString(),QString("Resolution"));
        auto* resolutionRow=root->activeFocusItem();
        auto* resolution=resolutionRow->property("editor").value<QObject*>(); QVERIFY(resolution);
        auto* choices=resolution->property("popup").value<QObject*>(); QVERIFY(choices);
        choices->setProperty("enter",QVariant::fromValue<QObject*>(nullptr));
        choices->setProperty("exit",QVariant::fromValue<QObject*>(nullptr));
        captureState("browse-resolution");
        press(SDL_CONTROLLER_BUTTON_A); QTRY_VERIFY(choices->property("visible").toBool());
        const int selected=resolution->property("currentIndex").toInt();
        const int highlighted=resolution->property("highlightedIndex").toInt();
        for(auto code:{SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_X}) {
            press(code);
            QVERIFY(choices->property("visible").toBool());
            QVERIFY(rail->isVisible() && !rail->hasActiveFocus());
            QCOMPARE(resolution->property("currentIndex").toInt(),selected);
        }
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QCOMPARE(resolution->property("highlightedIndex").toInt(),highlighted+1);
        captureState("choice-editor");
        press(SDL_CONTROLLER_BUTTON_B); QTRY_VERIFY(!choices->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(),resolutionRow);
        QCOMPARE(resolution->property("currentIndex").toInt(),selected);
        QCOMPARE(snapshot(),initial);
        // Opening a choice while a direction is held must stop that hold at
        // the context boundary, even though both contexts use arrow keys.
        for(bool useStick : {false,true}) {
            resolutionRow->forceActiveFocus();
            if(useStick) {
                SDL_Delay(160);
                QCOMPARE(SDL_JoystickSetVirtualAxis(virtualControllers.first(),SDL_CONTROLLER_AXIS_LEFTY,32767),0);
                QMetaObject::invokeMethod(navigation.get(),"onPollingTimerFired");
            } else button(SDL_CONTROLLERBUTTONDOWN,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            auto* heldRow=root->activeFocusItem();
            QCOMPARE(heldRow->property("settingsLabel").toString(),QString("Frame rate"));
            auto* heldControl=heldRow->property("editor").value<QObject*>(); QVERIFY(heldControl);
            auto* heldPopup=heldControl->property("popup").value<QObject*>(); QVERIFY(heldPopup);
            press(SDL_CONTROLLER_BUTTON_A); QTRY_VERIFY(heldPopup->property("visible").toBool());
            const int heldHighlight=heldControl->property("highlightedIndex").toInt();
            QTest::qWait(500);
            QCOMPARE(heldControl->property("highlightedIndex").toInt(),heldHighlight);
            if(useStick) {
                QCOMPARE(SDL_JoystickSetVirtualAxis(virtualControllers.first(),SDL_CONTROLLER_AXIS_LEFTY,0),0);
                QMetaObject::invokeMethod(navigation.get(),"onPollingTimerFired");
            } else button(SDL_CONTROLLERBUTTONUP,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            press(SDL_CONTROLLER_BUTTON_B); QTRY_VERIFY(!heldPopup->property("visible").toBool());
            QTRY_COMPARE(root->activeFocusItem(),heldRow);
        }
        resolutionRow->forceActiveFocus();
        // Commit a real choice, then restore it through the same editor.
        press(SDL_CONTROLLER_BUTTON_A); QTRY_VERIFY(choices->property("visible").toBool());
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN); press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!choices->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(),resolutionRow);
        QCOMPARE(resolution->property("currentIndex").toInt(),selected+1);
        press(SDL_CONTROLLER_BUTTON_A); QTRY_VERIFY(choices->property("visible").toBool());
        press(SDL_CONTROLLER_BUTTON_DPAD_UP); press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!choices->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(),resolutionRow);
        QCOMPARE(resolution->property("currentIndex").toInt(),selected);
        QCOMPARE(snapshot(),initial);
        const QStringList expected={"Resolution","Frame rate","Video codec","Enable YUV 4:4:4","Video bitrate"};
        for(int i=1;i<expected.size();++i) {
            press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            QCOMPARE(root->activeFocusItem()->property("settingsLabel").toString(),expected[i]);
        }
        press(SDL_CONTROLLER_BUTTON_DPAD_UP);
        QCOMPARE(root->activeFocusItem()->property("settingsLabel").toString(),QString("Enable YUV 4:4:4"));
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        auto* bitrateRow=root->activeFocusItem();
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT); QCOMPARE(root->activeFocusItem(),bitrateRow);
        QCOMPARE(snapshot(),initial);
        press(SDL_CONTROLLER_BUTTON_A);
        auto* edit=root->findChild<QObject*>("settingsValueEditor"); QVERIFY(edit);
        QTRY_VERIFY(edit->property("visible").toBool());
        const double oldValue=edit->property("pendingValue").toDouble();
        captureState("range-editor");
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        QVERIFY(edit->property("pendingValue").toDouble()>oldValue);
        QCOMPARE(snapshot(),initial); // Staged editing must not update preferences.
        press(SDL_CONTROLLER_BUTTON_B);
        QTRY_VERIFY(!edit->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(),bitrateRow);
        QCOMPARE(snapshot(),initial);
        press(SDL_CONTROLLER_BUTTON_DPAD_LEFT); QVERIFY(rail->hasActiveFocus());
        // Remember the selected row on re-entry, without changing a value.
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT); QCOMPARE(root->activeFocusItem(),bitrateRow);
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(edit->property("visible").toBool());
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        const int committed=edit->property("pendingValue").toInt();
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!edit->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(),bitrateRow);
        QCOMPARE(prefs->bitrateKbps,committed);
        auto* bitrateEditor=bitrateRow->property("editor").value<QObject*>(); QVERIFY(bitrateEditor);
        QCOMPARE(bitrateEditor->property("value").toInt(),committed);
        press(SDL_CONTROLLER_BUTTON_A); QTRY_VERIFY(edit->property("visible").toBool());
        QCOMPARE(edit->property("pendingValue").toInt(),committed);
        press(SDL_CONTROLLER_BUTTON_B); QTRY_VERIFY(!edit->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(),bitrateRow);
        QVERIFY(!prefs->autoAdjustBitrate);
        // Visit every category using real controller events. Check visible
        // movement and exact reversal rather than reusing production's ordering.
        for(const auto size : {QSize(1280,720),QSize(800,500)}) {
        root->resize(size); QTest::qWait(30);
        for(int category=0;category<8;++category) {
            rail->forceActiveFocus(); page->setProperty("category",category); QTest::qWait(30);
            press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            for(int limit=0;limit<100;++limit) {
                auto* previous=root->activeFocusItem(); press(SDL_CONTROLLER_BUTTON_DPAD_UP);
                if(root->activeFocusItem()==previous) break;
            }
            QList<QQuickItem*> visited;
            for(int limit=0;limit<100;++limit) {
                auto* current=root->activeFocusItem(); QVERIFY(current && current!=rail);
                QVERIFY(current->isVisible() && current->isEnabled());
                QVERIFY2(!current->property("settingsLabel").toString().isEmpty(),"A settings row must have a visible label");
                const auto pos=current->mapToItem(pane,QPointF());
                QVERIFY2(pos.y()>=-1 && pos.y()+current->height()<=pane->height()+1,qPrintable(QString("category %1 focus outside scroll pane at %2, height %3 in %4").arg(category).arg(pos.y()).arg(current->height()).arg(pane->height())));
                if(!visited.isEmpty()) QVERIFY(current->mapToScene(QPointF()).y()>=visited.last()->mapToScene(QPointF()).y());
                visited.append(current);
                const auto before=snapshot();
                press(SDL_CONTROLLER_BUTTON_DPAD_LEFT); QVERIFY(rail->hasActiveFocus()); QCOMPARE(snapshot(),before);
                press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT); QCOMPARE(root->activeFocusItem(),current);
                press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
                if(root->activeFocusItem()==current) break;
                QVERIFY(!visited.contains(root->activeFocusItem()));
            }
            QVERIFY(!visited.isEmpty());
            captureState(QString("category-%1-bottom-%2").arg(category).arg(size.width()));
            for(int i=visited.size()-2;i>=0;--i) { press(SDL_CONTROLLER_BUTTON_DPAD_UP); QCOMPARE(root->activeFocusItem(),visited[i]); }
            press(SDL_CONTROLLER_BUTTON_DPAD_UP); QCOMPARE(root->activeFocusItem(),visited.first());
            QCOMPARE(pane->property("contentY").toDouble(),0.0);
            captureState(QString("category-%1-top-%2").arg(category).arg(size.width()));
        }
        }
        page->setProperty("category",0); rail->forceActiveFocus(); QTest::qWait(30);
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        auto* number=root->findChild<QQuickItem*>("vrrBuffer"); QVERIFY(number);
        auto* numberRow=number->property("settingsRowHost").value<QQuickItem*>(); QVERIFY(numberRow);
        numberRow->forceActiveFocus();
        const int bufferBefore=prefs->property("vrrBufferPerMille").toInt();
        press(SDL_CONTROLLER_BUTTON_A); QTRY_VERIFY(edit->property("visible").toBool());
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        const int bufferCandidate=edit->property("pendingValue").toInt();
        QVERIFY(bufferCandidate>bufferBefore);
        captureState("number-editor");
        press(SDL_CONTROLLER_BUTTON_B); QTRY_VERIFY(!edit->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(),numberRow);
        QCOMPARE(prefs->property("vrrBufferPerMille").toInt(),bufferBefore);
        press(SDL_CONTROLLER_BUTTON_A); QTRY_VERIFY(edit->property("visible").toBool());
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT); press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!edit->property("visible").toBool());
        QCOMPARE(prefs->property("vrrBufferPerMille").toInt(),bufferCandidate);
        prefs->setProperty("vrrBufferPerMille",bufferBefore);
        QTRY_COMPARE(number->property("value").toInt(),bufferBefore); // Binding survives a commit.
        QTRY_COMPARE(root->activeFocusItem(),numberRow);
        const auto numberClick=number->mapToScene(QPointF(number->width()/2,number->height()/2)).toPoint();
        QTest::mouseClick(root,Qt::LeftButton,Qt::NoModifier,numberClick);
        QTRY_VERIFY(edit->property("visible").toBool());
        auto* numberInput=root->findChild<QQuickItem*>("settingsNumberInput"); QVERIFY(numberInput);
        QTRY_VERIFY(numberInput->hasActiveFocus());
        QTest::keyClick(root,Qt::Key_A,Qt::ControlModifier);
        QTest::keyClick(root,Qt::Key_Backspace); QTest::keyClick(root,Qt::Key_Return);
        QVERIFY(edit->property("visible").toBool());
        QCOMPARE(prefs->property("vrrBufferPerMille").toInt(),bufferBefore);
        QTest::keyClick(root,Qt::Key_1); QTest::keyClick(root,Qt::Key_Period);
        QTest::keyClick(root,Qt::Key_5); QTest::keyClick(root,Qt::Key_0);
        QCOMPARE(prefs->property("vrrBufferPerMille").toInt(),bufferBefore);
        QTest::keyClick(root,Qt::Key_Return);
        QTRY_VERIFY(!edit->property("visible").toBool());
        QCOMPARE(prefs->property("vrrBufferPerMille").toInt(),1500);
        prefs->setProperty("vrrBufferPerMille",bufferBefore);
        QTRY_COMPARE(number->property("value").toInt(),bufferBefore);
        QTRY_COMPARE(root->activeFocusItem(),numberRow);
        QTest::keyClick(root,Qt::Key_Return);
        QTRY_VERIFY(edit->property("visible").toBool());
        QTRY_VERIFY(numberInput->hasActiveFocus());
        QTest::keyClick(root,Qt::Key_Escape);
        QTRY_VERIFY(!edit->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(),numberRow);
        press(SDL_CONTROLLER_BUTTON_X);
        auto* help=root->findChild<QObject*>("settingsHelpDialog"); QVERIFY(help);
        QTRY_VERIFY(help->property("visible").toBool());
        press(SDL_CONTROLLER_BUTTON_B); QTRY_VERIFY(!help->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(),numberRow);
        QVERIFY(!navigation->currentUiNavMode());
        auto* resetRow=root->findChild<QQuickItem*>("resetBitrateRow"); QVERIFY(resetRow && resetRow->isVisible());
        resetRow->forceActiveFocus(); press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!resetRow->isVisible());
        QTRY_COMPARE(root->activeFocusItem()->property("settingsLabel").toString(),QString("V-Sync"));
        auto* vsyncRow=root->activeFocusItem();
        press(SDL_CONTROLLER_BUTTON_A); QVERIFY(!prefs->enableVsync);
        auto* disabledPacing=root->findChild<QQuickItem*>("framePacingRow"); QVERIFY(disabledPacing);
        QVERIFY(!disabledPacing->isEnabled());
        QCOMPARE(disabledPacing->property("settingsNavigation").value<QObject*>(),page);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QVERIFY(root->activeFocusItem()->isEnabled());
        QCOMPARE(root->activeFocusItem()->property("settingsLabel").toString(), QString("Frame pacing"));
        QVERIFY(root->activeFocusItem()->property("disabledSettingProxy").toBool());
        const auto disabledSnapshot = snapshot();
        press(SDL_CONTROLLER_BUTTON_A);
        auto* disabledHelp = root->findChild<QObject*>("settingsHelpDialog"); QVERIFY(disabledHelp);
        QTRY_VERIFY(disabledHelp->property("visible").toBool());
        QVERIFY(!disabledHelp->property("text").toString().isEmpty());
        press(SDL_CONTROLLER_BUTTON_B); QTRY_VERIFY(!disabledHelp->property("visible").toBool());
        QCOMPARE(snapshot(), disabledSnapshot);
        press(SDL_CONTROLLER_BUTTON_DPAD_UP); QCOMPARE(root->activeFocusItem(),vsyncRow);
        press(SDL_CONTROLLER_BUTTON_A); QVERIFY(prefs->enableVsync);
        page->setProperty("category",4); rail->forceActiveFocus(); QTest::qWait(30);
        auto* motion=root->findChild<QQuickItem*>("reduceMotionRow"); QVERIFY(motion && motion->isVisible());
        const bool previousMotion=prefs->uiReducedMotion;
        motion->forceActiveFocus(); press(SDL_CONTROLLER_BUTTON_A);
        QCOMPARE(prefs->uiReducedMotion,!previousMotion);
        press(SDL_CONTROLLER_BUTTON_A); QCOMPARE(prefs->uiReducedMotion,previousMotion);
        // Hold/release and swap-aware Back stay on the actual shell path.
        rail->forceActiveFocus(); page->setProperty("category",0);
        button(SDL_CONTROLLERBUTTONDOWN,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTest::qWait(1000);
        button(SDL_CONTROLLERBUTTONUP,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        const int releasedCategory=page->property("category").toInt();
        QVERIFY(releasedCategory>0 && releasedCategory<=7);
        QTest::qWait(200); QCOMPARE(page->property("category").toInt(),releasedCategory);
        prefs->swapFaceButtons=true;
        press(SDL_CONTROLLER_BUTTON_B); QVERIFY(!rail->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A); QVERIFY(rail->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A); QTRY_VERIFY(!rail->isVisible());
        prefs->swapFaceButtons=false;
        prefs->uiDisplayMode=savedUiMode;
        navigation->notifyWindowFocus(false);
    }
    void emptyHostSetupWithControllerNavigation() {
        ConsoleHostModel::hostCount=0;
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        prefs->uiLastHostUuid.clear();
        engine.rootContext()->setContextProperty("initialView",QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks",false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        auto* add=root->findChild<QQuickItem*>("addPcTile"); QVERIFY(add);
        QTRY_VERIFY(add->hasActiveFocus());
        root->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        navigation->notifyWindowFocus(true);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        add->forceActiveFocus();
        auto* label = add->property("contentItem").value<QObject*>(); QVERIFY(label);
        auto* background = add->property("background").value<QObject*>(); QVERIFY(background);
        QVERIFY2(label->property("color") != background->property("color"), "Focused Add PC label must remain readable");
        QTest::keyClick(root, Qt::Key_Menu);
        QVERIFY(add->isVisible());
        QVERIFY(!root->findChild<QQuickItem*>("settingsCategories"));
        QTest::qWait(60);
        const QString capture=qEnvironmentVariable("CONSOLE_UI_CAPTURE_DIR");
        if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+"/empty-pcs.png"));
        QTest::keyClick(root,Qt::Key_Return);
        auto* address=root->findChild<QObject*>("hostAddress"); QVERIFY(address);
        QTRY_VERIFY(address->property("visible").toBool());
        QTest::keyClick(root,Qt::Key_Escape);
        ConsoleHostModel::hostCount=2;
    }
    void controllerFullscreenCheckboxUsesConfirm_data() {
        QTest::addColumn<bool>("swapped");
        QTest::newRow("normal") << false;
        QTest::newRow("swapped") << true;
    }
    void controllerFullscreenCheckboxUsesConfirm() {
        QFETCH(bool, swapped);
        prefs->swapFaceButtons = swapped;
        prefs->uiLastHostUuid.clear();
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView",QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks",false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(),root);
        navigation->notifyWindowFocus(true);
        const auto press=[&](Uint8 code) { button(SDL_CONTROLLERBUTTONDOWN,code); button(SDL_CONTROLLERBUTTONUP,code); };
        press(SDL_CONTROLLER_BUTTON_START);
        auto* rail=root->findChild<QQuickItem*>("settingsCategories"); QTRY_VERIFY(rail && rail->isVisible());
        auto* page=rail->parentItem(); page->setProperty("category",4); QTest::qWait(50);
        auto* check=root->findChild<QQuickItem*>("controllerFullscreenCheck"); QVERIFY(check && check->isVisible());
        check->forceActiveFocus();
        const auto confirm=swapped ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
        press(confirm); QTRY_VERIFY(prefs->uiControllerFullscreen);
        QCOMPARE(check->property("checked").toBool(),true);
        prefs->save(); prefs->reload(); QVERIFY(prefs->uiControllerFullscreen);
        press(confirm); QTRY_VERIFY(!prefs->uiControllerFullscreen);
        QCOMPARE(check->property("checked").toBool(),false);
        prefs->save(); prefs->reload(); QVERIFY(!prefs->uiControllerFullscreen);
        prefs->setProperty("uiControllerFullscreen",true); QTRY_VERIFY(check->property("checked").toBool());
        prefs->setProperty("uiControllerFullscreen",false); QTRY_VERIFY(!check->property("checked").toBool());
        const QString capture=qEnvironmentVariable("CONSOLE_UI_CAPTURE_DIR");
        if(!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+"/interface-fullscreen.png"));
        navigation->notifyWindowFocus(false);
    }
    void emptyLibraryHasActionsAndRecoversAfterLoading() {
        prefs->uiLastHostUuid.clear();
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView",QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks",false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(),root);
        navigation->notifyWindowFocus(true);
        const auto press=[&](Uint8 code) { button(SDL_CONTROLLERBUTTONDOWN,code); button(SDL_CONTROLLERBUTTONUP,code); };
        auto* add=root->findChild<QQuickItem*>("addPcTile"); QVERIFY(add); add->forceActiveFocus();
        press(SDL_CONTROLLER_BUTTON_DPAD_LEFT); press(SDL_CONTROLLER_BUTTON_A);
        auto* shelf=root->findChild<QQuickItem*>("gameShelf"); QTRY_VERIFY(shelf && shelf->isVisible());
        auto* page=shelf->parentItem(); while(page && !page->property("appModel").isValid()) page=page->parentItem(); QVERIFY(page);
        auto* model=qobject_cast<ConsoleAppModel*>(page->property("appModel").value<QObject*>()); QVERIFY(model);
        model->setProperty("appListLoading",true);
        for(int id : QList<int>(model->ids)) model->removeApp(id);
        auto* choose=root->findChild<QQuickItem*>("emptyLibraryChoosePc"); QVERIFY(choose);
        QTRY_VERIFY(choose->hasActiveFocus());
        QCOMPARE(page->property("primaryVerb").toString(),QString("Choose PC"));
        auto* title=root->findChild<QObject*>("emptyLibraryTitle"); QVERIFY(title);
        QTRY_COMPARE(title->property("text").toString(),QString::fromUtf8("Loading games…"));
        const QString capture=qEnvironmentVariable("CONSOLE_UI_CAPTURE_DIR");
        const auto snap=[&](const QString& name) { if(!capture.isEmpty()) { QTest::qWait(50); QVERIFY(root->grabWindow().save(capture+"/"+name+".png")); } };
        snap("loading-games");
        model->setProperty("appListLoading",false);
        QTRY_COMPARE(title->property("text").toString(),QString("No games available"));
        for(const auto size : {QSize(800,500),QSize(1280,720)}) { root->resize(size); QTest::qWait(60); snap(QString("empty-library-%1").arg(size.width())); }
        model->setProperty("appListUnavailable",true);
        QTRY_COMPARE(title->property("text").toString(),QString("Games unavailable"));
        snap("games-unavailable");
        model->setProperty("appListUnavailable",false);
        model->insertApp(2); QTRY_VERIFY(shelf->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_X); QTRY_VERIFY(page->property("detailsExpanded").toBool());
        press(SDL_CONTROLLER_BUTTON_X); QTRY_VERIFY(!page->property("detailsExpanded").toBool());
        QTRY_VERIFY(shelf->hasActiveFocus());
        model->removeApp(2); QTRY_VERIFY(choose->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A); QTRY_VERIFY(!choose->isVisible());
        QVERIFY(root->findChild<QQuickItem*>("addPcTile")->isVisible());
        navigation->notifyWindowFocus(false);
    }
    void desktopLaunchWithoutControllerAndControllerHotplug() {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        // Exercise real SDL device discovery rather than faking input-method
        // labels: no device means the usual desktop startup, arrival enters FSE.
        for (int i = SDL_NumJoysticks() - 1; i >= 0; --i)
            if (SDL_JoystickIsVirtual(i)) QCOMPARE(SDL_JoystickDetachVirtual(i), 0);
        for (auto* joystick : virtualControllers) SDL_JoystickClose(joystick);
        virtualControllers.clear();
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        QCOMPARE(navigation->connectedGamepads(), 0);
        prefs->uiControllerFullscreen = true;
        prefs->uiDisplayMode = StreamingPreferences::UI_WINDOWED;
        prefs->uiLastHostUuid.clear();
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        QTRY_COMPARE(root->visibility(), QWindow::Windowed);
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QCOMPARE(root->visibility(), QWindow::Windowed);
        for (int i = 0; i < 2; ++i) {
            const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
            QVERIFY(index >= 0);
            auto* joystick = SDL_JoystickOpen(index); QVERIFY(joystick);
            virtualControllers.append(joystick);
        }
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        QTRY_COMPARE(navigation->connectedGamepads(), 2);
        QTRY_COMPARE(root->visibility(), QWindow::FullScreen);
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QTest::qWait(20);
        QCOMPARE(root->visibility(), QWindow::FullScreen);
        navigation->notifyWindowFocus(false);
#endif
    }
    void cleanupTestCase() {
        for (auto* joystick : virtualControllers) SDL_JoystickClose(joystick);
        navigation.reset();
        SDL_Quit();
    }
};
QTEST_MAIN(ControllerNavigationTest)
#include "tst_controllernavigation.moc"
