import QtQuick 2.9
import QtQuick.Window 2.2
import QtQuick.Effects
import StreamingPreferences 1.0
import "ConsoleStyle.js" as Style

Item {
    id: backdrop
    property url artwork: ""
    property url settledArtwork: ""
    readonly property bool effectsAvailable: GraphicsInfo.api !== GraphicsInfo.Software
    clip: true
    Rectangle { anchors.fill: parent; color: Style.background }
    onArtworkChanged: settle.restart()
    Timer { id: settle; interval: StreamingPreferences.uiReducedMotion ? 0 : 150; onTriggered: backdrop.settledArtwork = backdrop.artwork }
    Loader {
        anchors.fill: parent
        active: backdrop.effectsAvailable && !Style.placeholder(backdrop.settledArtwork)
        sourceComponent: Item {
            Image {
                id: sourceImage; anchors.fill: parent
                source: backdrop.settledArtwork; sourceSize.width: 960; sourceSize.height: 640
                fillMode: Image.PreserveAspectCrop; asynchronous: true; visible: false
            }
            MultiEffect { anchors.fill: parent; source: sourceImage; blurEnabled: true; blur: 1; blurMax: 64; autoPaddingEnabled: false }
            Rectangle {
                anchors.fill: parent
                gradient: Gradient { GradientStop { position: 0; color: Style.background } GradientStop { position: 0.12; color: "#8c0b0d10" } GradientStop { position: 0.7; color: Style.background } }
            }
        }
    }
}
