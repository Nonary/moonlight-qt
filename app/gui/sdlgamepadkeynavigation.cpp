#include "sdlgamepadkeynavigation.h"

#include <QKeyEvent>
#include <QGuiApplication>
#include <QWindow>

#include "settings/mappingmanager.h"

#define AXIS_NAVIGATION_REPEAT_DELAY 150

SdlGamepadKeyNavigation::SdlGamepadKeyNavigation(StreamingPreferences* prefs)
    : m_Prefs(prefs),
      m_Enabled(false),
      m_UiNavMode(false),
      m_FirstPoll(false),
      m_HasFocus(false),
      m_LastAxisNavigationEventTime(0)
{
    QCoreApplication::instance()->installEventFilter(this);
    m_PollingTimer = new QTimer(this);
    connect(m_PollingTimer, &QTimer::timeout, this, &SdlGamepadKeyNavigation::onPollingTimerFired);
}

SdlGamepadKeyNavigation::~SdlGamepadKeyNavigation()
{
    QCoreApplication::instance()->removeEventFilter(this);
    disable();
}

void SdlGamepadKeyNavigation::enable()
{
    if (m_Enabled) {
        return;
    }

    // We have to initialize and uninitialize this in enable()/disable()
    // because we need to get out of the way of the Session class. If it
    // doesn't get to reinitialize the GC subsystem, it won't get initial
    // arrival events. Additionally, there's a race condition between
    // our QML objects being destroyed and SDL being deinitialized that
    // this solves too.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
        return;
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    // Drop all pending gamepad add events. SDL will generate these for us
    // on first init of the GC subsystem. We can't depend on them due to
    // overlapping lifetimes of SdlGamepadKeyNavigation instances, so we
    // will attach ourselves.
    //
    // NB: We use SDL_JoystickUpdate() instead of SDL_PumpEvents() because
    // the latter can do a bit more work that we want (like handling video
    // events that we intentionally do not want to process yet).
    SDL_JoystickUpdate();
    SDL_FlushEvent(SDL_CONTROLLERDEVICEADDED);

    // Open all currently attached game controllers
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* gc = SDL_GameControllerOpen(i);
            if (gc != nullptr) {
                m_Gamepads.append(gc);
            }
        }
    }

    if (!m_Gamepads.isEmpty()) noteController(m_Gamepads.first());
    else { m_InputMethod = "keyboard"; m_ControllerFamily = "xbox"; }
    emit navigationStateChanged();
    m_Enabled = true;

    // Start the polling timer if the window is focused
    updateTimerState();
}

void SdlGamepadKeyNavigation::disable()
{
    if (!m_Enabled) {
        return;
    }

    m_Enabled = false;
    releaseControllerKeys();
    updateTimerState();
    Q_ASSERT(!m_PollingTimer->isActive());

    while (!m_Gamepads.isEmpty()) {
        SDL_GameControllerClose(m_Gamepads[0]);
        m_Gamepads.removeAt(0);
    }

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    emit navigationStateChanged();
}

void SdlGamepadKeyNavigation::notifyWindowFocus(bool hasFocus)
{
    if (!hasFocus) releaseControllerKeys();
    m_HasFocus = hasFocus;
    updateTimerState();
}

void SdlGamepadKeyNavigation::onPollingTimerFired()
{
    SDL_Event event;

    // Update joystick state without pumping other events (see enable() comment)
    SDL_JoystickUpdate();

    // Discard any pending button events on the first poll to avoid picking up
    // stale input data from the stream session (like the quit combo).
    if (m_FirstPoll) {
        releaseControllerKeys();
        SDL_FlushEvent(SDL_CONTROLLERBUTTONDOWN);
        SDL_FlushEvent(SDL_CONTROLLERBUTTONUP);
        m_FirstPoll = false;
    }

    // Peep events rather than polling to avoid calling SDL_PumpEvents()
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) == 1) {
        switch (event.type) {
        case SDL_QUIT:
            // SDL may send us a quit event since we initialize
            // the video subsystem on startup. If we get one,
            // forward it on for Qt to take care of.
            QCoreApplication::instance()->quit();
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        {
            QEvent::Type type =
                    event.type == SDL_CONTROLLERBUTTONDOWN ?
                        QEvent::Type::KeyPress : QEvent::Type::KeyRelease;

            noteController(SDL_GameControllerFromInstanceID(event.cbutton.which));
            const quint64 buttonId = (quint64(quint32(event.cbutton.which)) << 8) |
                                      event.cbutton.button;
            if (type == QEvent::KeyRelease) {
                // A release after focus reset or an unmatched release must
                // never become a fresh navigation event in the new context.
                if (m_PressedControllerKeys.contains(buttonId)) {
                    const auto key = m_PressedControllerKeys.take(buttonId);
                    if (key.window) sendKey(type, key.key, key.modifiers, key.window);
                }
                break;
            }
            // SDL buttons do not repeat. Duplicate downs must not apply a
            // second adjustment or overwrite the original release route.
            if (m_PressedControllerKeys.contains(buttonId)) {
                break;
            }
            const auto sendControllerKey = [&](Qt::Key key,
                                               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
                auto* target = QGuiApplication::focusWindow();
                if (!target) return;
                const bool directional = event.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
                                         event.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
                m_PressedControllerKeys.insert(buttonId, {key, modifiers, target,
                    SDL_GetTicks() + 350, directional, m_UiNavMode});
                sendKey(type, key, modifiers, target);
            };

            // Swap face buttons if needed
            if (m_Prefs->swapFaceButtons) {
                switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_B;
                    break;
                case SDL_CONTROLLER_BUTTON_B:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_A;
                    break;
                case SDL_CONTROLLER_BUTTON_X:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_Y;
                    break;
                case SDL_CONTROLLER_BUTTON_Y:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_X;
                    break;
                }
            }

            switch (event.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                if (m_UiNavMode) {
                    // Back-tab
                    sendControllerKey(Qt::Key_Backtab);
                }
                else {
                    sendControllerKey(Qt::Key_Up);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                if (m_UiNavMode) {
                    sendControllerKey(Qt::Key_Tab);
                }
                else {
                    sendControllerKey(Qt::Key_Down);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                sendControllerKey(Qt::Key_Left);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                sendControllerKey(Qt::Key_Right);
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                sendControllerKey(Qt::Key_PageUp);
                break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                sendControllerKey(Qt::Key_PageDown);
                break;
            case SDL_CONTROLLER_BUTTON_A:
                if (m_UiNavMode) {
                    sendControllerKey(Qt::Key_Space);
                }
                else {
                    sendControllerKey(Qt::Key_Return);
                }
                break;
            case SDL_CONTROLLER_BUTTON_B:
                sendControllerKey(Qt::Key_Escape);
                break;
            case SDL_CONTROLLER_BUTTON_X:
                sendControllerKey(Qt::Key_Menu);
                break;
            case SDL_CONTROLLER_BUTTON_Y:
            case SDL_CONTROLLER_BUTTON_START:
                // HACK: We use this keycode to inform main.qml
                // to show the settings when Key_Menu is handled
                // by the control in focus.
                sendControllerKey(Qt::Key_Hangup);
                break;
            default:
                break;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED:
            // A disconnected pad must not leave a pressed key or stale hints.
            releaseControllerKeys();
            for (int i = m_Gamepads.size() - 1; i >= 0; --i) {
                if (!SDL_GameControllerGetAttached(m_Gamepads[i])) {
                    SDL_GameControllerClose(m_Gamepads.takeAt(i));
                }
            }
            if (!m_Gamepads.isEmpty()) noteController(m_Gamepads.first());
            else { m_InputMethod = "keyboard"; m_ControllerFamily = "xbox"; }
            emit navigationStateChanged();
            break;
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameController* gc = SDL_GameControllerOpen(event.cdevice.which);
            if (gc != nullptr) {
                // SDL_CONTROLLERDEVICEADDED can be reported multiple times for the same
                // gamepad in rare cases, because SDL doesn't fixup the device index in
                // the SDL_CONTROLLERDEVICEADDED event if an unopened gamepad disappears
                // before we've processed the add event.
                if (!m_Gamepads.contains(gc)) {
                    m_Gamepads.append(gc);
                    noteController(gc);
                    emit navigationStateChanged();
                }
                else {
                    // We already have this game controller open
                    SDL_GameControllerClose(gc);
                }
            }
            break;
        }
    }

    // Repeat only directional buttons. Keep the original release route and
    // stop repeating if a popup changes the navigation mode during the hold.
    const auto heldKeys = m_PressedControllerKeys;
    const Uint32 now = SDL_GetTicks();
    for (auto held = heldKeys.cbegin(); held != heldKeys.cend(); ++held) {
        auto live = m_PressedControllerKeys.find(held.key());
        if (live == m_PressedControllerKeys.end()) continue;
        const ControllerKey key = live.value();
        if (key.repeatable && key.navigationMode == m_UiNavMode && key.window &&
            key.window == QGuiApplication::focusWindow() && Sint32(now - key.nextRepeat) >= 0) {
            live->nextRepeat = now + 90;
            sendKey(QEvent::KeyPress, key.key, key.modifiers, key.window, true);
        }
    }

    // Handle analog sticks by polling
    for (auto gc : std::as_const(m_Gamepads)) {
        short leftX = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        short leftY = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        const auto axisController = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        if (qAbs(int(leftX)) <= 30000 && qAbs(int(leftY)) <= 30000)
            m_SuppressedAxes.remove(axisController);
        if (m_SuppressedAxes.contains(axisController)) continue;
        if (qAbs(int(leftX)) > 30000 || qAbs(int(leftY)) > 30000) noteController(gc);
        if (SDL_GetTicks() - m_LastAxisNavigationEventTime < AXIS_NAVIGATION_REPEAT_DELAY) {
            // Do nothing
        }
        else if (leftY < -30000) {
            if (m_UiNavMode) {
                // Back-tab
                sendKey(QEvent::Type::KeyPress, Qt::Key_Backtab);
                sendKey(QEvent::Type::KeyRelease, Qt::Key_Backtab);
            }
            else {
                sendKey(QEvent::Type::KeyPress, Qt::Key_Up);
                sendKey(QEvent::Type::KeyRelease, Qt::Key_Up);
            }

            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
        else if (leftY > 30000) {
            if (m_UiNavMode) {
                sendKey(QEvent::Type::KeyPress, Qt::Key_Tab);
                sendKey(QEvent::Type::KeyRelease, Qt::Key_Tab);
            }
            else {
                sendKey(QEvent::Type::KeyPress, Qt::Key_Down);
                sendKey(QEvent::Type::KeyRelease, Qt::Key_Down);
            }

            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
        else if (leftX < -30000) {
            sendKey(QEvent::Type::KeyPress, Qt::Key_Left);
            sendKey(QEvent::Type::KeyRelease, Qt::Key_Left);
            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
        else if (leftX > 30000) {
            sendKey(QEvent::Type::KeyPress, Qt::Key_Right);
            sendKey(QEvent::Type::KeyRelease, Qt::Key_Right);
            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
    }
}

void SdlGamepadKeyNavigation::releaseControllerKeys()
{
    // Clear before dispatch: a receiver may synchronously change focus/mode.
    const auto pressed = m_PressedControllerKeys;
    m_PressedControllerKeys.clear();
    for (const auto& key : pressed) {
        if (key.window) sendKey(QEvent::KeyRelease, key.key, key.modifiers, key.window);
    }
}

void SdlGamepadKeyNavigation::sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers,
                                     QWindow* target, bool autoRepeat)
{
    QGuiApplication* app = static_cast<QGuiApplication*>(QGuiApplication::instance());
    QWindow* focusWindow = target ? target : app->focusWindow();
    if (focusWindow != nullptr) {
        QKeyEvent keyPressEvent(type, key, modifiers, QString(), autoRepeat);
        const bool wasSending = m_SendingKey;
        m_SendingKey = true;
        app->sendEvent(focusWindow, &keyPressEvent);
        m_SendingKey = wasSending;
    }
}

void SdlGamepadKeyNavigation::updateTimerState()
{
    if (m_PollingTimer->isActive() && (!m_HasFocus || !m_Enabled)) {
        m_PollingTimer->stop();
    }
    else if (!m_PollingTimer->isActive() && m_HasFocus && m_Enabled) {
        // Flush events on the first poll
        m_FirstPoll = true;

        // Poll every 50 ms for a new joystick event
        m_PollingTimer->start(50);
    }
}

void SdlGamepadKeyNavigation::setUiNavMode(bool uiNavMode)
{
    // An explicit call marks a focus-context boundary even when both contexts
    // use arrows. A held direction must not continue into a newly opened editor.
    for (auto key = m_PressedControllerKeys.begin(); key != m_PressedControllerKeys.end(); ++key)
        key->repeatable = false;
    for (auto gc : std::as_const(m_Gamepads)) {
        if (qAbs(int(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX))) > 30000 ||
            qAbs(int(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY))) > 30000)
            m_SuppressedAxes.insert(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc)));
    }
    m_UiNavMode = uiNavMode;
}

int SdlGamepadKeyNavigation::getConnectedGamepads()
{
    Q_ASSERT(m_Enabled);

    int count = 0;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            count++;
        }
    }

    return count;
}

QString SdlGamepadKeyNavigation::familyForControllerType(int type)
{
    QString family = "xbox"; // Unknown and older SDL devices use ABXY prompts.
#if SDL_VERSION_ATLEAST(2, 0, 12)
    {

        if (type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4
#if SDL_VERSION_ATLEAST(2, 0, 14)
            || type == SDL_CONTROLLER_TYPE_PS5
#endif
        ) family = "playstation";
        else if (type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO
#if SDL_VERSION_ATLEAST(2, 24, 0)
                 || type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT
                 || type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT
                 || type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
#endif
        ) family = "nintendo";
    }
#endif
    return family;
}

void SdlGamepadKeyNavigation::noteController(SDL_GameController* controller)
{
    if (!controller) return;
    const QString family = familyForControllerType(
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_GameControllerGetType(controller)
#else
        0
#endif
    );
    if (m_InputMethod != "controller" || m_ControllerFamily != family) {
        m_InputMethod = "controller";
        m_ControllerFamily = family;
        emit navigationStateChanged();
    }
}

bool SdlGamepadKeyNavigation::eventFilter(QObject* watched, QEvent* event)
{
    QString method = m_InputMethod;
    if (!m_SendingKey && event->type() == QEvent::KeyPress) method = "keyboard";
    else if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel) method = "mouse";
    else if (event->type() == QEvent::TouchBegin) method = "touch";
    if (method != m_InputMethod) {
        m_InputMethod = method;
        emit navigationStateChanged();
    }
    return QObject::eventFilter(watched, event);
}
