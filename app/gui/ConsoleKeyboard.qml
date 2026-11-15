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
    property bool allowText: true
    readonly property bool addressMode: targetField && !!targetField.addressInput
    property bool upperCase: false
    property bool symbols: false
    property bool previousNavMode: false
    property alias draftText: draft.text
    property alias keyGrid: keyGrid
    title: targetField && targetField.addressInput ? qsTr("Enter PC address") : numeric ? qsTr("Enter a value") : qsTr("Keyboard")
    width: Math.min(Style.px(880, hostWindow), (Overlay.overlay ? Overlay.overlay.width : 900) - Style.px(32, hostWindow))
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
        if (draft.acceptableInput) accept()
    }
    onOpened: {
        SdlGamepadKeyNavigation.setUiNavMode(false)
        keyGrid.currentIndex = 0
        keyGrid.forceActiveFocus()
    }
    onAccepted: {
        if (targetField) {
            targetField.text = draft.text
            if (typeof targetField.editingFinished === "function") targetField.editingFinished()
            if (commitHandler) commitHandler(draft.text)
        }
    }
    onClosed: {
        SdlGamepadKeyNavigation.setUiNavMode(previousNavMode)
        if (targetField) targetField.forceActiveFocus()
        targetField = null
        commitHandler = null
    }
    ColumnLayout {
        width: parent.width
        spacing: Style.px(12, keyboard.hostWindow)
        Keys.onMenuPressed: keyboard.erase()
        Keys.onHangupPressed: keyboard.commit()
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
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
            Layout.preferredHeight: cellHeight * Math.ceil(count / columns)
            property int columns: keyboard.numeric ? 6 : 10
            cellWidth: width / columns
            cellHeight: Style.px(56, keyboard.hostWindow)
            interactive: false
            property var rowLengths: keyboard.numeric ? [6,6] : keyboard.symbols ? [10,10,10,10] : [10,9,7]
            model: keyboard.numeric ? (keyboard.addressMode ? "1234567890.:" : "1234567890.,").split("") : keyboard.symbols ?
                "1234567890!@#$%&*()_-+=[]{}:/\\.,?;\"'<>|~`".split("") :
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
            ConsoleButton { text: qsTr("Shift"); visible: !keyboard.numeric; onClicked: keyboard.upperCase = !keyboard.upperCase }
            ConsoleButton { text: qsTr("Space"); visible: !keyboard.numeric; Layout.fillWidth: true; onClicked: keyboard.enterKey(" ") }
            ConsoleButton {
                id: backspace
                text: qsTr("Delete")
                onClicked: keyboard.erase()
            }
            Keys.onUpPressed: keyGrid.forceActiveFocus()
            Keys.onDownPressed: doneButton.forceActiveFocus()
            Keys.onLeftPressed: keyboard.focusPrevious()
            Keys.onRightPressed: keyboard.focusNext()
        }
        RowLayout {
            spacing: Style.px(12, keyboard.hostWindow)
            ConsoleText { role: "caption"; text: keyboard.commandLabel("options") + " " + qsTr("Delete") + " · " + keyboard.commandLabel("done") + " " + qsTr("Done") + " · " + keyboard.commandLabel("sections") + " " + qsTr("Cursor"); visible: SdlGamepadKeyNavigation.inputMethod === "controller" }
            Item { Layout.fillWidth: true }
            ConsoleButton { text: qsTr("Cancel"); onClicked: keyboard.reject() }
            ConsoleButton { id: doneButton; text: qsTr("Done"); enabled: draft.acceptableInput; onClicked: keyboard.commit() }
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
    function focusNext() { var item = keyGrid.Window.activeFocusItem; if (item) item.nextItemInFocusChain(true).forceActiveFocus() }
    function focusPrevious() { var item = keyGrid.Window.activeFocusItem; if (item) item.nextItemInFocusChain(false).forceActiveFocus() }
}
