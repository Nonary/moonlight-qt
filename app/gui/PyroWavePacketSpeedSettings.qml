import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

Column {
    id: root
    spacing: 5
    property int paceMbps: 0
    signal paceEdited(int value)

    Label {
        width: parent.width
        text: qsTr("Packet speed (Mbps)")
        font.pointSize: 12
        wrapMode: Text.Wrap
    }
    Label {
        width: parent.width
        text: qsTr("Controls how fast each PyroWave frame is sent. Lower it to reduce shimmering from packet loss. Video bitrate controls the amount of data. Changes apply to the next stream.")
        font.pointSize: 9
        wrapMode: Text.Wrap
    }
    RowLayout {
        width: parent.width
        spacing: 8
        SpinBox {
            id: speed
            objectName: "pyrowavePacketSpeed"
            Layout.fillWidth: true
            Layout.maximumWidth: 240
            from: 0
            to: 9999
            stepSize: 50
            editable: true
            value: root.paceMbps
            Component.onCompleted: contentItem.Keys.forwardTo = [speed]
            onValueModified: root.paceEdited(value)
            Keys.onLeftPressed: root.paceEdited(Math.max(from, value - stepSize))
            Keys.onRightPressed: root.paceEdited(Math.min(to, value + stepSize))
            Accessible.name: qsTr("PyroWave packet speed in Mbps; zero is Automatic")
        }
        Button {
            objectName: "pyrowavePacketSpeedAutomatic"
            text: qsTr("Automatic")
            enabled: root.paceMbps !== 0
            onClicked: root.paceEdited(0)
        }
    }
    Label {
        width: parent.width
        font.pointSize: 9
        wrapMode: Text.Wrap
        text: root.paceMbps === 0 ?
                  qsTr("0 = Automatic: the host chooses the packet speed from the link. Calibration sets a measured speed here.") :
                  qsTr("Selected: %1 Mbps. The host caps this at its link speed and raises it if needed to carry the video bitrate. Calibration replaces this value when you apply a result.").arg(root.paceMbps)
    }
}
