import QtQuick 2.0
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import SdlGamepadKeyNavigation 1.0
import "PopupFocus.js" as PopupFocus
import "ConsoleStyle.js" as Style

Dialog {
    id: dialog
    readonly property var hostWindow: parent ? parent.Window.window : null
    property bool previousNavigationMode: false
    property bool tabNavigation: true
    modal: true
    padding: Style.px(32, hostWindow)
    font.family: "Inter"
    font.pixelSize: Style.typeSize("body", hostWindow)
    background: Rectangle { color: Style.raised; radius: Style.px(10, dialog.hostWindow); border.color: Style.outline; border.width: 1 }
    Overlay.modal: Rectangle { color: "#99000000" }
    anchors.centerIn: Overlay.overlay
    topPadding: title.length ? 0 : Style.px(32, hostWindow)
    header: ConsoleText {
        text: dialog.title; role: "title"; visible: text.length > 0; wrapMode: Text.Wrap
        leftPadding: dialog.leftPadding; rightPadding: dialog.rightPadding
        topPadding: Style.px(32, dialog.hostWindow); bottomPadding: Style.px(24, dialog.hostWindow)
    }

    onAboutToShow: {
        previousNavigationMode = SdlGamepadKeyNavigation.currentUiNavMode()
        SdlGamepadKeyNavigation.setUiNavMode(tabNavigation)
    }

    footer: DialogButtonBox {
        standardButtons: dialog.standardButtons
        spacing: Style.px(12, dialog.hostWindow)
        padding: Style.px(32, dialog.hostWindow)
        topPadding: 0
        bottomPadding: Style.px(32, dialog.hostWindow)
        background: Item { }
        delegate: ConsoleButton {
            Keys.onRightPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocusReason)
            Keys.onLeftPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocusReason)
        }
    }
    onOpened: {
        if (footer && typeof footer.standardButton === "function") {
            var button = footer.standardButton(Dialog.No) || footer.standardButton(Dialog.Cancel) || footer.standardButton(Dialog.Close) || footer.standardButton(Dialog.Ok)
            if (button) button.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    onVisibleChanged: PopupFocus.setDialogVisible(dialog, visible)
    Component.onDestruction: {
        PopupFocus.setDialogVisible(dialog, false)
        if (typeof stackView !== "undefined" && stackView && stackView.focusViewAfterDialog) {
            if (typeof stackView !== "undefined") PopupFocus.restoreViewFocus(stackView)
        }
    }

    onClosed: {
        SdlGamepadKeyNavigation.setUiNavMode(previousNavigationMode)
        // Keep focus in another open popup, such as an error shown while
        // a pairing dialog is closing. Otherwise restore view navigation.
        if (typeof stackView !== "undefined") PopupFocus.restoreViewFocus(stackView)
    }
}
