import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import SdlGamepadKeyNavigation 1.0
import "ConsoleStyle.js" as Style

NavigableDialog {
    id: keyboard
    objectName: "consoleKeyboard"
    tabNavigation: false
    signal released()
    Component.onDestruction: released()
    property var targetField: null
    property bool numeric: false
    property var commitHandler: null
    property bool committing: false
    property bool allowText: true
    readonly property bool addressMode: targetField && !!targetField.addressInput
    readonly property string submitLabel: addressMode ? qsTr("Connect") : qsTr("Done")
    readonly property bool canSubmit: draft.acceptableInput && (!addressMode || draft.text.trim().length > 0)
    property bool upperCase: false
    property bool symbols: false
    property bool previousNavMode: false
    property alias draftText: draft.text
    property alias keyGrid: keyGrid
    title: targetField && targetField.addressInput ? qsTr("Enter PC address") : numeric ? qsTr("Enter a value") : qsTr("Keyboard")
    width: Math.min(Style.px(880, hostWindow), (Overlay.overlay ? Overlay.overlay.width : 900) - Style.px(32, hostWindow))
    height: Math.min(implicitHeight, (Overlay.overlay ? Overlay.overlay.height : 720) - Style.px(16, hostWindow))
    footer: Item { implicitHeight: 0 }
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape

    function openFor(field, digitsOnly) {
        targetField = field
        numeric = digitsOnly
        allowText = (field.inputMethodHints & Qt.ImhDigitsOnly) === 0
        upperCase = false
        symbols = false
        draft.text = field.text
        draft.cursorPosition = field.cursorPosition === undefined ? draft.length : field.cursorPosition
        previousNavMode = SdlGamepadKeyNavigation.currentUiNavMode()
        open()
    }
    function enterKey(key) {
        draft.insert(draft.cursorPosition, key)
    }
    function commit() {
        if (!canSubmit || !opened || committing) return
        committing = true
        accept()
    }
    onOpened: {
        SdlGamepadKeyNavigation.setUiNavMode(false)
        keyGrid.currentIndex = 0
        keyGrid.forceActiveFocus()
    }
    onClosed: {
        var field = targetField
        var handler = committing && field ? commitHandler : null
        var value = draft.text
        if (committing && field) {
            field.text = value
            if (typeof field.editingFinished === "function") field.editingFinished()
        }
        targetField = null
        commitHandler = null
        committing = false
        SdlGamepadKeyNavigation.setUiNavMode(previousNavMode)
        if (field) field.forceActiveFocus()
        // The parent form may close itself. Hand it the result only after
        // this popup has finished restoring focus and navigation mode.
        if (handler) Qt.callLater(function() { handler(value) })
    }
    contentItem: ColumnLayout {
        spacing: Style.px(12, keyboard.hostWindow)
        Keys.onMenuPressed: keyboard.erase()
        Keys.onHangupPressed: keyboard.commit()
        Keys.onPressed: function(event) {
            if (!(event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown)) {
                draft.cursorPosition = Math.max(0, Math.min(draft.length, draft.cursorPosition + (event.key === Qt.Key_PageUp ? -1 : 1)))
                event.accepted = true
            }
        }
        TextField {
            id: draft
            objectName: "keyboardDraft"
            Layout.fillWidth: true
            font.family: "Inter"
            font.pixelSize: Style.typeSize("heading", keyboard.hostWindow)
            selectByMouse: true
            cursorVisible: keyboard.opened
            selectionColor: Style.text
            selectedTextColor: Style.surface
            maximumLength: keyboard.targetField ? keyboard.targetField.maximumLength : 32767
            validator: keyboard.targetField ? keyboard.targetField.validator : null
            Keys.onDownPressed: keyGrid.forceActiveFocus()
            Keys.onReturnPressed: keyboard.commit()
            Keys.onEnterPressed: keyboard.commit()
        }
        GridView {
            id: keyGrid
            objectName: "keyboardKeys"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            Layout.preferredHeight: Style.px(56, keyboard.hostWindow) * rowLengths.length
            clip: true
            property int columns: keyboard.numeric ? 6 : 10
            cellWidth: width / columns
            cellHeight: Math.max(Style.px(32, keyboard.hostWindow), Math.floor(height / rowLengths.length))
            interactive: false
            property var rowLengths: keyboard.numeric ? (keyboard.allowText || keyboard.addressMode ? [6,6] : [6,4]) : keyboard.symbols ? [10,10,10,10,2] : [10,9,7]
            model: keyboard.numeric ? (keyboard.addressMode ? "1234567890.:" : keyboard.allowText ? "1234567890.," : "1234567890").split("") : keyboard.symbols ?
                "1234567890!@#$%^&*()_-+=[]{}:/\\.,?;\"'<>|~`".split("") :
                (keyboard.upperCase ? "QWERTYUIOPASDFGHJKL ZXCVBNM   " : "qwertyuiopasdfghjkl zxcvbnm   ").split("")
            function move(dx, dy) {
                var row = Math.floor(currentIndex / columns), col = currentIndex % columns
                if (dy && row + dy >= rowLengths.length) { backspace.forceActiveFocus(); return }
                if (dy && row + dy < 0) { draft.forceActiveFocus(); return }
                row += dy
                col = Math.max(0, Math.min(rowLengths[row] - 1, col + dx))
                currentIndex = Math.min(count - 1, row * columns + col)
            }
            delegate: ConsoleButton {
                width: keyGrid.cellWidth - Style.px(8, keyboard.hostWindow)
                height: keyGrid.cellHeight - Style.px(8, keyboard.hostWindow)
                text: modelData
                activeFocusOnTab: false
                onActiveFocusChanged: if (activeFocus) keyGrid.currentIndex = index
                focus: GridView.isCurrentItem
                visible: modelData !== " "
                enabled: visible
                font.pixelSize: Style.typeSize("heading", keyboard.hostWindow)
                Keys.onLeftPressed: keyGrid.move(-1,0)
                Keys.onRightPressed: keyGrid.move(1,0)
                Keys.onUpPressed: keyGrid.move(0,-1)
                Keys.onDownPressed: keyGrid.move(0,1)
                onClicked: { keyGrid.currentIndex = index; keyboard.enterKey(modelData); keyGrid.forceActiveFocus() }
            }
            Keys.onReturnPressed: keyboard.enterKey(model[currentIndex])
            Keys.onEnterPressed: keyboard.enterKey(model[currentIndex])
            Keys.onSpacePressed: keyboard.enterKey(model[currentIndex])
            Keys.onLeftPressed: move(-1,0)
            Keys.onRightPressed: move(1,0)
            Keys.onDownPressed: move(0,1)
            Keys.onUpPressed: move(0,-1)
        }
        RowLayout {
            spacing: Style.px(12, keyboard.hostWindow)
            Layout.fillWidth: true
            ConsoleButton { text: keyboard.numeric || keyboard.symbols ? qsTr("ABC") : qsTr("123 / #"); visible: keyboard.allowText; onClicked: { if (keyboard.numeric) keyboard.numeric = false; else keyboard.symbols = !keyboard.symbols; keyGrid.currentIndex = 0 } }
            ConsoleButton { text: qsTr("Shift"); selected: keyboard.upperCase; visible: !keyboard.numeric; onClicked: keyboard.upperCase = !keyboard.upperCase }
            ConsoleButton { text: qsTr("Space"); visible: !keyboard.numeric; Layout.fillWidth: true; onClicked: keyboard.enterKey(" ") }
            ConsoleButton {
                id: backspace
                text: qsTr("Backspace")
                onClicked: keyboard.erase()
            }
            Keys.onUpPressed: keyGrid.forceActiveFocus()
            Keys.onDownPressed: (doneButton.enabled ? doneButton : cancelButton).forceActiveFocus()
            Keys.onLeftPressed: keyboard.focusPrevious()
            Keys.onRightPressed: keyboard.focusNext()
        }
        RowLayout {
            spacing: Style.px(12, keyboard.hostWindow)
            ConsoleText { role: "caption"; text: keyboard.commandLabel("options") + " " + qsTr("Backspace") + " · " + keyboard.commandLabel("done") + " " + keyboard.submitLabel + " · " + keyboard.commandLabel("sections") + " " + qsTr("Cursor"); visible: SdlGamepadKeyNavigation.inputMethod === "controller" }
            Item { Layout.fillWidth: true }
            ConsoleButton { id: cancelButton; text: qsTr("Cancel"); onClicked: keyboard.reject() }
            ConsoleButton { id: doneButton; objectName: "keyboardSubmit"; text: keyboard.submitLabel; enabled: keyboard.canSubmit; onClicked: keyboard.commit() }
            Layout.fillWidth: true
            Keys.onUpPressed: backspace.forceActiveFocus()
            Keys.onLeftPressed: keyboard.focusPrevious()
            Keys.onRightPressed: keyboard.focusNext()
        }
    }
    function commandLabel(action) {
        return hostWindow && typeof hostWindow.controllerLabel === "function" ? hostWindow.controllerLabel(action) : action === "options" ? "X" : action === "done" ? "Y" : "LB / RB"
    }
    function erase() { if (draft.cursorPosition > 0) draft.remove(draft.cursorPosition - 1, draft.cursorPosition) }
    function focusNext() { var item = keyGrid.Window.activeFocusItem; if (item) { var next = item.nextItemInFocusChain(true); if (next.parent === item.parent) next.forceActiveFocus() } }
    function focusPrevious() { var item = keyGrid.Window.activeFocusItem; if (item) { var next = item.nextItemInFocusChain(false); if (next.parent === item.parent) next.forceActiveFocus() } }
}
