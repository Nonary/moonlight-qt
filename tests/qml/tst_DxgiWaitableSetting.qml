import QtQuick 2.9
import QtTest 1.2
import "../../app/gui"

Item {
    width: 640; height: 120
    DxgiWaitableSetting { id: setting }
    SignalSpy { id: toggles; target: setting; signalName: "toggled" }
    TestCase {
        name: "DxgiWaitableSetting"
        when: windowShown
        function init() {
            setting.windowsPlatform = true
            setting.vrrEnabled = true
            setting.vsyncEnabled = true
            setting.checked = false
            toggles.clear()
        }
        function test_mouse_and_keyboard() {
            verify(!setting.checked)
            mouseClick(setting)
            verify(setting.checked)
            compare(toggles.count, 1)
            setting.forceActiveFocus()
            keyClick(Qt.Key_Space)
            verify(!setting.checked)
            compare(toggles.count, 2)
        }
        function test_eligibility_preserves_selection() {
            setting.checked = true
            setting.vsyncEnabled = false
            verify(!setting.enabled)
            verify(setting.checked)
            setting.vrrEnabled = false
            verify(!setting.visible)
            setting.vrrEnabled = true
            setting.windowsPlatform = false
            verify(!setting.visible)
            verify(!setting.enabled)
            verify(setting.checked)
        }
    }
}
