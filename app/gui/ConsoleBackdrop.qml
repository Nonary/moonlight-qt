import QtQuick 2.9
import QtQuick.Window 2.2
import StreamingPreferences 1.0
import "ConsoleStyle.js" as Style

Item {
    id: backdrop
    property url artwork: ""
    property url settledArtwork: ""
    readonly property bool effectsAvailable: GraphicsInfo.api !== GraphicsInfo.Software
    readonly property bool useBlur: effectsAvailable && settledArtwork.toString().length > 0
                                   && !Style.placeholder(settledArtwork)
    // QtQuick.Effects is unavailable on older Qt builds, including the AppImage baseline.
    // Resolve it separately so an unavailable effect leaves the solid backdrop in place.
    readonly property Component blurComponent: useBlur ? Qt.createComponent("effects/ConsoleBackdropBlur.qml") : null
    clip: true
    Rectangle { anchors.fill: parent; color: Style.background }
    onArtworkChanged: settle.restart()
    Timer { id: settle; interval: StreamingPreferences.uiReducedMotion ? 0 : 150; onTriggered: backdrop.settledArtwork = backdrop.artwork }
    Loader {
        objectName: "consoleBackdropBlur"
        anchors.fill: parent
        sourceComponent: backdrop.blurComponent && backdrop.blurComponent.status === Component.Ready
                         ? backdrop.blurComponent : null
        onLoaded: item.artwork = Qt.binding(function() { return backdrop.settledArtwork })
    }
}
