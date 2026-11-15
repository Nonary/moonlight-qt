import QtQuick 2.9
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

ConsoleButton {
    implicitHeight: Style.px(48, Window.window)
    textOnly: true
    rail: true
    textAlignment: Text.AlignLeft
}
