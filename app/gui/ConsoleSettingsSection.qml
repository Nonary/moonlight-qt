import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

GroupBox {
    id: section
    padding: 0
    topPadding: label.implicitHeight + Style.px(24, Window.window)
    bottomPadding: Style.px(24, Window.window)
    background: Item { }
    label: ConsoleText { role: "heading"; text: section.title; width: section.width; leftPadding: Style.px(16, section.Window.window); rightPadding: leftPadding; wrapMode: Text.Wrap }
}
