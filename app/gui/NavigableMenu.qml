import QtQuick 2.0
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

Menu {
    readonly property var hostWindow: parent ? parent.Window.window : null
    background: Rectangle {
        implicitWidth: Style.px(280, hostWindow)
        color: Style.raised
        radius: Style.px(10, hostWindow)
        border.color: Style.outline
    }
    font.family: "Inter"
    font.pixelSize: Style.typeSize("body", hostWindow)
    property var initiator

    onOpened: {
        // If the initiating object currently has keyboard focus,
        // give focus to the first visible and enabled menu item
        if (initiator) {
            for (var i = 0; i < count; i++) {
                var item = itemAt(i)
                if (item.visible && item.enabled) {
                    item.forceActiveFocus(Qt.TabFocusReason)
                    break
                }
            }
        }
    }
}
