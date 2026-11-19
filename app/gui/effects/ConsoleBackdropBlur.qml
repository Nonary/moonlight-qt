import QtQuick 2.9
import QtQuick.Effects
import "../ConsoleStyle.js" as Style

Item {
    id: blurLayer
    property url artwork: ""

    Image {
        id: sourceImage
        anchors.fill: parent
        source: blurLayer.artwork
        sourceSize.width: 960
        sourceSize.height: 640
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        visible: false
    }

    MultiEffect {
        anchors.fill: parent
        source: sourceImage
        blurEnabled: true
        blur: 1
        blurMax: 64
        autoPaddingEnabled: false
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Style.background }
            GradientStop { position: 0.12; color: "#8c0b0d10" }
            GradientStop { position: 0.7; color: Style.background }
        }
    }
}
