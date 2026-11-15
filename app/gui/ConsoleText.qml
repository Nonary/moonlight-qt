import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

Label {
    property string role: "body"
    font.family: "Inter"
    font.pixelSize: Style.typeSize(role, Window.window)
    font.weight: role === "display" || role === "title" ? Font.DemiBold : role === "heading" ? Font.Medium : Font.Normal
    color: role === "secondary" ? Style.muted : role === "caption" ? Style.caption : Style.text
    renderType: Text.NativeRendering
}
