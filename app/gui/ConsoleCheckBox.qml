import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

CheckBox {
    id: control
    property var settingsNavigation: null
    property Item disabledSettingsProxy: null
    readonly property bool desktop: !!settingsNavigation && settingsNavigation.desktopPreferences === true
    readonly property Item desktopHelpLink: checkHint.helpButton
    readonly property bool settingsEntry: true
    readonly property string settingsLabel: text
    readonly property string settingsHelp: description
    Accessible.description: settingsHelp
    function activateSetting() { toggle(); toggled(); clicked() }
    Keys.onPressed: function(event) { event.accepted = settingsNavigation ? settingsNavigation.handleRowKey(control, event) : false }
    onActiveFocusChanged: if (activeFocus && settingsNavigation) settingsNavigation.rowFocused(control)
    property string description: ToolTip.text
    implicitHeight: Math.max(desktop ? 48 : Style.px(64, Window.window), labels.implicitHeight + topPadding + bottomPadding)
    leftPadding: Style.px(16, Window.window); rightPadding: Style.px(64, Window.window)
    topPadding: desktop ? 11 : Style.px(16, Window.window); bottomPadding: topPadding
    background: Rectangle {
        color: control.activeFocus ? Style.control : control.desktop && control.settingsNavigation.hoverRow === control ? "#232a32" : "transparent"; radius: Style.px(6, control.Window.window)
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Style.outline }
        ConsoleFocus { focused: control.activeFocus; radius: Style.px(6, control.Window.window) }
    }
    indicator: Rectangle {
        x: control.width - width - Style.px(16, control.Window.window); y: (control.height-height)/2
        width: control.desktop ? 20 : Style.px(24, control.Window.window); height: width; radius: Style.px(4, control.Window.window)
        color: control.checked ? (control.desktop ? "#9bcfff" : Style.text) : "transparent"; border.color: control.enabled ? Style.muted : Style.disabled; border.width: 1
        Canvas {
            anchors.fill: parent; visible: control.checked
            onPaint: { var c=getContext("2d"); c.reset(); c.strokeStyle=Style.background; c.lineWidth=2; c.beginPath(); c.moveTo(width*.23,height*.5); c.lineTo(width*.43,height*.7); c.lineTo(width*.78,height*.28); c.stroke() }
            onWidthChanged: requestPaint()
        }
    }
    contentItem: Item {
        implicitHeight: labels.implicitHeight
        Column {
        id: labels; width: parent.width; anchors.verticalCenter: parent.verticalCenter; spacing: control.desktop ? 4 : Style.px(8, control.Window.window)
        ConsoleText { width: parent.width; font.pixelSize: control.desktop ? 16 : Style.typeSize("body", control.Window.window); text: control.text; wrapMode: Text.Wrap; color: control.enabled ? Style.text : Style.disabled }
        DesktopSettingHint {
            id: checkHint
            row: control
            visible: control.desktop && (!!fullText || unavailable)
            width: parent.width
            height: implicitHeight
        }
        ConsoleText { width: Math.min(parent.width, Style.px(560, control.Window.window)); text: control.description; role: "secondary"; wrapMode: Text.Wrap; visible: !control.settingsNavigation && text.length > 0 }
        }
    }
}
