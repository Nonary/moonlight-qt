import QtQuick 2.9
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

Item {
    id: glyph
    property string key: ""
    property bool gamepad: false
    implicitWidth: gamepad ? Style.px(24, Window.window) : label.implicitWidth
    implicitHeight: Style.px(24, Window.window)
    Canvas {
        id: shape
        anchors.fill: parent
        visible: glyph.gamepad
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var c = getContext("2d"); c.reset(); c.strokeStyle = Style.muted; c.lineWidth = Math.max(1.5, width / 14)
            var a = width * 0.18, b = width * 0.82, mid = width / 2
            c.beginPath()
            if (glyph.key === "×") { c.moveTo(a,a); c.lineTo(b,b); c.moveTo(b,a); c.lineTo(a,b) }
            else if (glyph.key === "□") c.rect(a,a,b-a,b-a)
            else if (glyph.key === "△") { c.moveTo(mid,a); c.lineTo(b,b); c.lineTo(a,b); c.closePath() }
            else c.arc(mid,mid,width * 0.42,0,Math.PI*2)
            c.stroke()
        }
        Connections { target: glyph; function onKeyChanged() { shape.requestPaint() } }
    }
    ConsoleText {
        id: label; anchors.centerIn: parent; role: "caption"
        text: glyph.gamepad && ["×","○","□","△"].indexOf(glyph.key) >= 0 ? "" : glyph.key
    }
}
