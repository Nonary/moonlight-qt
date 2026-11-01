import QtQuick 2.0
import QtQuick.Controls 2.5
import "PopupFocus.js" as PopupFocus

Dialog {
    id: dialog
    modal: true
    anchors.centerIn: Overlay.overlay

    onVisibleChanged: PopupFocus.setDialogVisible(dialog, visible)
    Component.onDestruction: {
        PopupFocus.setDialogVisible(dialog, false)
        if (stackView && stackView.focusViewAfterDialog) {
            PopupFocus.restoreViewFocus(stackView)
        }
    }

    onClosed: {
        // Keep focus in another open popup, such as an error shown while
        // a pairing dialog is closing. Otherwise restore view navigation.
        PopupFocus.restoreViewFocus(stackView)
    }
}
