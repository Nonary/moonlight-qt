import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2
import QtTest 1.2
import "../../app/gui"

Rectangle {
    id: preview
    width: 520
    height: settings.implicitHeight + 32
    color: "#303030"
    Material.theme: Material.Dark
    property int savedPace: 1500
    property bool imageSaved: false
    PyroWavePacketSpeedSettings {
        id: settings
        x: 16; y: 16
        width: parent.width - 32
        paceMbps: preview.savedPace
        onPaceEdited: function(value) { preview.savedPace = value }
    }
    TestCase {
        name: "PyroWavePacketSpeed"
        when: windowShown
        function init() { preview.width = 520; preview.savedPace = 1500 }
        function test_edit_and_automatic() {
            var speed = findChild(settings, "pyrowavePacketSpeed")
            var automatic = findChild(settings, "pyrowavePacketSpeedAutomatic")
            compare(speed.value, 1500)
            speed.contentItem.forceActiveFocus()
            speed.contentItem.selectAll()
            keyClick(Qt.Key_1); keyClick(Qt.Key_3); keyClick(Qt.Key_0); keyClick(Qt.Key_0)
            keyClick(Qt.Key_Return)
            compare(preview.savedPace, 1300)
            speed.forceActiveFocus()
            keyClick(Qt.Key_Right)
            compare(preview.savedPace, 1350)
            mouseClick(automatic)
            compare(preview.savedPace, 0)
            compare(speed.value, 0)
            verify(!automatic.enabled)
            // Applying calibration updates the same control's binding.
            preview.savedPace = 1250
            compare(speed.value, 1250)
            verify(automatic.enabled)
        }
        function test_compact_layout() {
            preview.width = 320
            var speed = findChild(settings, "pyrowavePacketSpeed")
            var automatic = findChild(settings, "pyrowavePacketSpeedAutomatic")
            waitForRendering(preview)
            verify(speed.x + speed.width <= automatic.x)
            verify(automatic.x + automatic.width <= settings.width)
            verify(preview.height < 500)
            preview.imageSaved = false
            verify(preview.grabToImage(function(result) {
                preview.imageSaved = result.saveToFile("packet-speed-compact.png")
            }))
            tryCompare(preview, "imageSaved", true)
        }
    }
}
