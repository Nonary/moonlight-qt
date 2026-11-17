import QtQuick 2.9
import QtQuick.Templates 2.5 as T
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import SdlGamepadKeyNavigation 1.0
import "ConsoleStyle.js" as Style

T.Button {
    id: control
    property var settingsNavigation: null
    property Item disabledSettingsProxy: null
    readonly property bool settingsEntry: true
    readonly property bool settingsAction: true
    readonly property string settingsLabel: text
    readonly property string settingsHelp: ToolTip.text
    function activateSetting() { clicked() }
    Keys.onPressed: function(event) { event.accepted = settingsNavigation ? settingsNavigation.handleRowKey(control, event) : false }
    property bool primary: false
    property bool selected: primary
    property bool textOnly: false
    property bool rail: false
    property int textAlignment: Text.AlignHCenter
    readonly property bool focusVisible: activeFocus && SdlGamepadKeyNavigation.inputMethod !== "mouse" && SdlGamepadKeyNavigation.inputMethod !== "touch"
    implicitHeight: Style.px(48, Window.window)
    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    leftPadding: Style.px(16, Window.window)
    rightPadding: leftPadding
    topPadding: 0
    bottomPadding: 0
    topInset: 0; bottomInset: 0; leftInset: 0; rightInset: 0
    font.family: "Inter"
    font.pixelSize: Style.typeSize("body", Window.window)
    font.weight: Font.Medium
    activeFocusOnTab: true
    hoverEnabled: true
    contentItem: Text {
        text: control.text
        font: control.font
        renderType: Text.NativeRendering
        color: !control.enabled ? Style.disabled : control.focusVisible || control.selected ? Style.text : Style.muted
        horizontalAlignment: control.textAlignment
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: Style.px(6, control.Window.window)
        color: control.down ? Qt.lighter(Style.control, 1.65) : control.hovered || control.focusVisible ? Qt.lighter(Style.control, 1.3) : control.selected ? Style.raised : control.textOnly ? "transparent" : Style.control
        ConsoleFocus { focused: control.activeFocus; radius: parent.radius }
        Rectangle {
            visible: control.selected && !control.focusVisible
            color: Style.text
            x: control.rail ? 0 : control.leftPadding
            y: control.rail ? Style.px(10, control.Window.window) : parent.height - Style.px(2, control.Window.window)
            width: control.rail ? Style.px(3, control.Window.window) : parent.width - control.leftPadding - control.rightPadding
            height: control.rail ? parent.height - 2 * y : Style.px(2, control.Window.window)
        }
    }
    Keys.onReturnPressed: { if (settingsNavigation) settingsNavigation.activateRow(control); else clicked() }
    Keys.onEnterPressed: { if (settingsNavigation) settingsNavigation.activateRow(control); else clicked() }
}
