pragma Singleton
import QtQuick 2.9

QtObject {
    property bool hasActiveProfile: true
    property string activeProfileId: "default"
    property string activeProfileName: "Default"
    property string defaultProfileId: "default"
    property string autoLoginProfileId: "default"
    property bool autoLoginEnabled: true
    property var profiles: [
        { id: "default", name: "Default", defaultProfile: true, autoLogin: true },
        { id: "second", name: "Second", defaultProfile: false, autoLogin: false }
    ]
    property int activationCount: 0
    function activateProfile(id) {
        activationCount++
        activeProfileId = id
        activeProfileName = id === "default" ? "Default" : "Second"
        return true
    }
    function setAutoLoginEnabled(enabled) { autoLoginEnabled = enabled }
    function retranslate() {}

    property string defaultHostUuid: ""
    signal computerAddCompleted(bool success, bool detectedPortBlocking)
    function reloadForActiveProfile() {}
    function startPolling() {}
    function stopPollingAsync() {}

    property bool uiNavMode: false
    function enable() {}
    function disable() {}
    function notifyWindowFocus(focused) {}
    function getConnectedGamepads() { return 1 }
    function setUiNavMode(enabled) { uiNavMode = enabled }

    property bool usesMaterial3Theme: true
    property bool hasDesktopEnvironment: true
    property bool hasBrowser: false
    property bool hasDiscordIntegration: false
    property bool isDarwin: false
    property bool rendererAlwaysFullScreen: false
    property bool supportsHdr: true
    property bool hasPyroWave: false
    property size maximumResolution: Qt.size(7680, 4320)
    function refreshDisplays() {}
    function getNativeResolution(index) { return index === 0 ? Qt.rect(0, 0, 1920, 1080) : Qt.rect(0, 0, 0, 0) }
    function getSafeAreaResolution(index) { return getNativeResolution(index) }
    function getRefreshRate(index) { return index === 0 ? 60 : 0 }
    property bool enableMdns: false
    property string versionString: "test"
    property string friendlyNativeArchName: "test"
    property int uiDisplayMode: 0
    enum DisplayMode { UI_WINDOWED, UI_MAXIMIZED, UI_FULLSCREEN }
    signal updateAvailable(string version, string url)
    function start() {}

    // Target-specific services are idle fixtures. Navigation never calibrates
    // a decoder or changes the machine's network settings.
    property bool running: false
    property string message: ""
    property string linkSummary: ""
    property var results: []
    function cancel() {}
    property bool needsFix: false
    property bool canApply: false
    property bool busy: false
    property string problemText: ""
    property string fixDescription: ""
    property string manualHint: ""
    property string manualCommand: ""
    function refresh() {}
    function apply() {}
    function copyCommand() {}
}
