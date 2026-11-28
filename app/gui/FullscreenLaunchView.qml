import QtQuick 2.9
import QtQuick.Controls 2.2
import "ConsoleStyle.js" as Style

Item {
    id: launchView
    objectName: "fullscreenLaunchView"

    property string title
    property url artwork
    property string status
    property string disconnectHint
    property bool showSpinner

    readonly property real uiScale: Style.scale(width, height)
    readonly property real sideInset: width * 0.08
    readonly property real verticalInset: Math.round(36 * uiScale)
    readonly property bool hasArtwork: !Style.placeholder(artwork) && poster.status !== Image.Error

    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    Item {
        id: body
        anchors.fill: parent
        anchors.leftMargin: launchView.sideInset
        anchors.rightMargin: launchView.sideInset
        anchors.topMargin: launchView.verticalInset
        anchors.bottomMargin: launchView.verticalInset + footer.height

        Image {
            id: poster
            objectName: "launchArtwork"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: launchView.hasArtwork ? Math.min(parent.width * 0.35, height * 0.95) : 0
            height: parent.height * 0.76
            source: Style.placeholder(launchView.artwork) ? "" : launchView.artwork
            visible: status === Image.Ready
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            Accessible.ignored: true
        }

        Column {
            id: details
            anchors.left: poster.right
            anchors.leftMargin: launchView.hasArtwork ? parent.width * 0.065 : 0
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: Math.round(22 * launchView.uiScale)

            Text {
                objectName: "launchTitle"
                width: parent.width
                text: launchView.title
                textFormat: Text.PlainText
                color: Style.text
                font.pixelSize: Math.round(58 * launchView.uiScale)
                font.weight: Font.DemiBold
                horizontalAlignment: launchView.hasArtwork ? Text.AlignLeft : Text.AlignHCenter
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }

            Item {
                id: progress
                readonly property real gap: Math.round(12 * launchView.uiScale)
                readonly property real indicatorSize: Math.round(36 * launchView.uiScale)
                width: Math.min(details.width, indicatorSize + gap + statusLabel.implicitWidth)
                height: Math.max(indicatorSize, statusLabel.implicitHeight)
                anchors.horizontalCenter: launchView.hasArtwork ? undefined : parent.horizontalCenter

                BusyIndicator {
                    id: spinner
                    objectName: "launchProgress"
                    anchors.verticalCenter: parent.verticalCenter
                    width: progress.indicatorSize
                    height: width
                    visible: launchView.showSpinner
                    running: visible
                    Accessible.ignored: true
                }

                Label {
                    id: statusLabel
                    objectName: "launchStatus"
                    anchors.left: spinner.right
                    anchors.leftMargin: progress.gap
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: launchView.status
                    textFormat: Text.PlainText
                    color: Style.muted
                    font.pixelSize: Math.round(24 * launchView.uiScale)
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }
        }
    }

    Label {
        id: footer
        objectName: "launchDisconnectHint"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: launchView.sideInset
        anchors.rightMargin: launchView.sideInset
        anchors.bottom: parent.bottom
        anchors.bottomMargin: launchView.verticalInset
        visible: text.length > 0
        text: launchView.disconnectHint
        textFormat: Text.PlainText
        color: Style.muted
        font.pixelSize: Math.round(18 * launchView.uiScale)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }
}
