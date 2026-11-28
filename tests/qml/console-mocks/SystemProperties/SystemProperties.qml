pragma Singleton
import QtQuick 2.9
QtObject {
    property bool usesMaterial3Theme: true
    enum BenqGr10State { Gr10Absent, Gr10NeedsHelper, Gr10Ready }
    property int mockGr10State: SystemProperties.Gr10Absent
    function benqGr10State() { return mockGr10State }

    property bool isWow64: false
    property bool hasHardwareAcceleration: true
    property bool isRunningXWayland: false
    property string unmappedGamepads: ""
    property string versionString: "Test build"
    function startAsyncLoad() {}
    function waitForAsyncLoad() {}

    property bool hasDesktopEnvironment: true
    property bool hasDiscordIntegration: true
    property bool hasPyroWave: true
    property bool isDarwin: false
    property bool rendererAlwaysFullScreen: false
    property bool supportsHdr: true
    property bool hasBrowser: true
    property size maximumResolution: Qt.size(7680, 4320)
    function refreshDisplays() {}
    function getNativeResolution(index) { return index === 0 ? Qt.rect(0, 0, 1920, 1080) : Qt.rect(0, 0, 0, 0) }
    function getSafeAreaResolution(index) { return getNativeResolution(index) }
    function getRefreshRate(index) { return index === 0 ? 120 : 0 }
    property int mockUltraLowLatencyVrrFpsLimit: 116
    function getUltraLowLatencyVrrFpsLimit() { return mockUltraLowLatencyVrrFpsLimit }
}
