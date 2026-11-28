import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

Slider {
    id: control
    property var settingsRowHost: null
    readonly property bool desktop: !!settingsRowHost && settingsRowHost.desktop === true
    property string valueText: qsTr("%1 Mbps").arg(value / 1000)
    signal settingsCommitted(real newValue)
    function openSettingsEditor(row) { row.settingsNavigation.openValueEditor(row, control, true) }
    Keys.onPressed: function(event) { event.accepted = settingsRowHost && settingsRowHost.settingsNavigation ? settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event) : false }
    implicitHeight: desktop ? 44 : Style.px(48, Window.window)
    Binding { target: control; property: "topPadding"; when: control.desktop; value: 20 }
    background: Rectangle {
        visible: control.desktop || !control.settingsRowHost
        x: control.leftPadding; y: control.topPadding + control.availableHeight / 2 - height/2
        width: control.availableWidth; height: Style.px(4, control.Window.window); radius: height/2; color: Style.control
        Rectangle { width: control.visualPosition * parent.width; height: parent.height; radius: parent.radius; color: control.enabled ? Style.text : Style.disabled }
    }
    handle: Rectangle {
        visible: control.desktop || !control.settingsRowHost
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height)/2
        width: Style.px(20, control.Window.window); height: width; radius: width/2; color: control.enabled ? Style.text : Style.disabled
        ConsoleFocus { focused: control.activeFocus; radius: parent.radius }
    }
    ConsoleText { anchors.fill: parent; font.pixelSize: control.desktop ? 14 : Style.typeSize("body", control.Window.window); visible: !!control.settingsRowHost; text: control.valueText; horizontalAlignment: Text.AlignRight; verticalAlignment: control.desktop ? Text.AlignTop : Text.AlignVCenter }
    MouseArea { anchors.fill: parent; visible: !!control.settingsRowHost && !control.desktop; onClicked: control.openSettingsEditor(control.settingsRowHost) }
}
