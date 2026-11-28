import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

GroupBox {
    id: section
    readonly property bool desktop: typeof settingsShell !== "undefined" && settingsShell.desktopPreferences === true
    padding: 0
    topPadding: label.implicitHeight + (desktop ? 10 : Style.px(24, Window.window))
    bottomPadding: Style.px(24, Window.window)
    background: Item { }
    label: ConsoleText { role: "heading"; font.pixelSize: section.desktop ? 22 : Style.typeSize("heading", section.Window.window); font.weight: section.desktop ? Font.DemiBold : Font.Medium; text: section.title; width: section.width; leftPadding: Style.px(16, section.Window.window); rightPadding: leftPadding; wrapMode: Text.Wrap }
}
