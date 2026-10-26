#pragma once

#include "settings/streamingpreferences.h"
#include "backend/computermanager.h"

#include "SDL_compat.h"
#include "dualsensetriggers.h"
#include "steamcontroller/steamcontrollersdl.h"

#include <Limelight.h>
#include "cursormodepolicy.h"

#include <QHash>
#include <QList>

#include <memory>
#include <vector>

class SteamControllerHost;
class SteamControllerLink;


struct GamepadState {
    SDL_GameController* controller;
    bool hapticsAttached;
    bool speakerPcmAttached;
    SDL_JoystickID jsId;
    short index;
    // Owned by the Steam Controller (2026) driver, not SDL: controller stays null and the SDL
    // loops skip it; the slot only reserves the player index and the gamepad mask bit.
    bool steamOwned;

#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_Haptic* haptic;
    int hapticMethod;
    int hapticEffectId;
#endif

    SDL_TimerID mouseEmulationTimer;
    uint32_t lastStartDownTime;

    bool clickpadButtonEmulationEnabled;
    bool emulatedClickpadButtonDown;

#if SDL_VERSION_ATLEAST(2, 0, 14)
    uint16_t gyroReportPeriodMs;
    float lastGyroEventData[SDL_arraysize(SDL_ControllerSensorEvent::data)];
    uint32_t lastGyroEventTime;

    uint16_t accelReportPeriodMs;
    float lastAccelEventData[SDL_arraysize(SDL_ControllerSensorEvent::data)];
    uint32_t lastAccelEventTime;
#endif

    int buttons;
    short lsX, lsY;
    short rsX, rsY;
    unsigned char lt, rt;
};


// activeGamepadMask is a short, so we're bounded by the number of mask bits
#define MAX_GAMEPADS 16

#define MAX_FINGERS 2

#define GAMEPAD_HAPTIC_METHOD_NONE 0
#define GAMEPAD_HAPTIC_METHOD_LEFTRIGHT 1
#define GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE 2

#define GAMEPAD_HAPTIC_SIMPLE_HIFREQ_MOTOR_WEIGHT 0.33
#define GAMEPAD_HAPTIC_SIMPLE_LOWFREQ_MOTOR_WEIGHT 0.8

// SDL_USEREVENT codes for the client-side cursor (106 is SDL_CODE_STEAM_CONTROLLER_LINK)
#define SDL_CODE_HOST_CURSOR_STATE 107
#define SDL_CODE_HOST_CURSOR_SHAPE 108
#define SDL_CODE_HOST_CURSOR_TICK 109

class SdlInputHandler
{
public:
    explicit SdlInputHandler(StreamingPreferences& prefs, int streamWidth, int streamHeight);

    ~SdlInputHandler();

    void setWindow(SDL_Window* window);

    void handleKeyEvent(SDL_KeyboardEvent* event);

    void handleMouseButtonEvent(SDL_MouseButtonEvent* event);

    void handleMouseMotionEvent(SDL_MouseMotionEvent* event);

    void handleMouseWheelEvent(SDL_MouseWheelEvent* event);

    void handleControllerAxisEvent(SDL_ControllerAxisEvent* event);

    void handleControllerButtonEvent(SDL_ControllerButtonEvent* event);

    void initializeControllers();

    void handleControllerDeviceEvent(SDL_ControllerDeviceEvent* event);

#if SDL_VERSION_ATLEAST(2, 0, 14)
    void handleControllerSensorEvent(SDL_ControllerSensorEvent* event);

    void handleControllerTouchpadEvent(SDL_ControllerTouchpadEvent* event);
#endif

#if SDL_VERSION_ATLEAST(2, 24, 0)
    void handleJoystickBatteryEvent(SDL_JoyBatteryEvent* event);
#endif

    void handleJoystickArrivalEvent(SDL_JoyDeviceEvent* event);

    // SDL_CODE_STEAM_CONTROLLER_LINK: a Steam Controller (2026) link came or went (main thread).
    void handleSteamControllerEvent(SDL_UserEvent* event);

    void sendText(QString& string);

    void rumble(uint16_t controllerNumber, uint16_t lowFreqMotor, uint16_t highFreqMotor);

    void rumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger);

    void setMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

    void setControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);

    void setAdaptiveTriggers(uint16_t controllerNumber, DualSenseOutputReport *report);



    void handleTouchFingerEvent(SDL_TouchFingerEvent* event);

    int getAttachedGamepadMask();

    int getAttachedPlayStationGamepadMask();

    void raiseAllKeys();

    void notifyMouseLeave();

    void notifyFocusLost();

    void notifyFocusGained();

    bool isCaptureActive();

    bool isSystemKeyCaptureActive();

    void setCaptureActive(bool active);

    bool isMouseInVideoRegion(int mouseX, int mouseY, int windowWidth = -1, int windowHeight = -1);

    void updateKeyboardGrabState();

    void updatePointerRegionLock();

    // Client-side cursor (hostcursor.cpp), on the main thread. startHostCursor()
    // subscribes when the host supports it; until then the handlers do nothing.
    void startHostCursor();

    void handleHostCursorState(const LI_HOST_CURSOR_STATE* state);

    void handleHostCursorShape(const LI_HOST_CURSOR_SHAPE* shape);

    // SDL_CODE_HOST_CURSOR_TICK: a pending automatic mouse mode switch is due.
    void handleHostCursorTick();

    // Automatic mouse mode, for the stats overlay; safe to call from any thread.
    static
    QString getMouseModeStatus();

    static
    QString getUnmappedGamepads();

private:
    enum KeyCombo {
        KeyComboQuit,
        KeyComboUngrabInput,
        KeyComboToggleFullScreen,
        KeyComboToggleStatsOverlay,
        KeyComboToggleMouseMode,
        KeyComboToggleCursorHide,
        KeyComboToggleMinimize,
        KeyComboPasteText,
        KeyComboTogglePointerRegionLock,
        KeyComboQuitAndExit,
        KeyComboToggleKeyboardGrab,
        KeyComboToggleTimingGraph,
        KeyComboToggleMouseModeLock,
        KeyComboMax
    };

    GamepadState*
    findStateForGamepad(SDL_JoystickID id);

    uint8_t moonlightControllerType(SDL_GameController* controller);

    void sendControllerArrival(GamepadState* state);

    void sendGamepadState(GamepadState* state);

    void sendGamepadBatteryState(GamepadState* state, SDL_JoystickPowerLevel level);

    void updateSteamGamepadMask();

#if HAVE_STEAM_CONTROLLER_DRIVER
    void steamControllerConnected(const std::shared_ptr<SteamControllerLink>& link, int generation);

    void steamControllerDisconnected(int linkId, int generation);
#endif

    void handleAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void emulateAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void disableTouchFeedback();

    void handleRelativeFingerEvent(SDL_TouchFingerEvent* event);

    void performSpecialKeyCombo(KeyCombo combo);

    struct HostCursorShape {
        int width;
        int height;
        int hotspotX;
        int hotspotY;
        std::vector<uint8_t> pixels;
    };

    SDL_Rect getVideoRegion();

    bool isHostCursorDrawingWanted();

    bool isHostCursorDrawnLocally();

    void updateLocalCursor(int mouseInVideoRegion = -1);

    void updateHostCursorControl();

    void warpToHostCursor(bool modeSwitch = false);

    bool isAutomaticMouseModeActive();

    void publishMouseModeStatus();

    static
    void clearMouseModeStatus();

    void evaluateCursorMode();

    void scheduleCursorModeTimer();

    void noteMouseButtons();

    void applyCursorMode(CursorModePolicy::Mode mode, CursorModePolicy::Mode previous);

    bool getHostCursorClipRect(SDL_Rect* rect);

    static
    Uint32 cursorModeTimerCallback(Uint32 interval, void* param);

    SDL_Cursor* getHostSdlCursor();

    void freeHostSdlCursor(uint32_t shapeId);

    void freeHostSdlCursors();

    static
    Uint32 longPressTimerCallback(Uint32 interval, void* param);

    static
    Uint32 mouseEmulationTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseLeftButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseRightButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 dragTimerCallback(Uint32 interval, void* param);

    SDL_Window* m_Window;
    bool m_MultiController;
    bool m_GamepadMouse;
    bool m_SwapMouseButtons;
    bool m_ReverseScrollDirection;
    bool m_SwapFaceButtons;

    bool m_MouseWasInVideoRegion;
    bool m_PendingMouseButtonsAllUpOnVideoRegionLeave;
    bool m_PointerRegionLockActive;
    bool m_PointerRegionLockToggledByUser;

    int m_GamepadMask;
    GamepadState m_GamepadState[MAX_GAMEPADS];
    QSet<short> m_KeysDown;
    bool m_FakeMouseCaptureActive;
    bool m_KeyboardCaptureActive;
    QString m_OldIgnoreDevices;
    QString m_OldIgnoreDevicesExcept;
#if HAVE_STEAM_CONTROLLER_DRIVER
    bool m_SteamHintChanged;
    QString m_OldHidapiSteamHint;
    SteamControllerHost* m_SteamHost;
    SteamControllerRunner* m_SteamController;
    std::shared_ptr<SteamControllerLink> m_SteamLinks[MAX_GAMEPADS];
    int m_SteamLinkGeneration[MAX_GAMEPADS];
#endif
    QStringList m_IgnoreDeviceGuids;
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode;
    bool m_BackgroundGamepad;
    int m_MouseCursorCapturedVisibilityState;

    struct {
        KeyCombo keyCombo;
        SDL_Keycode keyCode;
        SDL_Scancode scanCode;
        bool enabled;
    } m_SpecialKeyCombos[KeyComboMax];

    SDL_TouchFingerEvent m_LastTouchDownEvent;
    SDL_TouchFingerEvent m_LastTouchUpEvent;
    SDL_TimerID m_LongPressTimer;
    int m_StreamWidth;
    int m_StreamHeight;
    bool m_AbsoluteMouseMode;
    bool m_AbsoluteTouchMode;
    bool m_DisabledTouchFeedback;

    SDL_TouchFingerEvent m_TouchDownEvent[MAX_FINGERS];
    SDL_TimerID m_LeftButtonReleaseTimer;
    SDL_TimerID m_RightButtonReleaseTimer;
    SDL_TimerID m_DragTimer;
    char m_DragButton;
    int m_NumFingersDown;

    bool m_HostCursorSupported;
    bool m_HostCursorReady;
    LI_HOST_CURSOR_STATE m_HostCursorState;
    bool m_HostCursorHasPosition;
    int32_t m_HostCursorX;
    int32_t m_HostCursorY;
    int m_HostCursorControlSent;
    uint32_t m_HostCursorResendId;
    Uint32 m_HostCursorResendTime;
    QHash<uint32_t, HostCursorShape> m_HostCursorShapes;
    QList<uint32_t> m_HostCursorShapeOrder;
    QHash<uint32_t, SDL_Cursor*> m_HostSdlCursors;
    double m_HostSdlCursorScale;

    bool m_AutomaticMouseMode;
    bool m_FallbackAbsoluteMouseMode;
    bool m_AutomaticCaptureDone;
    CursorModePolicy::Policy m_CursorPolicy;
    CursorModePolicy::Decision m_CursorDecision;
    SDL_TimerID m_CursorModeTimer;
    uint32_t m_CursorModeTimerDue;
    bool m_ClientDrawsGraceActive;
    uint32_t m_ClientDrawsSinceMs;
    bool m_HasAbsoluteSyncPoint;
    SDL_Point m_AbsoluteSyncPoint;

    static const int k_ButtonMap[];
};
