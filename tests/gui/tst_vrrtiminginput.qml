import QtQuick 2.9
import QtTest 1.2
import "../../app/gui"

TestCase {
    name: "VrrTimingInput"
    when: windowShown
    width: 600
    height: 800

    VrrTimingSettings {
        id: settings
        width: 550
        onBufferEdited: function(value) { bufferPerMille = value }
        onTargetEdited: function(value) { targetHundredths = value }
        onHistoryEdited: function(value) { historySeconds = value }
        onToleranceEdited: function(value) { toleranceUs = value }
    }

    function test_singlePress_data() {
        return [
            { tag: "buffer", control: "vrrBuffer", property: "bufferPerMille", initial: 1000, step: 250 },
            { tag: "target", control: "vrrTarget", property: "targetHundredths", initial: 9950, step: 50 },
            { tag: "history", control: "vrrHistory", property: "historySeconds", initial: 120, step: 10 },
            { tag: "tolerance", control: "vrrTolerance", property: "toleranceUs", initial: 1000, step: 250 }
        ]
    }

    function test_singlePress(data) {
        settings[data.property] = data.initial
        var control = findChild(settings, data.control)
        verify(control)
        control.forceActiveFocus(Qt.TabFocusReason)
        if (control.editable) {
            control.contentItem.cursorPosition = control.contentItem.text.length
        }
        keyClick(Qt.Key_Left)
        compare(settings[data.property], data.initial - data.step)
        keyClick(Qt.Key_Left)
        compare(settings[data.property], data.initial - 2 * data.step)
        keyClick(Qt.Key_Right)
        compare(settings[data.property], data.initial - data.step)
        if (control.editable) {
            control.contentItem.cursorPosition = 0
        }
        keyClick(Qt.Key_Right)
        compare(settings[data.property], data.initial)
        settings[data.property] = control.from
        keyClick(Qt.Key_Left)
        compare(settings[data.property], control.from)
        settings[data.property] = control.to
        keyClick(Qt.Key_Right)
        compare(settings[data.property], control.to)
    }
}
