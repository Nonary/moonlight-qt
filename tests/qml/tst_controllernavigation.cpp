#include "gui/sdlgamepadkeynavigation.h"
#include "consolefixtures.h"
#include "gui/recentappsmodel.h"
#include "settings/mappingmanager.h"
#include <QAccessible>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlIncubator>
#include <QPointer>
#include <QScopedValueRollback>
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

class MutableConsoleHostModel : public ConsoleHostModel {
    Q_OBJECT
public:
    using ConsoleHostModel::ConsoleHostModel;
    bool reversed = false;
    Q_INVOKABLE QVariantMap get(int row) const {
        return ConsoleHostModel::get(reversed && row >= 0 && row < hostCount ? hostCount - row - 1 : row);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        return get(index.row()).value(QString::fromUtf8(roleNames().value(role)));
    }
    void reverseHosts() {
        beginResetModel();
        reversed = !reversed;
        endResetModel();
    }
    void resetHostCount(int count) {
        beginResetModel();
        hostCount = count;
        endResetModel();
    }
};

class ControllerNavigationTest : public QObject {
    Q_OBJECT
    QTemporaryDir config;
    KeyWindow window;
    StreamingPreferences* prefs = nullptr;
    std::unique_ptr<SdlGamepadKeyNavigation> navigation;
    QList<SDL_Joystick*> virtualControllers;
    static QQuickItem* visualItem(QQuickItem* item, const QString& name) {
        if (item->objectName() == name) return item;
        for (auto* child : item->childItems())
            if (auto* result = visualItem(child, name)) return result;
        return nullptr;
    }
    static void polishItems(QQuickItem* item) {
        for (auto* child : item->childItems()) polishItems(child);
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
        item->ensurePolished();
#endif
    }
    static void settleLayout(QQmlApplicationEngine& engine, QQuickWindow* root) {
        // Synthetic keys can outrun asynchronous Loader creation and Column
        // polish. Finish those before checking the visible navigation order.
        if (auto* controller = engine.incubationController()) controller->incubateFor(50);
        QCoreApplication::processEvents();
        polishItems(root->contentItem());
        QCoreApplication::processEvents();
        polishItems(root->contentItem());
    }
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
        qmlRegisterType<MutableConsoleHostModel>("ComputerModel",1,0,"ComputerModel");
        qmlRegisterType<ConsoleAppModel>("AppModel",1,0,"AppModel");
        qmlRegisterType<RecentAppsModel>("AppModel",1,0,"RecentAppsModel");
        qmlRegisterUncreatableType<ConsoleSession>("Session", 1, 0, "Session", "Created by the fixture app model");
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
        // sdl2-compat also uses SDL3's gamepad filter; allow only the
        // zero-VID/PID virtual fixtures in this isolated test process.
        SDL_SetHint("SDL_GAMEPAD_IGNORE_DEVICES_EXCEPT", "0x0000/0x0000");
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x0000/0x0000");
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
        window.show(); window.requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), &window);
        navigation->notifyWindowFocus(false);
        navigation->notifyWindowFocus(true);
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        prefs->swapFaceButtons = false;
        // Most shell cases verify a specific desktop geometry. Fullscreen's
        // default and explicit-exit contract have their own window tests below.
        prefs->uiControllerFullscreen = false;
        window.keys.clear();
        window.onPress = {};
    }
    void consoleBackdropLoadsWithOptionalBlur() {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/gui/ConsoleBackdrop.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QQuickWindow backdropWindow;
        std::unique_ptr<QObject> object(component.create());
        auto* backdrop = qobject_cast<QQuickItem*>(object.get());
        QVERIFY2(backdrop != nullptr, qPrintable(component.errorString()));
        backdropWindow.resize(64, 64);
        backdrop->setSize(QSizeF(64, 64));
        backdrop->setParentItem(backdropWindow.contentItem());
        backdropWindow.show();
        auto* loader = backdrop->findChild<QObject*>(QStringLiteral("consoleBackdropBlur"));
        QVERIFY(loader);
        const auto blur = [loader] { return loader->property("item").value<QObject*>(); };
        QVERIFY(!blur());

        const QUrl artwork(QStringLiteral("qrc:/res/moonlight.svg"));
        QVERIFY(backdrop->setProperty("artwork", artwork));
        QTRY_COMPARE(backdrop->property("settledArtwork").toUrl(), artwork);

        const bool sceneGraphHasEffects = backdropWindow.rendererInterface()->graphicsApi() != QSGRendererInterface::Software;
        QTRY_COMPARE(backdrop->property("effectsAvailable").toBool(), sceneGraphHasEffects);
        const bool canBlur = QT_VERSION >= QT_VERSION_CHECK(6, 5, 0) && sceneGraphHasEffects;
        if (canBlur) {
            QTRY_VERIFY(blur());
            QCOMPARE(blur()->property("artwork").toUrl(), artwork);
        } else {
            QVERIFY(!blur());
        }

        QVERIFY(backdrop->setProperty("artwork", QUrl()));
        QTRY_VERIFY(backdrop->property("settledArtwork").toUrl().isEmpty());
        QVERIFY(!blur());
    }
    void launchPresentation_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<int>("displayMode");
        QTest::addColumn<bool>("controllerFullscreen");
        QTest::addColumn<int>("appId");
        QTest::addColumn<int>("runningAppId");
        QTest::newRow("deck") << QSize(1280, 800) << int(StreamingPreferences::UI_WINDOWED) << true << 1 << 0;
        QTest::newRow("docked-resume") << QSize(1920, 1080) << int(StreamingPreferences::UI_FULLSCREEN) << false << 2 << 2;
        QTest::newRow("no-artwork") << QSize(1280, 800) << int(StreamingPreferences::UI_FULLSCREEN) << false << 5 << 0;
        QTest::newRow("switch-game") << QSize(1280, 800) << int(StreamingPreferences::UI_FULLSCREEN) << false << 3 << 1;
        QTest::newRow("windowed") << QSize(960, 600) << int(StreamingPreferences::UI_WINDOWED) << false << 1 << 0;
        QTest::newRow("maximized") << QSize(1280, 800) << int(StreamingPreferences::UI_MAXIMIZED) << false << 1 << 0;
    }

    void launchPresentation() {
        QFETCH(QSize, size);
        QFETCH(int, displayMode);
        QFETCH(bool, controllerFullscreen);
        QFETCH(int, appId);
        QFETCH(int, runningAppId);
        prefs->uiDisplayMode = static_cast<StreamingPreferences::UIDisplayMode>(displayMode);
        prefs->uiControllerFullscreen = controllerFullscreen;
        prefs->uiLastHostUuid.clear();
        prefs->rememberUiSelection("host-one", appId);

        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(root);
        const bool fullscreen = controllerFullscreen || displayMode == StreamingPreferences::UI_FULLSCREEN;
        const auto visibility = fullscreen ? QWindow::FullScreen :
            displayMode == StreamingPreferences::UI_MAXIMIZED ? QWindow::Maximized : QWindow::Windowed;
        QTRY_COMPARE(root->visibility(), visibility);
        root->resize(size);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        navigation->notifyWindowFocus(true);
        QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
        const auto press = [&](Uint8 code) {
            button(SDL_CONTROLLERBUTTONDOWN, code);
            button(SDL_CONTROLLERBUTTONUP, code);
        };

        auto* add = root->findChild<QQuickItem*>("addPcTile");
        QVERIFY(add);
        add->forceActiveFocus();
        press(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        press(SDL_CONTROLLER_BUTTON_A);
        auto* shelf = root->findChild<QQuickItem*>("gameShelf");
        QTRY_VERIFY(shelf && shelf->isVisible());
        auto* page = shelf->parentItem();
        while (page && !page->property("appModel").isValid()) page = page->parentItem();
        QVERIFY(page);
        auto* model = qobject_cast<ConsoleAppModel*>(page->property("appModel").value<QObject*>());
        QVERIFY(model);
        auto* session = new ConsoleSession(model);
        model->launchSession = session;
        model->runningAppId = runningAppId;
        shelf->forceActiveFocus();
        shelf->setProperty("currentIndex", model->indexOfApp(appId));
        QTRY_COMPARE(page->property("selectedApp").toMap().value("appid").toInt(), appId);
        press(SDL_CONTROLLER_BUTTON_A);

        if (runningAppId && runningAppId != appId) {
            auto* modal = root->findChild<QObject*>("quitRunningGameDialog");
            QTRY_VERIFY(modal && modal->property("opened").toBool());
            QVERIFY(QMetaObject::invokeMethod(modal, "accept"));
            QQmlComponent managerLookup(&engine);
            managerLookup.setData(R"(
                import QtQml
                import ComputerManager 1.0
                QtObject { property QtObject manager: ComputerManager }
            )", QUrl());
            std::unique_ptr<QObject> lookup(managerLookup.create());
            QVERIFY2(lookup, qPrintable(managerLookup.errorString()));
            auto* manager = lookup->property("manager").value<QObject*>();
            QVERIFY(manager);
            QTest::qWait(150);
            QVERIFY(QMetaObject::invokeMethod(manager, "quitAppCompleted", Q_ARG(QVariant, QVariant())));
        }
        QTRY_VERIFY(session->started);
        QCOMPARE(root->visibility(), visibility);
        auto* compact = visualItem(root->contentItem(), "compactLaunchView");
        QVERIFY(compact);
        QCOMPARE(compact->isVisible(), !fullscreen);
        auto* view = visualItem(root->contentItem(), "fullscreenLaunchView");
        QCOMPARE(view != nullptr, fullscreen);

        if (fullscreen) {
            auto* artwork = visualItem(view, "launchArtwork");
            auto* title = visualItem(view, "launchTitle");
            auto* status = visualItem(view, "launchStatus");
            auto* progress = visualItem(view, "launchProgress");
            auto* hint = visualItem(view, "launchDisconnectHint");
            QVERIFY(artwork && title && status && progress && hint);
            QTRY_VERIFY(progress->property("running").toBool());
            QTRY_COMPARE(artwork->isVisible(), appId != 5);
            if (artwork->isVisible()) {
                const auto artHeight = artwork->property("paintedHeight").toReal();
                QVERIFY2(artHeight > qMin(view->height() * 0.5, view->width() * 0.35),
                         qPrintable(QString("Rendered artwork height %1 in launch view %2x%3")
                             .arg(artHeight).arg(view->width()).arg(view->height())));
                QVERIFY(artwork->mapToScene(QPointF(artwork->width(), 0)).x() < title->mapToScene({}).x());
            } else {
                QCOMPARE(title->mapToScene(QPointF(title->width() / 2, 0)).x(), view->width() / 2);
            }
            QCOMPARE(title->property("text").toString(), model->get(model->indexOfApp(appId)).value("name").toString());
            QCOMPARE(status->property("text").toString(), runningAppId == appId ? QStringLiteral("Resuming…") : QStringLiteral("Starting…"));
            QVERIFY(hint->property("text").toString().contains("Start+Select+L1+R1"));
            auto* accessibleTitle = QAccessible::queryAccessibleInterface(title);
            QVERIFY(accessibleTitle);
            QCOMPARE(accessibleTitle->role(), QAccessible::Heading);
            const QString capture = qEnvironmentVariable("MOONLIGHT_LAUNCH_CAPTURE_DIR");
            if (!capture.isEmpty()) {
                QCOMPARE(root->size(), size);
                QTest::qWait(250);
                QVERIFY(root->grabWindow().save(capture + "/moonlight-launch-" + QTest::currentDataTag() + ".png"));
            }
            emit session->stageStarting("RTSP handshake");
            QCOMPARE(status->property("text").toString(), QString("Starting RTSP handshake..."));
            auto* accessibleStatus = QAccessible::queryAccessibleInterface(status);
            QVERIFY(accessibleStatus);
            QCOMPARE(accessibleStatus->text(QAccessible::Name), status->property("text").toString());
            title->setProperty("text", QString(220, QChar('W')));
            emit session->stageStarting(QString("Negotiating a very long translated stage ").repeated(12));
            settleLayout(engine, root);
            QVERIFY(title->mapToScene(QPointF(0, title->height())).y() < status->mapToScene({}).y());
            QVERIFY(status->mapToScene(QPointF(0, status->height())).y() < hint->mapToScene({}).y());
        }

        if (appId == 1 && fullscreen) {
            emit session->stageFailed("RTSP handshake", 7, "TCP 48010");
            emit session->sessionFinished(0);
            emit session->readyForDeletion();
            auto* error = root->findChild<QObject*>("streamError");
            QTRY_VERIFY(error && error->property("opened").toBool());
            QVERIFY(error->property("text").toString().contains("TCP 48010"));
            root->requestActivate();
            QTRY_COMPARE(QGuiApplication::focusWindow(), root);
            navigation->notifyWindowFocus(true);
            QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
            press(SDL_CONTROLLER_BUTTON_B);
            QTRY_VERIFY(!error->property("visible").toBool());
        } else {
            emit session->connectionStarted();
            QTRY_VERIFY(!root->isVisible());
            QVERIFY(!visualItem(root->contentItem(), "fullscreenLaunchView"));
            QVERIFY(!compact->isVisible());
            emit session->sessionFinished(0);
            emit session->readyForDeletion();
            QTRY_VERIFY(root->isVisible());
        }
        QTRY_VERIFY(shelf->hasActiveFocus());
        prefs->uiDisplayMode = StreamingPreferences::UI_WINDOWED;
    }

    void controllerFamiliesAndPrompts() {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
        auto label=[root](const char* action) {
            QVariant result;
            QMetaObject::invokeMethod(root,"controllerLabel",Q_RETURN_ARG(QVariant,result),
                Q_ARG(QVariant,QVariant(QString::fromLatin1(action))));
            return result.toString();
        };
        // SDL virtual pads always report VIRTUAL in sdl2-compat; inject only
        // the detected type into the same classifier used by noteController.
        // Real SDL event discovery/switching is checked separately below.
        const auto select=[this](int type) {
            navigation->m_InputMethod="controller";
            navigation->m_ControllerFamily=navigation->familyForControllerType(type);
            emit navigation->navigationStateChanged();
        };
        for (int type:{int(SDL_CONTROLLER_TYPE_XBOX360),int(SDL_CONTROLLER_TYPE_XBOXONE),
                      int(SDL_CONTROLLER_TYPE_UNKNOWN),int(SDL_CONTROLLER_TYPE_VIRTUAL)}) {
            select(type); QCOMPARE(navigation->controllerFamily(),QString("xbox"));
            for(bool swapped:{false,true}) {
                prefs->swapFaceButtons=swapped;
                QCOMPARE(label("confirm"),QString(swapped ? "B" : "A"));
                QCOMPARE(label("back"),QString(swapped ? "A" : "B"));
                QCOMPARE(label("options"),QString(swapped ? "Y" : "X"));
                QCOMPARE(label("done"),QString(swapped ? "X" : "Y"));
                QCOMPARE(label("sections"),QString("LB / RB"));
            }
        }
        for(int type:{int(SDL_CONTROLLER_TYPE_PS3),int(SDL_CONTROLLER_TYPE_PS4),int(SDL_CONTROLLER_TYPE_PS5)}) {
            select(type); QCOMPARE(navigation->controllerFamily(),QString("playstation"));
            for(bool swapped:{false,true}) {
                prefs->swapFaceButtons=swapped;
                QCOMPARE(label("confirm"),QString::fromUtf8(swapped ? "○" : "×"));
                QCOMPARE(label("back"),QString::fromUtf8(swapped ? "×" : "○"));
                QCOMPARE(label("options"),QString::fromUtf8(swapped ? "△" : "□"));
                QCOMPARE(label("done"),QString::fromUtf8(swapped ? "□" : "△"));
                QCOMPARE(label("sections"),QString("L1 / R1"));
            }
        }
        select(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO);
        QCOMPARE(label("sections"),QString("L / R"));
        prefs->swapFaceButtons=false;
        QCOMPARE(label("confirm"),QString("B"));
        select(SDL_CONTROLLER_TYPE_PS5);
        // Actual SDL input from an unknown/virtual pad replaces the PS family.
        button(SDL_CONTROLLERBUTTONDOWN,SDL_CONTROLLER_BUTTON_LEFTSTICK,1);
        button(SDL_CONTROLLERBUTTONUP,SDL_CONTROLLER_BUTTON_LEFTSTICK,1);
        QCOMPARE(navigation->controllerFamily(),QString("xbox"));
        QCOMPARE(label("confirm"),QString("A"));
        select(SDL_CONTROLLER_TYPE_PS5);
        const int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX,SDL_CONTROLLER_BUTTON_MAX,0);
        QVERIFY(index>=0);
        QMetaObject::invokeMethod(navigation.get(),"onPollingTimerFired");
        QCOMPARE(navigation->controllerFamily(),QString("xbox"));
        QCOMPARE(label("confirm"),QString("A"));
        QCOMPARE(SDL_JoystickDetachVirtual(index),0);
        QMetaObject::invokeMethod(navigation.get(),"onPollingTimerFired");
#else
        QSKIP("Controller family lifecycle test requires SDL 2.0.14 virtual devices");
#endif
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
        navigation->notifyWindowFocus(true);
        QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
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
    void persistedHistoryNormalizesStringIdsAndDeduplicates() {
        const QString key = "ui/recentApps/restarted-history";
        // INI-loaded variants may be strings even when originally saved as ints.
        QSettings().setValue(key, QVariantList({"4", "2", "4", "bad", "0", "-1"}));
        QCOMPARE(prefs->recentUiApps("restarted-history"), QVariantList({4, 2}));
        prefs->recordUiPlay("restarted-history", 2);
        QCOMPARE(prefs->recentUiApps("restarted-history"), QVariantList({2, 4}));
        QCOMPARE(QSettings().value(key).toList(), QVariantList({2, 4}));
        ConsoleAppModel catalogue;
        RecentAppsModel recent;
        recent.setSourceModel(&catalogue);
        recent.setAppIds({"4", "2", "4", "bad", "0", "-1"});
        QCOMPARE(recent.rowCount(), 2);
        QCOMPARE(recent.get(0).value("appid").toInt(), 4);
        QCOMPARE(recent.get(1).value("appid").toInt(), 2);
        QSettings().remove(key);
    }
    void hostDiscoveryPreservesSettingsFocusAndStatusTracksSelection() {
        prefs->uiLastHostUuid = "host-two";
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        auto* grid = root->findChild<QQuickItem*>("Computers"); QVERIFY(grid);
        QTRY_VERIFY(!grid->property("startupRestorePending").toBool());
        QVERIFY(grid->property("rememberedStatus").toString().contains("Living Room PC"));
        grid->setProperty("currentIndex", 0);
        QTRY_VERIFY(grid->property("rememberedStatus").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(root, "openSettings", Q_ARG(QVariant, QVariant(0))));
        auto* rail = root->findChild<QQuickItem*>("settingsCategories");
        QTRY_VERIFY(rail && rail->isVisible());
        rail->forceActiveFocus();
        auto* model = qobject_cast<MutableConsoleHostModel*>(grid->property("computerModel").value<QObject*>()); QVERIFY(model);
        model->resetHostCount(1);
        QTest::qWait(60);
        QVERIFY(rail->hasActiveFocus());
        model->resetHostCount(2);
        QTest::qWait(60);
        QVERIFY(rail->hasActiveFocus());
        model->reverseHosts();
        QTRY_COMPARE(grid->property("selectedHost").toMap().value("name").toString(), QString("Living Room PC"));
        QVERIFY(rail->hasActiveFocus());
        model->reverseHosts();
        QTRY_COMPARE(grid->property("selectedHost").toMap().value("name").toString(), QString("Gaming PC"));
        prefs->uiLastHostUuid.clear();
    }
    void recentGameActionsFollowIdentityAfterCatalogueRowsMove() {
        QSettings().remove("ui/selectedApps/host-one");
        QSettings().remove("ui/recentApps/host-one");
        prefs->recordUiPlay("host-one", 4);
        QCOMPARE(prefs->recentUiApps("host-one"), QVariantList({QVariant(4)}));
        prefs->uiLastHostUuid.clear();
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(root);
        root->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        auto* add = root->findChild<QQuickItem*>("addPcTile"); QVERIFY(add);
        add->forceActiveFocus();
        QTest::keyClick(root, Qt::Key_Left);
        QTest::keyClick(root, Qt::Key_Return);
        QQuickItem* recent = nullptr;
        QTRY_VERIFY((recent = root->findChild<QQuickItem*>("recentGameShelf")));
        auto* page = recent->parentItem();
        while (page && !page->property("appModel").isValid()) page = page->parentItem();
        QVERIFY(page);
        auto* model = qobject_cast<ConsoleAppModel*>(page->property("appModel").value<QObject*>()); QVERIFY(model);
        QVERIFY(QMetaObject::invokeMethod(page, "focusShelf", Q_ARG(QVariant, QVariant(true))));
        QTRY_COMPARE(page->property("selectedApp").toMap().value("appid").toInt(), 4);
        // Removing an app absent from Recents shifts the source row without a
        // Recent selection change, so a bound invokable row lookup goes stale.
        model->removeApp(1);
        QTest::qWait(40);
        QCOMPARE(page->property("selectedApp").toMap().value("appid").toInt(), 4);
        QTest::keyClick(root, Qt::Key_Menu);
        auto* hide = root->findChild<QQuickItem*>("hideGameButton"); QVERIFY(hide);
        QTRY_VERIFY(hide->isVisible());
        QVERIFY(QMetaObject::invokeMethod(hide, "clicked"));
        QTRY_VERIFY(model->hiddenIds.contains(4));
        QVERIFY(!model->hiddenIds.contains(5));
        QSettings().remove("ui/recentApps/host-one");
        prefs->uiLastHostUuid.clear();
    }
    void pcOptionsUseTheSelectedTile() {
        prefs->uiLastHostUuid.clear();
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(root);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        navigation->notifyWindowFocus(true);
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        auto* pcs = root->findChild<QQuickItem*>("Computers");
        QVERIFY(pcs);
        const auto press = [&](Uint8 code) { button(SDL_CONTROLLERBUTTONDOWN, code); button(SDL_CONTROLLERBUTTONUP, code); };
        auto focusedText = [&]() { return root->activeFocusItem() ? root->activeFocusItem()->property("text").toString() : QString(); };
        press(SDL_CONTROLLER_BUTTON_X);
        QTRY_COMPARE(focusedText(), QString("View All Apps"));
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_COMPARE(focusedText(), QString("Test Network"));
        press(SDL_CONTROLLER_BUTTON_B);
        QTRY_VERIFY(pcs->hasActiveFocus());

        // Right-click the offline PC while the online PC is still selected.
        auto* tile = pcs->property("currentItem").value<QQuickItem*>();
        QVERIFY(tile);
        const QPointF secondTile(pcs->property("cellWidth").toReal() + 35, pcs->property("cardY").toReal() + 20);
        QTest::mouseClick(root, Qt::RightButton, Qt::NoModifier, tile->mapToScene(secondTile).toPoint());
        QTRY_COMPARE(pcs->property("currentIndex").toInt(), 1);
        QTRY_COMPARE(focusedText(), QString("Wake PC"));
        press(SDL_CONTROLLER_BUTTON_B);
        QTRY_VERIFY(pcs->hasActiveFocus());
        auto* options = visualItem(root->contentItem(), "footerHint_options");
        QVERIFY(options && options->isVisible());
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, options->mapToScene(options->boundingRect().center()).toPoint());
        QTRY_COMPARE(focusedText(), QString("Wake PC"));
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_COMPARE(focusedText(), QString("Rename PC"));
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(root->activeFocusItem() && root->activeFocusItem()->property("placeholderText").toString() == "Living Room PC");
        press(SDL_CONTROLLER_BUTTON_B);
    }
    void gameVideoOverridesFollowHostAndGameIdentity() {
        const auto defaults = prefs->videoSettingsForApp({}, 0);
        const QVariantMap overrides{
            {"resolution", QSize(2560, 1600)}, {"fps", 116}, {"bitrateKbps", 42500}, {"hdr", !defaults.hdr}
        };
        prefs->saveAppVideoOverrides("profile-host", 4, overrides);
        QSettings().sync();
        QCOMPARE(prefs->appVideoOverrides("profile-host", 4), overrides);

        const auto requested = prefs->videoSettingsForApp("profile-host", 4);
        QCOMPARE(requested.resolution, QSize(2560, 1600));
        QCOMPARE(requested.fps, 116);
        QCOMPARE(requested.bitrateKbps, 42500);
        QCOMPARE(requested.hdr, !defaults.hdr);
        QCOMPARE(prefs->videoSettingsForApp("profile-host", 5).fps, defaults.fps);
        QCOMPARE(prefs->videoSettingsForApp("other-host", 4).resolution, defaults.resolution);
        QCOMPARE(prefs->enableHdr, defaults.hdr);

        prefs->saveAppVideoOverrides("profile-host", 4, {{"resolution", QSize(0, 1600)}, {"hdr", false}});
        const auto partial = prefs->videoSettingsForApp("profile-host", 4);
        QCOMPARE(partial.resolution, defaults.resolution);
        QCOMPARE(partial.bitrateKbps, defaults.bitrateKbps);
        QVERIFY(!partial.hdr);
        prefs->saveAppVideoOverrides("profile-host", 4, {});
        QVERIFY(prefs->appVideoOverrides("profile-host", 4).isEmpty());
    }
    void gameStreamProfileOpenSaveAndCancel() {
        const auto lastHost = prefs->uiLastHostUuid;
        prefs->uiLastHostUuid.clear();
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(root);
        root->resize(800, 500);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        auto* add = root->findChild<QQuickItem*>("addPcTile");
        QVERIFY(add);
        add->forceActiveFocus();
        QTest::keyClick(root, Qt::Key_Left);
        QTest::keyClick(root, Qt::Key_Return);
        QQuickItem* shelf = nullptr;
        QTRY_VERIFY((shelf = root->findChild<QQuickItem*>("gameShelf")) && shelf->isVisible());

        navigation->notifyWindowFocus(true);
        QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
        const auto press = [&](Uint8 code) { button(SDL_CONTROLLERBUTTONDOWN, code); button(SDL_CONTROLLERBUTTONUP, code); };
        press(SDL_CONTROLLER_BUTTON_X);
        auto* entry = root->findChild<QQuickItem*>("gameStreamProfileButton");
        QVERIFY(entry);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_VERIFY(entry->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A);
        auto* dialog = root->findChild<QObject*>("gameStreamProfile");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto* resolution = dialog->findChild<QQuickItem*>("gameProfileResolution");
        auto* row = dialog->findChild<QQuickItem*>("gameProfileBitrateRow");
        auto* bitrate = dialog->findChild<QQuickItem*>("gameProfileBitrate");
        auto* editor = dialog->findChild<QObject*>("gameProfileBitrateEditor");
        auto* resetBitrate = dialog->findChild<QQuickItem*>("gameProfileResetBitrate");
        auto* hdr = dialog->findChild<QQuickItem*>("gameProfileHdr");
        auto* resetHdr = dialog->findChild<QQuickItem*>("gameProfileResetHdr");
        auto* save = dialog->findChild<QQuickItem*>("gameProfileSave");
        QVERIFY(resolution && row && bitrate && editor && resetBitrate && hdr && resetHdr && save);
        const int globalBitrate = prefs->bitrateKbps;
        const bool globalHdr = prefs->enableHdr;
        const auto host = dialog->property("hostUuid").toString();
        const int appId = dialog->property("appId").toInt();
        QVERIFY(!resetBitrate->isVisible() && !resetHdr->isVisible());
        QTRY_VERIFY(resolution->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_VERIFY(row->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(editor->property("opened").toBool());
        const int start = editor->property("pendingValue").toInt();
        const int step = bitrate->property("stepSize").toInt();
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        const int edited = std::min(start + step * 11, bitrate->property("to").toInt());
        QCOMPARE(editor->property("pendingValue").toInt(), edited);
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!editor->property("visible").toBool() && row->hasActiveFocus());
        QCOMPARE(bitrate->property("value").toInt(), edited);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_VERIFY(resetBitrate->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_VERIFY(hdr->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A);
        QCOMPARE(hdr->property("checked").toBool(), !globalHdr);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_VERIFY(resetHdr->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_VERIFY(save->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(prefs->appVideoOverrides(host, appId), (QVariantMap{{"bitrateKbps", edited}, {"hdr", !globalHdr}}));
        QTRY_VERIFY(entry->hasActiveFocus());

        // An old override above the current slider limit must survive opening
        // and canceling the editor, and saving edits to a different field.
        prefs->saveAppVideoOverrides(host, appId, {{"bitrateKbps", 725501}, {"hdr", !globalHdr}});
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(bitrate->property("valueText").toString().contains("725.501"));
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(editor->property("opened").toBool());
        press(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        press(SDL_CONTROLLER_BUTTON_B);
        QTRY_VERIFY(!editor->property("visible").toBool() && row->hasActiveFocus());
        QVERIFY(bitrate->property("valueText").toString().contains("725.501"));
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_VERIFY(resetHdr->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(hdr->hasActiveFocus() && !resetHdr->isVisible());
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(prefs->appVideoOverrides(host, appId), (QVariantMap{{"bitrateKbps", 725501}}));

        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(dialog->property("opened").toBool());
        for (int i = 0; i < 3; ++i) press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_VERIFY(resetBitrate->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(row->hasActiveFocus() && !resetBitrate->isVisible());
        QCOMPARE(bitrate->property("value").toInt(), globalBitrate);
        press(SDL_CONTROLLER_BUTTON_B);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(prefs->appVideoOverrides(host, appId), (QVariantMap{{"bitrateKbps", 725501}}));
        QCOMPARE(prefs->bitrateKbps, globalBitrate);
        QCOMPARE(prefs->enableHdr, globalHdr);
        prefs->saveAppVideoOverrides(host, appId, {});
        prefs->uiLastHostUuid = lastHost;
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
        press(SDL_CONTROLLER_BUTTON_START);
        auto* categories = root->findChild<QQuickItem*>("settingsCategories");
        QTRY_VERIFY(categories && categories->hasActiveFocus());
        for (int i = 0; i < 4; ++i) press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        auto* fullscreenButton = root->findChild<QQuickItem*>("uiFullscreenButton");
        QTRY_VERIFY(fullscreenButton && fullscreenButton->hasActiveFocus());
        QCOMPARE(fullscreenButton->property("text").toString(), QString("Exit fullscreen"));
        press(SDL_CONTROLLER_BUTTON_A);
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
        QVERIFY(root->property("toolbarFocused").toBool());
        auto* footerConfirm = visualItem(root->contentItem(), "footerHint_confirm"); QVERIFY(footerConfirm);
        bool selectHint = false;
        for (auto* item : footerConfirm->findChildren<QQuickItem*>())
            if (item->property("text").toString() == "Select") selectHint = true;
        QVERIFY(selectHint);
        if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture + "/toolbar-library.png"));
        const int toolbarSelection = selectedId();
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier,
            footerConfirm->mapToScene(QPointF(footerConfirm->width()/2, footerConfirm->height()/2)).toPoint());
        QTRY_VERIFY(recent->hasActiveFocus());
        QCOMPARE(selectedId(), toolbarSelection);
        // Emptying Recents skips that row and leaves the complete catalogue usable.
        model->setAppHidden(model->indexOfApp(1), true);
        model->setAppHidden(model->indexOfApp(2), true);
        QTRY_COMPARE(recent->property("count").toInt(), 0);
        all->forceActiveFocus();
        QTest::keyClick(root, Qt::Key_Down);
        QVERIFY(all->hasActiveFocus()); QVERIFY(!page->property("detailsExpanded").toBool());
        QSettings().remove("ui/recentApps/host-one");
    }
    void ultraLowLatencyFrameRateSetting_data() {
        QTest::addColumn<int>("limit");
        QTest::addColumn<int>("requestedFps");
        QTest::addColumn<int>("shownFps");
        QTest::newRow("qualified") << 116 << 120 << 116;
        QTest::newRow("fallback") << 100 << 120 << 100;
        QTest::newRow("explicit 116") << 116 << 116 << 116;
        QTest::newRow("explicit 100") << 116 << 100 << 100;
        QTest::newRow("explicit below fallback") << 100 << 90 << 90;
    }
    void ultraLowLatencyFrameRateSetting() {
        QFETCH(int, limit);
        QFETCH(int, requestedFps);
        QFETCH(int, shownFps);
        const int savedFps = prefs->fps;
        const bool savedVrr = prefs->enableVrr;
        const bool savedVsync = prefs->enableVsync;
        const bool savedUltraLowLatency = prefs->ultraLowLatencyVrr;
        prefs->setProperty("enableVsync", true);
        prefs->setProperty("enableVrr", true);
        prefs->setProperty("ultraLowLatencyVrr", false);
        prefs->setProperty("fps", requestedFps);

        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        const auto directory = QUrl::fromLocalFile(QStringLiteral(CONSOLE_GUI_PATH) + "/");
        const QByteArray wrapper = R"(
import QtQuick 2.9
import QtQuick.Controls 2.5
import SystemProperties 1.0
import "." as Views
ApplicationWindow {
    width: 1280
    height: 600
    property bool pollingActive: false
    property int probeLimit: 116
    Component.onCompleted: SystemProperties.mockUltraLowLatencyVrrFpsLimit = probeLimit
    onProbeLimitChanged: SystemProperties.mockUltraLowLatencyVrrFpsLimit = probeLimit
    Item { id: stackView; property bool focusViewAfterDialog: false }
    Views.SettingsView { anchors.fill: parent }
}
)";
        engine.loadData(wrapper, directory.resolved(QUrl("FrameRateTest.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = engine.rootObjects().first();
        QVERIFY(root->setProperty("probeLimit", limit));
        auto* row = root->findChild<QObject*>("frameRateRow");
        auto* toggle = root->findChild<QObject*>("ultraLowLatencyVrr");
        QVERIFY(row && toggle);
        auto* editor = row->property("editor").value<QObject*>();
        QVERIFY(editor);

        QVERIFY(QMetaObject::invokeMethod(toggle, "activateSetting"));
        QVERIFY(prefs->ultraLowLatencyVrr);
        QCOMPARE(editor->property("fpsLimit").toInt(), limit);
        QVERIFY(editor->property("currentText").toString().contains(QString("%1 FPS").arg(shownFps)));
        QCOMPARE(prefs->fps, requestedFps);

        QVERIFY(QMetaObject::invokeMethod(toggle, "activateSetting"));
        QVERIFY(!prefs->ultraLowLatencyVrr);
        QCOMPARE(editor->property("fpsLimit").toInt(), 0);
        QVERIFY(editor->property("currentText").toString().contains(QString("%1 FPS").arg(requestedFps)));
        QCOMPARE(prefs->fps, requestedFps);

        prefs->setProperty("fps", savedFps);
        prefs->setProperty("enableVrr", savedVrr);
        prefs->setProperty("enableVsync", savedVsync);
        prefs->setProperty("ultraLowLatencyVrr", savedUltraLowLatency);
    }
    void settingsPresentation_data() {
        QTest::addColumn<bool>("fullscreen");
        QTest::addColumn<bool>("controller");
        QTest::newRow("desktop-windowed") << false << false;
        QTest::newRow("desktop-fullscreen") << true << false;
        QTest::newRow("controller-fullscreen") << true << true;
    }
    void settingsPresentation() {
        QFETCH(bool, fullscreen);
        QFETCH(bool, controller);
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        QScopedValueRollback<QString> host(prefs->uiLastHostUuid, {});
        QScopedValueRollback<StreamingPreferences::UIDisplayMode> mode(prefs->uiDisplayMode, fullscreen ? StreamingPreferences::UI_FULLSCREEN : StreamingPreferences::UI_WINDOWED);
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(root);
        QTRY_COMPARE(root->visibility(), fullscreen ? QWindow::FullScreen : QWindow::Windowed);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        auto center = [](QQuickItem* item) { return item->mapToScene(item->boundingRect().center()).toPoint(); };
        QQuickItem* settingsButton = nullptr;
        for (auto* item : root->findChildren<QQuickItem*>()) {
            if (item->property("text").toString() == "Settings" &&
                    item->metaObject()->indexOfSignal("clicked()") >= 0) {
                settingsButton = item;
                break;
            }
        }
        QVERIFY(settingsButton);
        auto press = [&](Uint8 code) {
            button(SDL_CONTROLLERBUTTONDOWN, code); button(SDL_CONTROLLERBUTTONUP, code);
            settleLayout(engine, root);
        };
        if (controller) {
            navigation->notifyWindowFocus(true);
            QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
            press(SDL_CONTROLLER_BUTTON_START);
        } else QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, center(settingsButton));
        auto* rail = root->findChild<QQuickItem*>("settingsCategories");
        QTRY_VERIFY(rail && rail->hasActiveFocus());
        settleLayout(engine, root);
        auto* resolution = root->findChild<QQuickItem*>("resolutionRow");
        QVERIFY(resolution);
        auto* pane = root->findChild<QQuickItem*>("settingsContent");
        QVERIFY(pane);
        const auto capture = [&](const QString& state) {
            const QString dir = qEnvironmentVariable("CONSOLE_UI_CAPTURE_DIR");
            if (dir.isEmpty()) return;
            QTest::qWait(250);
            const auto shot = root->grabWindow();
            QVERIFY(!shot.isNull());
            QVERIFY(shot.save(dir + QString("/%1-%2-%3.png").arg(fullscreen ? "fullscreen" : "windowed").arg(root->width()).arg(state)));
        };
        auto* page = rail->parentItem();
        QCOMPARE(page->property("desktopPreferences").toBool(), !controller);
        if (controller) {
            press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            QTRY_COMPARE(root->activeFocusItem(), resolution);
            capture("controller");
            press(SDL_CONTROLLER_BUTTON_X);
            auto* help = root->findChild<QObject*>("settingsHelpDialog");
            QTRY_VERIFY(help && help->property("visible").toBool());
            press(SDL_CONTROLLER_BUTTON_B);
            QTRY_COMPARE(root->activeFocusItem(), resolution);
            return;
        }
        auto* frameRate = root->findChild<QQuickItem*>("frameRateRow");
        auto* search = root->findChild<QQuickItem*>("desktopSettingsSearch");
        auto* results = root->findChild<QQuickItem*>("desktopSettingsResults");
        auto* dialog = root->findChild<QObject*>("settingsHelpDialog");
        QVERIFY(frameRate && search && results && dialog);
        const int savedWidth = prefs->width;
        QTest::mouseMove(root, resolution->mapToScene(QPointF(35, 15)).toPoint());
        QTRY_COMPARE(page->property("hoverRow").value<QQuickItem*>(), resolution);
        auto* summary = visualItem(resolution, "inlineSettingSummary");
        QVERIFY(summary && summary->isVisible());
        QVERIFY(!summary->property("text").toString().isEmpty());
        capture("hover");
        auto* combo = resolution->property("editor").value<QQuickItem*>();
        auto* choices = combo ? combo->property("popup").value<QObject*>() : nullptr;
        QVERIFY(choices);
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, center(combo));
        QTRY_VERIFY(choices->property("visible").toBool());
        QTest::keyClick(root, Qt::Key_Escape);
        QTRY_VERIFY(!choices->property("visible").toBool());
        QTRY_COMPARE(root->activeFocusItem(), resolution);
        QCOMPARE(prefs->width, savedWidth);
        QTest::keyClick(root, Qt::Key_Left);
        QTRY_VERIFY(rail->hasActiveFocus());
        QTest::keyClick(root, Qt::Key_Tab);
        QTRY_COMPARE(root->activeFocusItem(), resolution);
        QTest::keyClick(root, Qt::Key_Down);
        QTRY_COMPARE(root->activeFocusItem(), frameRate);
        capture("focus");
        QTest::keyClick(root, Qt::Key_Tab, Qt::ShiftModifier);
        QTRY_COMPARE(root->activeFocusItem()->objectName(), QString("inlineMoreHelp"));
        QTest::keyClick(root, Qt::Key_Tab);
        QTRY_COMPARE(root->activeFocusItem(), frameRate);
        auto* more = visualItem(resolution, "inlineMoreHelp");
        QVERIFY(more && more->isVisible());
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, center(more));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QCOMPARE(dialog->property("text").toString(), resolution->property("settingsHelp").toString());
        capture("help");
        QTest::keyClick(root, Qt::Key_Escape);
        QTRY_COMPARE(root->activeFocusItem(), resolution);
        QTest::keyClick(root, Qt::Key_F1);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTest::keyClick(root, Qt::Key_Escape);
        QTRY_COMPARE(root->activeFocusItem(), resolution);

        auto* bitrateRow = root->findChild<QQuickItem*>("bitrateRow");
        QVERIFY(bitrateRow);
        for (int i = 0; i < 5 && root->activeFocusItem() != bitrateRow; ++i)
            QTest::keyClick(root, Qt::Key_Down);
        QTRY_COMPARE(root->activeFocusItem(), bitrateRow);
        auto* slider = bitrateRow->property("editor").value<QQuickItem*>();
        QVERIFY(slider && slider->property("desktop").toBool());
        const int bitrateBefore = prefs->bitrateKbps;
        const bool automaticBefore = prefs->autoAdjustBitrate;
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier,
                          slider->mapToScene(QPointF(slider->width() * 0.7, slider->height() * 0.75)).toPoint());
        QTRY_VERIFY(prefs->bitrateKbps != bitrateBefore);
        QVERIFY(!prefs->autoAdjustBitrate);
        prefs->setProperty("bitrateKbps", bitrateBefore);
        prefs->setProperty("autoAdjustBitrate", automaticBefore);

        QTest::keyClick(root, Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(search->hasActiveFocus());
        auto type = [&](const QByteArray& text) {
            for (char ch : text) QTest::keyClick(root, static_cast<Qt::Key>(ch == ' ' ? Qt::Key_Space : Qt::Key_A + ch - 'a'));
        };
        type("frametime");
        QTRY_VERIFY(results->isVisible() && results->property("count").toInt() > 0);
        capture("search");
        QTest::keyClick(root, Qt::Key_Return);
        QTRY_COMPARE(page->property("category").toInt(), 6);
        QTRY_COMPARE(root->activeFocusItem()->property("settingsLabel").toString(), QString("Show frametime graph while streaming"));
        QTest::keyClick(root, Qt::Key_F1);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(dialog->property("text").toString().contains("Ctrl+Alt+Shift+F"));
        capture("full-help");
        QTest::keyClick(root, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(prefs->width, savedWidth);
        QTest::keyClick(root, Qt::Key_F, Qt::ControlModifier);
        type("zzzzzz");
        QTRY_COMPARE(results->property("count").toInt(), 0);
        QTest::keyClick(root, Qt::Key_Escape);
        QVERIFY(!results->isVisible());
        QVERIFY(search->hasActiveFocus());
        const bool vrrBefore = prefs->enableVrr;
        prefs->setProperty("enableVrr", true);
        type("buffer allowance");
        QTRY_VERIFY(results->property("count").toInt() > 0);
        QTest::keyClick(root, Qt::Key_Return);
        auto* spin = root->findChild<QQuickItem*>("vrrBuffer");
        QTRY_VERIFY(spin && spin->isVisible() && spin->property("desktopControl").toBool());
        QTRY_COMPARE(root->activeFocusItem(), spin->property("settingsRowHost").value<QQuickItem*>());
        settleLayout(engine, root);
        auto* input = spin->property("contentItem").value<QQuickItem*>();
        QVERIFY(input);
        QTRY_VERIFY(pane->contains(input->mapToItem(pane, input->boundingRect().center())));
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, center(input));
        QTRY_VERIFY(input->hasActiveFocus() || spin->hasActiveFocus());
        const int bufferValue = spin->property("value").toInt();
        QTest::keyClick(root, Qt::Key_Left);
        QTest::keyClick(root, Qt::Key_Right);
        QCOMPARE(spin->property("value").toInt(), bufferValue);
        QTest::keyClick(root, Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(search->hasActiveFocus());
        prefs->setProperty("enableVrr", vrrBefore);
        // Selecting a search result must show the live disabled proxy and its
        // explanation. Merely browsing it must leave the preference untouched.
        const bool vsyncBefore = prefs->enableVsync;
        prefs->setProperty("enableVsync", false);
        type("frame pacing");
        QTRY_VERIFY(results->property("count").toInt() > 0);
        QTest::keyClick(root, Qt::Key_Return);
        QTRY_VERIFY(root->activeFocusItem()->property("disabledSettingProxy").toBool());
        QVERIFY(root->activeFocusItem()->property("settingsHelp").toString().contains("V-Sync"));
        QTest::keyClick(root, Qt::Key_Return);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(!prefs->enableVsync);
        QTest::keyClick(root, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(root->activeFocusItem()->property("disabledSettingProxy").toBool());
        prefs->setProperty("enableVsync", vsyncBefore);
        auto* done = root->findChild<QQuickItem*>("desktopSettingsDone");
        QVERIFY(done);
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, center(done));
        QTRY_VERIFY(!root->findChild<QQuickItem*>("settingsCategories") || !root->findChild<QQuickItem*>("settingsCategories")->isVisible());
    }

    void benqDockUsesMoreHelp() {
        QScopedValueRollback<bool> enabledVrr(prefs->enableVrr, true);
        QScopedValueRollback<bool> enabledVsync(prefs->enableVsync, true);
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        const auto directory = QUrl::fromLocalFile(QStringLiteral(CONSOLE_GUI_PATH) + "/");
        const QByteArray wrapper = R"(
import QtQuick 2.9
import QtQuick.Controls 2.5
import SystemProperties 1.0
import "." as Views
ApplicationWindow {
    visible: true; width: 1280; height: 800
    property bool pollingActive: false
    Component.onCompleted: SystemProperties.mockGr10State = SystemProperties.Gr10NeedsHelper
    Item { id: stackView; property bool focusViewAfterDialog: false }
    Views.SettingsView { objectName: "dockSettings"; anchors.fill: parent }
}
)";
        engine.loadData(wrapper, directory.resolved(QUrl("DockSettingsTest.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        auto* settings = root->findChild<QQuickItem*>("dockSettings");
        auto* dock = root->findChild<QQuickItem*>("benqGr10VrrRestart");
        auto* dialog = root->findChild<QObject*>("settingsHelpDialog");
        QVERIFY(settings && dock && dialog);
        QVERIFY(settings->setProperty("desktopPreferences", false));
        root->requestActivate();
        QTRY_VERIFY(dock->isVisible());
        dock->forceActiveFocus();
        QTRY_VERIFY(settings->property("helpDialogOnly").toBool());
        QVERIFY(QMetaObject::invokeMethod(settings, "optionsAction"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(dialog->property("text").toString().contains("Install the SteamOS GR10 recovery helper"));
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
        const QPointer<QQuickItem> shelf(root->findChild<QQuickItem*>("gameShelf"));
        QTRY_VERIFY(shelf && shelf->isVisible());
        QKeyEvent settingsPress(QEvent::KeyPress,Qt::Key_Hangup,Qt::NoModifier);
        QCoreApplication::sendEvent(root,&settingsPress);
        QKeyEvent settingsRelease(QEvent::KeyRelease,Qt::Key_Hangup,Qt::NoModifier);
        QCoreApplication::sendEvent(root,&settingsRelease);
        const QPointer<QQuickItem> categories(root->findChild<QQuickItem*>("settingsCategories"));
        QTRY_VERIFY(categories && categories->isVisible());
        QTest::qWait(60);
        if (!capture.isEmpty()) QVERIFY(root->grabWindow().save(capture+"/settings-main.png"));
        QTest::keyClick(root,Qt::Key_Escape);
        QTRY_VERIFY(shelf && shelf->isVisible());
        QTest::keyClick(root,Qt::Key_Menu);
        auto* play=root->findChild<QQuickItem*>("gamePlayButton");
        QTRY_VERIFY(play && play->hasActiveFocus());
        QTest::keyClick(root,Qt::Key_Menu);
        QTRY_VERIFY(shelf && shelf->hasActiveFocus());
        QTRY_VERIFY(!categories || !categories->isVisible());
        QTest::keyClick(root,Qt::Key_Menu);
        QTRY_VERIFY(play->hasActiveFocus());
        for (int i=0;i<10;++i) QTest::keyClick(root,Qt::Key_Down);
        auto* lastAction = root->activeFocusItem();
        QVERIFY(lastAction != play);
        QTest::keyClick(root,Qt::Key_Down); QCOMPARE(root->activeFocusItem(),lastAction);
        QTest::keyClick(root,Qt::Key_Escape);
        QTRY_VERIFY(shelf && shelf->hasActiveFocus());
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
    void settingsBackReturnsToRememberedLibraryFromPcs() {
        prefs->uiLastHostUuid.clear();
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(CONSOLE_MOCKS_PATH));
        engine.rootContext()->setContextProperty("initialView", QString("qrc:/gui/PcView.qml"));
        engine.rootContext()->setContextProperty("runConfigChecks", false);
        engine.load(QUrl("qrc:/gui/main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(root);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        navigation->notifyWindowFocus(true);
        QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
        const auto press = [&](Uint8 code) { button(SDL_CONTROLLERBUTTONDOWN, code); button(SDL_CONTROLLERBUTTONUP, code); };

        auto* pcs = root->findChild<QQuickItem*>("Computers");
        QTRY_VERIFY(pcs && pcs->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!root->property("selectedHostName").toString().isEmpty());
        const QString host = root->property("selectedHostName").toString();
        const auto libraryFocused = [&] {
            for (auto* item = root->activeFocusItem(); item; item = item->parentItem()) {
                if (item->objectName() == host) return true;
            }
            return false;
        };
        QTRY_VERIFY(libraryFocused());

        QVERIFY(QMetaObject::invokeMethod(root, "focusNavigation"));
        const auto focusedText = [&] {
            auto* focused = root->activeFocusItem();
            return focused ? focused->property("text").toString() : QString();
        };
        QCOMPARE(focusedText(), QString("Home"));
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        QTRY_COMPARE(focusedText(), QString("Settings"));
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        QTRY_COMPARE(focusedText(), QString("PCs"));
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QTRY_VERIFY(libraryFocused());

        const auto settingsFocused = [&] {
            auto* categories = root->findChild<QQuickItem*>("settingsCategories");
            return categories && categories->hasActiveFocus();
        };
        press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        QTRY_VERIFY(settingsFocused());
        press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        QTRY_VERIFY(pcs->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        QTRY_VERIFY(settingsFocused());
        const QPointer<QQuickItem> categories(root->findChild<QQuickItem*>("settingsCategories"));
        press(SDL_CONTROLLER_BUTTON_B);
        QTRY_COMPARE(root->property("selectedHostName").toString(), host);
        QTRY_VERIFY(libraryFocused());
        QTRY_VERIFY(!categories || !categories->isVisible());
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
        const auto press=[&](Uint8 code) { button(SDL_CONTROLLERBUTTONDOWN,code); button(SDL_CONTROLLERBUTTONUP,code); settleLayout(engine, root); };
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
        root->resize(size); settleLayout(engine, root);
        for(int category=0;category<8;++category) {
            rail->forceActiveFocus(); page->setProperty("category",category); settleLayout(engine, root);
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
        auto* latencyRow = root->findChild<QQuickItem*>("ultraLowLatencyVrr");
        auto* help = root->findChild<QObject*>("settingsHelpDialog");
        QVERIFY(latencyRow && help);
        for (const QSize size : {QSize(800, 500), QSize(1280, 720)}) {
            root->resize(size);
            settleLayout(engine, root);
            latencyRow->forceActiveFocus();
            press(SDL_CONTROLLER_BUTTON_X);
            QTRY_VERIFY(help->property("opened").toBool());
            QVERIFY(help->property("text").toString().contains("2 frames of presentation latency on average"));

            auto* background = help->property("background").value<QQuickItem*>();
            auto* message = visualItem(root->contentItem(), "messageScroll");
            QVERIFY(background && message);
            const QPointF center = background->mapToScene(QPointF(background->width() / 2, background->height() / 2));
            QVERIFY(QLineF(center, QPointF(size.width() / 2, size.height() / 2)).length() <= 1);
            QVERIFY(background->height() < size.height());
            QVERIFY(qAbs(message->width() - help->property("availableWidth").toReal()) <= 1);
            captureState(QString("latency-more-help-%1").arg(size.width()));

            press(SDL_CONTROLLER_BUTTON_B);
            QTRY_VERIFY(!help->property("visible").toBool());
            QTRY_COMPARE(root->activeFocusItem(), latencyRow);
        }
        QVERIFY(!navigation->currentUiNavMode());
        auto* resetRow=root->findChild<QQuickItem*>("resetBitrateRow"); QVERIFY(resetRow && resetRow->isVisible());
        resetRow->forceActiveFocus(); press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!resetRow->isVisible());
        QTRY_VERIFY(root->activeFocusItem() && root->activeFocusItem()->isVisible() && root->activeFocusItem()->isEnabled());
        QVERIFY(!root->activeFocusItem()->property("settingsLabel").toString().isEmpty());
        QQuickItem* vsyncSetting = nullptr;
        for (auto* item : root->findChildren<QQuickItem*>())
            if (item->isVisible() && item->property("settingsLabel").toString() == "V-Sync") { vsyncSetting = item; break; }
        QVERIFY(vsyncSetting);
        vsyncSetting->forceActiveFocus();
        auto* vsyncRow=root->activeFocusItem();
        press(SDL_CONTROLLER_BUTTON_A); QVERIFY(!prefs->enableVsync);
        auto* disabledPacing=root->findChild<QQuickItem*>("framePacingRow"); QVERIFY(disabledPacing);
        QVERIFY(!disabledPacing->isEnabled());
        QCOMPARE(disabledPacing->property("settingsNavigation").value<QObject*>(),page);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        QVERIFY(root->activeFocusItem()->isEnabled());
        QCOMPARE(root->activeFocusItem()->property("settingsLabel").toString(), QString("Frame pacing"));
        QVERIFY(root->activeFocusItem()->property("disabledSettingProxy").toBool());
        auto* disabledProxy = root->activeFocusItem();
        const auto disabledSnapshot = snapshot();
        for (const QSize size : {QSize(800,500), QSize(1280,720)}) {
            root->resize(size);
            QTRY_VERIFY(QLineF(disabledProxy->mapToScene(QPointF()), disabledPacing->mapToScene(QPointF())).length() < 1);
            QTRY_VERIFY(qAbs(disabledProxy->width() - disabledPacing->width()) < 1);
            QTRY_VERIFY(qAbs(disabledProxy->height() - disabledPacing->height()) < 1);
            QTRY_COMPARE(root->activeFocusItem(), disabledProxy);
            QTRY_VERIFY(disabledProxy->mapToItem(pane, QPointF()).y() >= -1 &&
                disabledProxy->mapToItem(pane, QPointF()).y() + disabledProxy->height() <= pane->height() + 1);
            QCOMPARE(snapshot(), disabledSnapshot);
            captureState(QString("disabled-row-%1").arg(size.width()));
        }
        press(SDL_CONTROLLER_BUTTON_A);
        auto* disabledHelp = root->findChild<QObject*>("settingsHelpDialog"); QVERIFY(disabledHelp);
        QTRY_VERIFY(disabledHelp->property("visible").toBool());
        QVERIFY(!disabledHelp->property("text").toString().isEmpty());
        press(SDL_CONTROLLER_BUTTON_B); QTRY_VERIFY(!disabledHelp->property("visible").toBool());
        QCOMPARE(snapshot(), disabledSnapshot);
        press(SDL_CONTROLLER_BUTTON_DPAD_UP); QCOMPARE(root->activeFocusItem(),vsyncRow);
        press(SDL_CONTROLLER_BUTTON_A); QVERIFY(prefs->enableVsync);
#ifdef Q_OS_WIN
        QQuickItem* vrrSetting = nullptr;
        for (auto* item : root->findChildren<QQuickItem*>()) {
            if (item->isVisible() && item->property("settingsLabel").toString() == "VRR") {
                vrrSetting = item;
                break;
            }
        }
        QVERIFY(vrrSetting);
        auto* frameRateRow = root->findChild<QQuickItem*>("frameRateRow");
        QVERIFY(frameRateRow);
        auto* frameRate = frameRateRow->property("editor").value<QObject*>();
        QVERIFY(frameRate);
        const int savedFps = prefs->fps;
        const int savedBitrate = prefs->bitrateKbps;
        prefs->setProperty("fps", 116);
        vrrSetting->forceActiveFocus();
        press(SDL_CONTROLLER_BUTTON_A);
        QVERIFY(!prefs->enableVrr);
        QCOMPARE(prefs->fps, 120);
        QCOMPARE(frameRate->property("currentText").toString(), QString("120 FPS"));
        press(SDL_CONTROLLER_BUTTON_A);
        QVERIFY(prefs->enableVrr);
        QCOMPARE(prefs->fps, 116);
        QCOMPARE(frameRate->property("currentText").toString(), QString("116 FPS"));
        prefs->setProperty("fps", 100);
        press(SDL_CONTROLLER_BUTTON_A);
        QCOMPARE(prefs->fps, 100);
        press(SDL_CONTROLLER_BUTTON_A);
        QCOMPARE(frameRate->property("currentText").toString(), QString("100 FPS (Low-latency VRR)"));
        prefs->setProperty("fps", 120);
        auto* ultraLowLatency = root->findChild<QQuickItem*>("ultraLowLatencyVrr");
        QVERIFY(ultraLowLatency);
        ultraLowLatency->forceActiveFocus();
        press(SDL_CONTROLLER_BUTTON_A);
        QVERIFY(prefs->ultraLowLatencyVrr);
        QCOMPARE(frameRate->property("fpsLimit").toInt(), 116);
        QCOMPARE(frameRate->property("currentText").toString(), QString("116 FPS"));
        QCOMPARE(prefs->fps, 120);
        press(SDL_CONTROLLER_BUTTON_A);
        QVERIFY(!prefs->ultraLowLatencyVrr);
        QCOMPARE(frameRate->property("fpsLimit").toInt(), 0);
        QCOMPARE(frameRate->property("currentText").toString(), QString("120 FPS"));
        prefs->setProperty("fps", savedFps);
        prefs->setProperty("bitrateKbps", savedBitrate);
#endif
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
        // StackView owns the Settings page and can destroy it during Back.
        const QPointer<QQuickItem> settingsRail(rail);
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!settingsRail || !settingsRail->isVisible());
        const auto hostsVisible = [root] {
            auto* hosts = root->findChild<QQuickItem*>("Computers");
            return hosts && hosts->isVisible();
        };
        QTRY_VERIFY(hostsVisible());
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
        // Complete the intentional stale-input flush before injecting a new press.
        QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
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
        QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
        const auto press=[&](Uint8 code) { button(SDL_CONTROLLERBUTTONDOWN,code); button(SDL_CONTROLLERBUTTONUP,code); };
        const QPointer<QQuickItem> add(root->findChild<QQuickItem*>("addPcTile"));
        QVERIFY(add);
        add->forceActiveFocus();
        press(SDL_CONTROLLER_BUTTON_DPAD_LEFT); press(SDL_CONTROLLER_BUTTON_A);
        auto* shelf=root->findChild<QQuickItem*>("gameShelf"); QTRY_VERIFY(shelf && shelf->isVisible());
        auto* page=shelf->parentItem(); while(page && !page->property("appModel").isValid()) page=page->parentItem(); QVERIFY(page);
        auto* model=qobject_cast<ConsoleAppModel*>(page->property("appModel").value<QObject*>()); QVERIFY(model);
        model->setProperty("appListLoading",true);
        for(int id : QList<int>(model->ids)) model->removeApp(id);
        QTRY_VERIFY(!page->property("hasGames").toBool());
        const QPointer<QQuickItem> choose(root->findChild<QQuickItem*>("emptyLibraryChoosePc"));
        QTRY_VERIFY(choose && choose->hasActiveFocus());
        QCOMPARE(page->property("primaryVerb").toString(),QString("Choose PC"));
        auto* emptySleep = root->findChild<QQuickItem*>("emptyHostSleepAction"); QVERIFY(emptySleep);
        // Expose the optional host capability without performing a power action.
        emptySleep->setProperty("visible", true);
        emptySleep->forceActiveFocus();
        QTRY_COMPARE(page->property("primaryVerb").toString(), QString("Sleep PC"));
        QVERIFY(QMetaObject::invokeMethod(page, "primaryAction"));
        auto* sleepConfirmation = root->findChild<QObject*>("hostSleepConfirmation"); QVERIFY(sleepConfirmation);
        QTRY_VERIFY(sleepConfirmation->property("visible").toBool());
        QTest::keyClick(root, Qt::Key_Escape);
        QTRY_VERIFY(!sleepConfirmation->property("visible").toBool());
        emptySleep->setProperty("visible", false);
        QVERIFY(choose);
        choose->forceActiveFocus();
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
        model->removeApp(2); QTRY_VERIFY(choose && choose->hasActiveFocus());
        press(SDL_CONTROLLER_BUTTON_A);
        QTRY_VERIFY(!choose || !choose->isVisible());
        QTRY_VERIFY(add && add->isVisible());
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
        QCOMPARE(navigation->inputMethod(), QString("keyboard"));
        QCOMPARE(navigation->controllerFamily(), QString("xbox"));
        // Reproduce a controller disconnect while the stream owns SDL.
        const int psIndex = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        QVERIFY(psIndex >= 0);
        auto* ps = SDL_JoystickOpen(psIndex); QVERIFY(ps);
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
        navigation->m_ControllerFamily="playstation";
        navigation->m_InputMethod="controller";
        navigation->disable();
        QCOMPARE(SDL_JoystickDetachVirtual(psIndex), 0); SDL_JoystickClose(ps);
        navigation->enable();
        QCOMPARE(navigation->connectedGamepads(), 0);
        QCOMPARE(navigation->inputMethod(), QString("keyboard"));
        QCOMPARE(navigation->controllerFamily(), QString("xbox"));
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
        // Let the launcher activate itself at startup without a controller.
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        QTRY_COMPARE(root->visibility(), QWindow::Windowed);
        QVERIFY(root->property("compactDesktop").toBool());
        QCOMPARE(root->property("uiScale").toReal(), 0.75);
        QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QCOMPARE(root->visibility(), QWindow::Windowed);
#ifdef Q_OS_WIN
        // Leave the launcher visible in the background. Its own timer must
        // see the hotplug and activate it even with automatic fullscreen off.
        window.requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), &window);
        QVERIFY(!navigation->m_HasFocus);
        QVERIFY(navigation->m_PollingTimer->isActive());
#endif
        for (int i = 0; i < 2; ++i) {
            const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
            QVERIFY(index >= 0);
            auto* joystick = SDL_JoystickOpen(index); QVERIFY(joystick);
            virtualControllers.append(joystick);
        }
#ifdef Q_OS_WIN
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
        QCOMPARE(root->visibility(), QWindow::Windowed);
#else
        QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired");
#endif
        QTRY_COMPARE(navigation->connectedGamepads(), 2);
#ifdef Q_OS_WIN
        window.requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), &window);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A);
        QVERIFY(window.keys.isEmpty());
        QCOMPARE(QGuiApplication::focusWindow(), &window);
        root->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), root);
#endif
        prefs->uiControllerFullscreen = true;
        emit prefs->uiControllerFullscreenChanged();
        QTRY_COMPARE(root->visibility(), QWindow::FullScreen);
        QVERIFY(!root->property("compactDesktop").toBool());
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
