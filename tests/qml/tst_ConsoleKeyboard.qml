import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import QtTest 1.2
import SdlGamepadKeyNavigation 1.0
import "../../app/gui"

TestCase {
    id: testCase
    name: "ConsoleKeyboard"
    width: 900
    height: 720
    when: windowShown
    Item { id: stackView; property bool focusViewAfterDialog: false; function forceActiveFocus() {} }
    ConsoleTextField { id: field; width: 250; text: "host" }
    ConsoleSpinBox { id: numeric; y: 80; editable: true; from: 10; to: 300; value: 100 }
    function init() {
        SdlGamepadKeyNavigation.inputMethod = "controller"
        SdlGamepadKeyNavigation.setUiNavMode(true)
        field.text = "host"
        field.cursorPosition = field.length
        field.forceActiveFocus()
    }
    function test_confirmOpensKeyboardAndCancelPreservesValue() {
        keyClick(Qt.Key_Space)
        var keyboard = findChild(field.Window.window, "consoleKeyboard")
        verify(keyboard)
        tryCompare(keyboard, "opened", true)
        compare(SdlGamepadKeyNavigation.uiNavMode, false)
        keyClick(Qt.Key_Return)
        compare(keyboard.draftText, "hostq")
        compare(field.text, "host")
        keyboard.reject()
        tryCompare(keyboard, "visible", false)
        compare(field.text, "host")
        compare(SdlGamepadKeyNavigation.uiNavMode, true)
        verify(field.activeFocus)
    }
    function test_commitAndCursorInsertion() {
        field.openKeyboard()
        var keyboard = findChild(field.Window.window, "consoleKeyboard")
        tryCompare(keyboard, "opened", true)
        var draft = findChild(keyboard, "keyboardDraft")
        draft.cursorPosition = 2
        keyboard.enterKey(".")
        keyboard.commit()
        tryCompare(keyboard, "visible", false)
        compare(field.text, "ho.st")
    }
    function test_numericValidationAndCommit() {
        numeric.value = 100
        numeric.contentItem.forceActiveFocus()
        keyClick(Qt.Key_Space)
        var keyboard = findChild(numeric.Window.window, "consoleKeyboard")
        tryCompare(keyboard, "opened", true)
        compare(keyboard.numeric, true)
        keyboard.draftText = "999"
        keyboard.commit()
        verify(keyboard.visible)
        compare(numeric.value, 100)
        keyboard.draftText = "123"
        keyboard.commit()
        tryCompare(keyboard, "visible", false)
        compare(numeric.value, 123)
    }
    function test_symbolRowAndCursorRemainReachable() {
        field.openKeyboard()
        var keyboard = findChild(field.Window.window, "consoleKeyboard")
        tryCompare(keyboard, "opened", true)
        var draft = findChild(keyboard, "keyboardDraft")
        verify(draft.cursorVisible)
        keyboard.symbols = true
        compare(keyboard.keyGrid.count, 42)
        keyboard.keyGrid.currentIndex = 39
        keyboard.keyGrid.move(0, 1)
        compare(keyboard.keyGrid.currentIndex, 41)
        keyboard.reject()
        tryCompare(keyboard, "visible", false)
    }
    function test_physicalKeyboardStillTypes() {
        SdlGamepadKeyNavigation.inputMethod = "keyboard"
        keyClick(Qt.Key_A)
        compare(field.text, "hosta")
        compare(findChild(field.Window.window, "consoleKeyboard").visible, false)
    }
}
