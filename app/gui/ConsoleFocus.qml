import QtQuick 2.9
import QtQuick.Window 2.2
import SdlGamepadKeyNavigation 1.0
import "ConsoleStyle.js" as Style

Rectangle {
    property bool focused: false
    anchors.fill: parent
    anchors.margins: Style.px(2, Window.window)
    color: "transparent"
    border.width: Style.px(2, Window.window)
    border.color: Style.accent
    visible: focused && SdlGamepadKeyNavigation.inputMethod !== "mouse" && SdlGamepadKeyNavigation.inputMethod !== "touch"
}
