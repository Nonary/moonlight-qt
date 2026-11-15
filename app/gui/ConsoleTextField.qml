import QtQuick 2.9
import QtQuick.Controls 2.5
import SdlGamepadKeyNavigation 1.0
import QtQuick.Window 2.2
import "ConsoleKeyboardService.js" as Keyboard
import "ConsoleStyle.js" as Style

TextField {
    id: field
    selectByMouse: true
    property bool addressInput: false
    font.family: "Inter"
    font.pixelSize: Style.typeSize("body", Window.window)
    property bool controllerEditing: SdlGamepadKeyNavigation.inputMethod === "controller"
    function openKeyboard() { Keyboard.openFor(field, Window.window, addressInput || (inputMethodHints & Qt.ImhDigitsOnly) !== 0, null) }
    Keys.onPressed: function(event) {
        event.accepted = false
        if (controllerEditing && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)) {
            openKeyboard()
            event.accepted = true
        }
    }
}
