import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

Slider {
    id: control
    property var settingsRowHost: null
    signal settingsCommitted(real newValue)
    function openSettingsEditor(row) { row.settingsNavigation.openValueEditor(row, control, true) }
    Keys.onPressed: function(event) { event.accepted = settingsRowHost && settingsRowHost.settingsNavigation ? settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event) : false }
    implicitHeight: Style.px(48, Window.window)
    background: Rectangle {
        visible: !control.settingsRowHost
        x: control.leftPadding; y: control.topPadding + control.availableHeight / 2 - height/2
        width: control.availableWidth; height: Style.px(4, control.Window.window); radius: height/2; color: Style.control
        Rectangle { width: control.visualPosition * parent.width; height: parent.height; radius: parent.radius; color: control.enabled ? Style.text : Style.disabled }
    }
    handle: Rectangle {
        visible: !control.settingsRowHost
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height)/2
        width: Style.px(20, control.Window.window); height: width; radius: width/2; color: control.enabled ? Style.text : Style.disabled
        ConsoleFocus { focused: control.activeFocus; radius: parent.radius }
    }
    ConsoleText { anchors.fill: parent; visible: !!control.settingsRowHost; text: qsTr("%1 Mbps").arg(control.value / 1000); horizontalAlignment: Text.AlignRight; verticalAlignment: Text.AlignVCenter }
    MouseArea { anchors.fill: parent; visible: !!control.settingsRowHost; onClicked: control.openSettingsEditor(control.settingsRowHost) }
}
