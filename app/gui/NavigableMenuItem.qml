import QtQuick 2.0
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

MenuItem {
    id: control
    implicitHeight: Style.px(48, Window.window)
    leftPadding: Style.px(16, Window.window); rightPadding: leftPadding
    contentItem: ConsoleText { text: control.text; font.weight: Font.Medium; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; color: !control.enabled ? Style.disabled : Style.text }
    background: Rectangle { color: control.activeFocus || control.hovered ? Style.control : "transparent"; radius: Style.px(6, control.Window.window); ConsoleFocus { focused: control.activeFocus; radius: parent.radius } }
    // Ensure focus can't be given to an invisible item
    enabled: visible
    height: visible ? implicitHeight : 0
    focusPolicy: visible ? Qt.TabFocus : Qt.NoFocus

    onTriggered: {
        // We must close the context menu first or
        // it can steal focus from any dialogs that
        // onTriggered may spawn.
        menu.close()
    }

    Keys.onReturnPressed: {
        triggered()
    }

    Keys.onEnterPressed: {
        triggered()
    }

    Keys.onEscapePressed: {
        menu.close()
    }
}
