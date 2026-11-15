import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

CheckBox {
    id: control
    property var settingsNavigation: null
    property Item disabledSettingsProxy: null
    readonly property bool settingsEntry: true
    readonly property string settingsLabel: text
    readonly property string settingsHelp: description
    function activateSetting() { toggle(); toggled(); clicked() }
    Keys.onPressed: function(event) { event.accepted = settingsNavigation ? settingsNavigation.handleRowKey(control, event) : false }
    onActiveFocusChanged: if (activeFocus && settingsNavigation) settingsNavigation.rowFocused(control)
    property string description: ToolTip.text
    implicitHeight: Math.max(Style.px(64, Window.window), labels.implicitHeight + topPadding + bottomPadding)
    leftPadding: Style.px(16, Window.window); rightPadding: Style.px(64, Window.window)
    topPadding: Style.px(16, Window.window); bottomPadding: topPadding
    background: Rectangle {
        color: control.activeFocus ? Style.control : "transparent"; radius: Style.px(6, control.Window.window)
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Style.outline }
        ConsoleFocus { focused: control.activeFocus; radius: Style.px(6, control.Window.window) }
    }
    indicator: Rectangle {
        x: control.width - width - Style.px(16, control.Window.window); y: (control.height-height)/2
        width: Style.px(24, control.Window.window); height: width; radius: Style.px(4, control.Window.window)
        color: control.checked ? Style.text : "transparent"; border.color: control.enabled ? Style.muted : Style.disabled; border.width: 1
        Canvas {
            anchors.fill: parent; visible: control.checked
            onPaint: { var c=getContext("2d"); c.reset(); c.strokeStyle=Style.background; c.lineWidth=2; c.beginPath(); c.moveTo(width*.23,height*.5); c.lineTo(width*.43,height*.7); c.lineTo(width*.78,height*.28); c.stroke() }
            onWidthChanged: requestPaint()
        }
    }
    contentItem: Item {
        implicitHeight: labels.implicitHeight
        Column {
        id: labels; width: parent.width; anchors.verticalCenter: parent.verticalCenter; spacing: Style.px(8, control.Window.window)
        ConsoleText { width: parent.width; text: control.text; wrapMode: Text.Wrap; color: control.enabled ? Style.text : Style.disabled }
        ConsoleText { width: Math.min(parent.width, Style.px(560, control.Window.window)); text: control.description; role: "secondary"; wrapMode: Text.Wrap; visible: !control.settingsNavigation && text.length > 0 }
        }
    }
}
