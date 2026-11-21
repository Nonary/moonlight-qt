import QtQuick 2.9
import "ConsoleStyle.js" as Style
import QtQuick.Controls 2.2
import QtQuick.Window 2.2
import QtQuick.Layouts 1.2

Column {
    id: root
    spacing: 0
    property int paceMbps: 0
    signal paceEdited(int value)

    ConsoleSettingRow {
        width: parent.width
        labelItem: ConsoleText { text: qsTr("Packet speed (Mbps)") }
        description: qsTr("Controls how fast each PyroWave frame is sent. Lower it to reduce packet loss. Zero lets the host choose. Calibration replaces this value when you apply a result. Changes apply to the next stream.")
        editor: ConsoleSpinBox {
            id: speed
            objectName: "pyrowavePacketSpeed"
            from: 0
            to: 9999
            stepSize: 50
            editable: true
            value: root.paceMbps
            onValueModified: root.paceEdited(value)
            onSettingsCommitted: function(newValue) { root.paceEdited(newValue) }
            Keys.onLeftPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) root.paceEdited(Math.max(from, value - stepSize)) }
            Keys.onRightPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) root.paceEdited(Math.min(to, value + stepSize)) }
            Accessible.name: qsTr("PyroWave packet speed in Mbps; zero is Automatic")
        }
    }
    ConsoleListRow {
        objectName: "pyrowavePacketSpeedAutomatic"
        text: qsTr("Use automatic packet speed")
        property string unavailableReason: qsTr("Packet speed is already automatic.")
        enabled: root.paceMbps !== 0
        onClicked: root.paceEdited(0)
    }
}
