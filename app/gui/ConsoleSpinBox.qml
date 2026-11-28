import QtQuick 2.9
import QtQuick.Controls 2.5
import SdlGamepadKeyNavigation 1.0
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style
import "ConsoleKeyboardService.js" as Keyboard

SpinBox {
    id: control
    property var settingsRowHost: null
    readonly property bool desktopControl: !!settingsRowHost && settingsRowHost.desktop === true
    readonly property bool desktopEditing: desktopControl && (activeFocus || contentItem.activeFocus)
    // The VRR spin boxes install controller arrows on both the outer item and
    // its text input. Leave cursor motion and stepping to Qt in the desktop form.
    Keys.enabled: !desktopControl
    signal settingsCommitted(real newValue)
    function openSettingsEditor(row) { row.settingsNavigation.openValueEditor(row, control, false) }
    implicitHeight: Style.px(48, Window.window)
    font.family: "Inter"
    font.pixelSize: Style.typeSize("body", Window.window)
    readonly property bool rowBrowsing: !!settingsRowHost && !!settingsRowHost.settingsNavigation && !settingsRowHost.desktop
    up.indicator.opacity: rowBrowsing ? 0 : 1
    down.indicator.opacity: rowBrowsing ? 0 : 1
    background: Rectangle { color: control.rowBrowsing ? "transparent" : Style.control; radius: Style.px(6, control.Window.window) }
    ConsoleText { anchors.fill: parent; visible: control.rowBrowsing; text: control.textFromValue(control.value, control.locale); horizontalAlignment: Text.AlignRight; verticalAlignment: Text.AlignVCenter; color: control.enabled ? Style.text : Style.disabled }
    MouseArea { anchors.fill: parent; z: 1; visible: control.rowBrowsing; onClicked: control.openSettingsEditor(control.settingsRowHost) }
    function controllerEntry(event) {
        event.accepted = false
        if (settingsRowHost && settingsRowHost.settingsNavigation && settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) { event.accepted = true; return }
        if (editable && SdlGamepadKeyNavigation.inputMethod === "controller" &&
                (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)) {
            Keyboard.openFor(contentItem, Window.window, true, function(text) {
                var parsed = control.valueFromText(text, control.locale)
                if (isFinite(parsed)) {
                    control.value = Math.max(control.from, Math.min(control.to, parsed))
                    control.valueModified()
                }
            })
            event.accepted = true
        }
    }
    Keys.onPressed: function(event) { controllerEntry(event) }
    Component.onCompleted: {
        contentItem.opacity = Qt.binding(function() { return control.rowBrowsing ? 0 : 1 })
        contentItem.enabled = Qt.binding(function() { return !control.rowBrowsing })
        contentItem.Keys.enabled = Qt.binding(function() { return !control.desktopControl })
        contentItem.Keys.priority = Keys.BeforeItem
        contentItem.Keys.pressed.connect(controllerEntry)
    }
}
