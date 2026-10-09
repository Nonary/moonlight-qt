import QtQuick 2.9
import QtQuick.Controls 2.2
import QtTest 1.2
import SdlGamepadKeyNavigation 1.0
import "../../app/gui"

Item {
    width: 640; height: 900
    VrrTimingSettings {
        id: settings
        width: 420
        onPresetPicked: function(mode) {
            var values = mode === 2 ? [500, 9900, 60, 500] : mode === 0 ? [4000, 9995, 300, 250] : [1000, 9950, 120, 500]
            bufferPerMille = values[0]; targetHundredths = values[1]
            historySeconds = values[2]; toleranceUs = values[3]
        }
        onBufferEdited: function(value) { bufferPerMille = value }
        onTargetEdited: function(value) { targetHundredths = value }
        onHistoryEdited: function(value) { historySeconds = value }
        onToleranceEdited: function(value) { toleranceUs = value }
    }
    // Same component and model role used by the PyroWave host chooser.
    AutoResizingComboBox {
        id: host
        y: 820; width: 420
        textRole: "name"
        KeyNavigation.tab: afterHost
        model: ListModel {
            ListElement { name: "Host A" }
            ListElement { name: "Host B" }
            ListElement { name: "Host C" }
        }
    }
    Button { id: afterHost; x: 440; y: 820; text: "Continue" }
    TestCase {
        name: "VrrSettings"
        when: windowShown
        function init() {
            settings.presetPicked(1)
            host.currentIndex = 0
        }
        function cleanup() {
            host.popup.close()
            wait(200) // Finish style popup transitions before moving focus in the next test.
        }
        function control(name) { return findChild(settings, name) }
        function test_presets_and_custom() {
            var preset = control("vrrPreset")
            preset.forceActiveFocus()
            keyClick(Qt.Key_Left)
            compare(settings.bufferPerMille, 500)
            compare(settings.targetHundredths, 9900)
            compare(settings.historySeconds, 60)
            compare(settings.toleranceUs, 500)
            var target = control("vrrTarget")
            tryCompare(target, "value", 9900)
            target.forceActiveFocus()
            verify(target.activeFocus)
            keyClick(Qt.Key_Left)
            compare(settings.targetHundredths, 9850)
            compare(preset.currentIndex, 3)
            settings.presetPicked(0)
            compare(preset.currentIndex, 2)
            compare(settings.targetHundredths, 9995)
            compare(settings.toleranceUs, 250)
            settings.presetPicked(1)
            compare(preset.currentIndex, 1)
            compare(settings.targetHundredths, 9950)
        }
        function test_adjust_and_bounds() {
            var names = ["vrrBuffer", "vrrTarget", "vrrHistory", "vrrTolerance"]
            var properties = ["bufferPerMille", "targetHundredths", "historySeconds", "toleranceUs"]
            var minima = [250, 9000, 10, 250]
            var maxima = [4000, 9999, 300, 2000]
            for (var i = 0; i < names.length; i++) {
                var spin = control(names[i])
                spin.forceActiveFocus()
                for (var j = 0; j < 40; j++) keyClick(Qt.Key_Left)
                compare(settings[properties[i]], minima[i])
                for (j = 0; j < 40; j++) keyClick(Qt.Key_Right)
                compare(settings[properties[i]], maxima[i])
            }
        }
        function test_keyboard_entry() {
            var target = control("vrrTarget")
            target.contentItem.forceActiveFocus()
            keyClick(Qt.Key_A, Qt.ControlModifier)
            keyClick(Qt.Key_9); keyClick(Qt.Key_7); keyClick(Qt.Key_Period)
            keyClick(Qt.Key_2); keyClick(Qt.Key_5)
            keyClick(Qt.Key_Return)
            compare(settings.targetHundredths, 9725)
            keyClick(Qt.Key_Left)
            compare(settings.targetHundredths, 9675)
        }
        function test_controller_popup() {
            host.forceActiveFocus()
            keyClick(Qt.Key_Space) // Controller A in UI-navigation mode.
            tryCompare(host.popup, "opened", true)
            compare(SdlGamepadKeyNavigation.uiNavMode, false)
            keyClick(Qt.Key_Down)
            keyClick(Qt.Key_Return) // Controller A in popup mode.
            tryCompare(host.popup, "visible", false)
            compare(host.currentIndex, 1)
            compare(SdlGamepadKeyNavigation.uiNavMode, true)
            verify(host.activeFocus)
            keyClick(Qt.Key_Space)
            tryCompare(host.popup, "opened", true)
            keyClick(Qt.Key_Down)
            keyClick(Qt.Key_Escape) // Controller B cancels selection.
            tryCompare(host.popup, "visible", false)
            compare(host.currentIndex, 1)
            compare(SdlGamepadKeyNavigation.uiNavMode, true)
        }
        function test_popup_does_not_apply_sideways_navigation() {
            host.forceActiveFocus()
            keyClick(Qt.Key_Space)
            tryCompare(host.popup, "opened", true)
            keyClick(Qt.Key_Down)
            compare(host.highlightedIndex, 1)
            keyClick(Qt.Key_Right)
            compare(host.currentIndex, 0)
            keyClick(Qt.Key_Left)
            compare(host.currentIndex, 0)
            verify(host.popup.visible)
            keyClick(Qt.Key_Escape)
            tryCompare(host.popup, "visible", false)
            compare(host.currentIndex, 0)
            verify(host.activeFocus)
            keyClick(Qt.Key_Tab)
            verify(afterHost.activeFocus)
        }
        function test_controller_focus_order() {
            var names = ["vrrPreset", "vrrBuffer", "vrrTarget", "vrrHistory", "vrrTolerance"]
            control(names[0]).forceActiveFocus()
            for (var i = 1; i < names.length; i++) {
                keyClick(Qt.Key_Tab) // Controller down in UI-navigation mode.
                verify(control(names[i]).activeFocus, names[i] + " should receive focus")
            }
            keyClick(Qt.Key_Backtab)
            verify(control("vrrHistory").activeFocus)
        }
    }
}
