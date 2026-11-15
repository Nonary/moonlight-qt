#pragma once

#include <QTimer>
#include <QEvent>
#include <QHash>
#include <QSet>
#include <QPair>
#include <QPointer>

class QWindow;

#include "SDL_compat.h"

#include "settings/streamingpreferences.h"

class SdlGamepadKeyNavigation : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int connectedGamepads READ connectedGamepads NOTIFY navigationStateChanged)
    Q_PROPERTY(QString inputMethod READ inputMethod NOTIFY navigationStateChanged)
    Q_PROPERTY(QString controllerFamily READ controllerFamily NOTIFY navigationStateChanged)

public:
    SdlGamepadKeyNavigation(StreamingPreferences* prefs);

    ~SdlGamepadKeyNavigation();

    int connectedGamepads() const { return m_Gamepads.size(); }
    QString inputMethod() const { return m_InputMethod; }
    QString controllerFamily() const { return m_ControllerFamily; }
    Q_INVOKABLE bool currentUiNavMode() const { return m_UiNavMode; }
    Q_INVOKABLE void enable();

    Q_INVOKABLE void disable();

    Q_INVOKABLE void notifyWindowFocus(bool hasFocus);

    Q_INVOKABLE void setUiNavMode(bool settingsMode);

    Q_INVOKABLE int getConnectedGamepads();

signals:
    void navigationStateChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class ControllerNavigationTest;
    static QString familyForControllerType(int type);
    void noteController(SDL_GameController* controller);
    QString m_InputMethod = "keyboard";
    QString m_ControllerFamily = "xbox";
    bool m_SendingKey = false;
    void sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                 QWindow* target = nullptr, bool autoRepeat = false);

    void releaseControllerKeys();

    void updateTimerState();

private slots:
    void onPollingTimerFired();

private:
    StreamingPreferences* m_Prefs;
    QTimer* m_PollingTimer;
    QList<SDL_GameController*> m_Gamepads;
    bool m_Enabled;
    bool m_UiNavMode;
    bool m_FirstPoll;
    bool m_HasFocus;
    Uint32 m_LastAxisNavigationEventTime;
    // Popups can change navigation mode during a button press. Its release
    // must still match the key and modifiers sent for that same press.
    struct ControllerKey {
        Qt::Key key;
        Qt::KeyboardModifiers modifiers;
        QPointer<QWindow> window;
        Uint32 nextRepeat = 0;
        bool repeatable = false;
        bool navigationMode = false;
    };
    QHash<quint64, ControllerKey> m_PressedControllerKeys;
    QSet<SDL_JoystickID> m_SuppressedAxes;
};
