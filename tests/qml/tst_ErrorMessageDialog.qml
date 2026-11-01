import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import QtTest 1.2
import SystemProperties 1.0
import "../../app/gui"
import "../../app/gui/PopupFocus.js" as PopupFocus

TestCase {
    id: testCase
    name: "ErrorMessageDialog"
    when: windowShown
    width: 640
    height: 480

    // Match the application's view-focus behavior during navigation.
    StackView {
        id: stackView
        property bool focusViewAfterDialog: false
        width: parent.width
        height: parent.height
        focus: true
        initialItem: view
        onCurrentItemChanged: PopupFocus.focusCurrentView(stackView)
        Window.onActiveFocusItemChanged: PopupFocus.keepModalFocus(Overlay.overlay)
    }
    Component {
        id: view
        Item {
            property int backgroundKeyCount: 0
            Keys.onReturnPressed: backgroundKeyCount++
            Keys.onDownPressed: backgroundKeyCount++
            Keys.onTabPressed: backgroundKeyCount++
        }
    }

    ErrorMessageDialog {
        id: errorDialog
        text: "Connection failed"
        imageSrc: ""
        property int acceptedCount: 0
        property int closedCount: 0
        onAccepted: acceptedCount++
        onClosed: closedCount++
    }

    NavigableMessageDialog {
        id: pairingDialog
        text: "Pairing"
        imageSrc: ""
        standardButtons: Dialog.Cancel
    }

    NavigableMessageDialog {
        id: confirmationDialog
        text: "Are you sure?"
        imageSrc: ""
        standardButtons: Dialog.Yes | Dialog.No
        property int acceptedCount: 0
        property bool navigateOnAccept: false
        onAccepted: {
            acceptedCount++
            if (navigateOnAccept) stackView.push(view, StackView.Immediate)
        }
    }

    NavigableDialog {
        id: formDialog
        property int menuActivated: 0
        standardButtons: Dialog.Close
        onOpened: formField.forceActiveFocus()
        Column {
            TextField { id: formField }
            AutoResizingComboBox {
                id: formCombo
                width: 200
                model: ["First", "Second"]
            }
            Menu {
                id: formMenu
                MenuItem {
                    id: formMenuItem
                    text: "Form action"
                    onTriggered: formDialog.menuActivated++
                }
            }
        }
    }

    Menu {
        id: navigationMenu
        MenuItem {
            id: navigationMenuItem
            text: "Open another view"
            onTriggered: stackView.push(view, StackView.Immediate)
        }
    }

    Component {
        id: transientError
        ErrorMessageDialog { text: "Temporary error"; imageSrc: "" }
    }

    Component {
        id: errorView
        Item {
            property alias error: viewError
            ErrorMessageDialog { id: viewError; text: "Quit failed"; imageSrc: "" }
        }
    }

    function init() {
        SystemProperties.hasBrowser = true
        errorDialog.acceptedCount = 0
        errorDialog.closedCount = 0
        confirmationDialog.navigateOnAccept = false
        formDialog.menuActivated = 0
        formField.text = ""
        stackView.forceActiveFocus()
    }

    function cleanup() {
        errorDialog.close()
        pairingDialog.close()
        confirmationDialog.close()
        formCombo.popup.close()
        formMenu.close()
        formDialog.close()
        navigationMenu.close()
        tryCompare(errorDialog, "visible", false)
        stackView.pop(null, StackView.Immediate)
    }

    function test_confirm_data() {
        return [
            { tag: "return-with-help", browser: true, key: Qt.Key_Return },
            { tag: "space-with-help", browser: true, key: Qt.Key_Space },
            { tag: "return-without-help", browser: false, key: Qt.Key_Return },
            { tag: "space-without-help", browser: false, key: Qt.Key_Space }
        ]
    }

    function test_confirm(data) {
        SystemProperties.hasBrowser = data.browser
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        var ok = errorDialog.standardButton(Dialog.Ok)
        verify(ok && ok.activeFocus, "Controller confirmation should initially target OK")
        keyClick(data.key) // Controller A sends Return or Space, depending on navigation mode.
        tryCompare(errorDialog, "visible", false)
        compare(errorDialog.acceptedCount, 1)
        compare(errorDialog.closedCount, 1)
        verify(stackView.activeFocus, "Navigation focus should return after the error closes")
    }

    function test_cancel() {
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        keyClick(Qt.Key_Escape) // Controller B.
        tryCompare(errorDialog, "visible", false)
        compare(errorDialog.closedCount, 1)
    }

    function test_confirmation_dialog_keeps_its_default_focus() {
        confirmationDialog.acceptedCount = 0
        confirmationDialog.open()
        tryCompare(confirmationDialog, "opened", true)
        var buttons = confirmationDialog.footer
        verify(buttons.itemAt(buttons.count - 1).activeFocus)
        keyClick(Qt.Key_Escape)
        tryCompare(confirmationDialog, "visible", false)
        compare(confirmationDialog.acceptedCount, 0)
    }

    function test_confirmation_can_open_a_new_view() {
        confirmationDialog.navigateOnAccept = true
        confirmationDialog.open()
        tryCompare(confirmationDialog, "opened", true)
        confirmationDialog.accept()
        tryCompare(confirmationDialog, "visible", false)
        compare(stackView.depth, 2)
        verify(stackView.currentItem.activeFocus)
    }

    function test_help_remains_reachable_and_reopen_resets_focus() {
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        var ok = errorDialog.standardButton(Dialog.Ok)
        var help = errorDialog.standardButton(Dialog.Help)
        verify(ok && ok.activeFocus)
        verify(help)
        keyClick(Qt.Key_Right)
        tryVerify(function() { return help.activeFocus })
        keyClick(Qt.Key_Escape)
        tryCompare(errorDialog, "visible", false)
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        verify(ok.activeFocus)
    }

    function test_error_stays_focused_when_previous_dialog_closes() {
        pairingDialog.open()
        tryCompare(pairingDialog, "opened", true)
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        pairingDialog.close()
        tryCompare(pairingDialog, "visible", false)
        verify(errorDialog.standardButton(Dialog.Ok).activeFocus,
               "Closing an earlier dialog should not take focus from the error")
        keyClick(Qt.Key_Return)
        tryCompare(errorDialog, "visible", false)
        verify(stackView.activeFocus)
    }

    function test_error_opened_while_previous_dialog_is_closing() {
        pairingDialog.open()
        tryCompare(pairingDialog, "opened", true)
        pairingDialog.close()
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        tryCompare(pairingDialog, "visible", false)
        verify(errorDialog.standardButton(Dialog.Ok).activeFocus)
        keyClick(Qt.Key_Return)
        tryCompare(errorDialog, "visible", false)
        verify(stackView.currentItem.activeFocus)
    }

    function test_error_stays_focused_when_view_changes() {
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        stackView.push(view, StackView.Immediate)
        verify(errorDialog.standardButton(Dialog.Ok).activeFocus,
               "Changing the background view should not take focus from the error")
        keyClick(Qt.Key_Return)
        tryCompare(errorDialog, "visible", false)
        verify(stackView.currentItem.activeFocus)
    }

    function test_error_recovers_focus_if_background_view_steals_it_data() {
        return [
            { tag: "grid-navigation", confirm: Qt.Key_Return },
            { tag: "settings-navigation", confirm: Qt.Key_Space }
        ]
    }

    function test_error_recovers_focus_if_background_view_steals_it(data) {
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        var background = stackView.currentItem
        background.forceActiveFocus()
        keyClick(Qt.Key_Down)
        compare(background.backgroundKeyCount, 0,
                "Controller navigation must not reach the view behind the error")
        keyClick(data.confirm)
        tryCompare(errorDialog, "visible", false)
        compare(background.backgroundKeyCount, 0)
    }

    function test_tab_navigation_does_not_reach_the_background() {
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        var background = stackView.currentItem
        background.forceActiveFocus()
        keyClick(Qt.Key_Tab)
        compare(background.backgroundKeyCount, 0)
        verify(errorDialog.activeFocus)
        keyClick(Qt.Key_Escape)
        tryCompare(errorDialog, "visible", false)
    }

    function test_other_modal_recovers_focus_if_background_view_steals_it() {
        pairingDialog.open()
        tryCompare(pairingDialog, "opened", true)
        var background = stackView.currentItem
        background.forceActiveFocus()
        keyClick(Qt.Key_Down)
        compare(background.backgroundKeyCount, 0)
        verify(pairingDialog.activeFocus)
        keyClick(Qt.Key_Escape)
        tryCompare(pairingDialog, "visible", false)
    }

    function test_modal_restores_its_focused_field() {
        formDialog.open()
        tryCompare(formDialog, "opened", true)
        verify(formField.activeFocus)
        stackView.currentItem.forceActiveFocus()
        verify(formField.activeFocus)
        keyClick(Qt.Key_A)
        compare(formField.text, "a")
    }

    function test_modal_dropdown_keeps_its_own_focus() {
        formDialog.open()
        tryCompare(formDialog, "opened", true)
        formCombo.currentIndex = 0
        formCombo.forceActiveFocus()
        keyClick(Qt.Key_Space)
        tryCompare(formCombo.popup, "opened", true)
        keyClick(Qt.Key_Down)
        keyClick(Qt.Key_Return)
        tryCompare(formCombo.popup, "visible", false)
        compare(formCombo.currentIndex, 1)
        verify(formDialog.visible)
    }

    function test_modal_menu_keeps_its_own_focus() {
        formDialog.open()
        tryCompare(formDialog, "opened", true)
        formMenu.open()
        tryCompare(formMenu, "opened", true)
        formMenuItem.forceActiveFocus()
        verify(formMenuItem.activeFocus)
        keyClick(Qt.Key_Return)
        tryCompare(formMenu, "visible", false)
        compare(formDialog.menuActivated, 1)
        verify(formDialog.visible)
    }

    function test_modal_menu_keeps_focus_when_background_view_changes() {
        formDialog.open()
        tryCompare(formDialog, "opened", true)
        formMenu.open()
        tryCompare(formMenu, "opened", true)
        formMenuItem.forceActiveFocus()
        stackView.push(view, StackView.Immediate)
        verify(formMenuItem.activeFocus)
        keyClick(Qt.Key_Return)
        tryCompare(formMenu, "visible", false)
        compare(formDialog.menuActivated, 1)
        formDialog.close()
        tryCompare(formDialog, "visible", false)
        verify(stackView.currentItem.activeFocus)
    }

    function test_top_modal_recovers_focus_from_an_older_dialog() {
        pairingDialog.open()
        tryCompare(pairingDialog, "opened", true)
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        pairingDialog.standardButton(Dialog.Cancel).forceActiveFocus()
        verify(errorDialog.standardButton(Dialog.Ok).activeFocus)
        keyClick(Qt.Key_Return)
        tryCompare(errorDialog, "visible", false)
        verify(pairingDialog.visible)
        tryVerify(function() { return pairingDialog.activeFocus })
        keyClick(Qt.Key_Escape)
        tryCompare(pairingDialog, "visible", false)
    }

    function test_modal_recovers_focus_from_a_background_menu() {
        navigationMenu.open()
        tryCompare(navigationMenu, "opened", true)
        errorDialog.open()
        tryCompare(errorDialog, "opened", true)
        navigationMenuItem.forceActiveFocus()
        verify(errorDialog.standardButton(Dialog.Ok).activeFocus)
        keyClick(Qt.Key_Return)
        tryCompare(errorDialog, "visible", false)
        compare(stackView.depth, 1)
    }

    function test_view_change_focuses_the_new_view_without_a_popup() {
        stackView.push(view, StackView.Immediate)
        verify(stackView.currentItem.activeFocus)
    }

    function test_menu_navigation_focuses_the_new_view() {
        navigationMenu.open()
        tryCompare(navigationMenu, "opened", true)
        navigationMenuItem.forceActiveFocus()
        keyClick(Qt.Key_Return)
        tryCompare(navigationMenu, "visible", false)
        compare(stackView.depth, 2)
        verify(stackView.currentItem.activeFocus)
    }

    function test_destroyed_dialog_is_removed_from_focus_tracking() {
        var transient = transientError.createObject(testCase)
        verify(transient)
        transient.open()
        tryCompare(transient, "opened", true)
        verify(PopupFocus.hasVisibleModal())
        transient.destroy()
        tryVerify(function() { return !PopupFocus.hasVisibleModal() })
        stackView.push(view, StackView.Immediate)
        verify(stackView.currentItem.activeFocus)
    }

    function test_destroyed_dialog_restores_changed_view() {
        var transient = transientError.createObject(testCase)
        verify(transient)
        transient.open()
        tryCompare(transient, "opened", true)
        stackView.push(view, StackView.Immediate)
        transient.destroy()
        tryVerify(function() { return !PopupFocus.hasVisibleModal() })
        tryVerify(function() { return stackView.currentItem.activeFocus })
    }

    function test_error_remains_usable_when_its_view_is_popped() {
        var item = errorView.createObject(stackView)
        stackView.push(item, StackView.Immediate)
        item.error.open()
        tryCompare(item.error, "opened", true)
        stackView.pop(null, StackView.Immediate)
        verify(item.error.visible)
        verify(item.error.standardButton(Dialog.Ok).activeFocus)
        keyClick(Qt.Key_Return)
        tryCompare(item.error, "visible", false)
        item.destroy()
    }
}
