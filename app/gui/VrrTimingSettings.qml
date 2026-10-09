import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

GroupBox {
    id: root
    title: qsTr("VRR timing")
    property int bufferPerMille: 1000
    property int targetHundredths: 9950
    property int historySeconds: 120
    property int toleranceUs: 500
    signal presetPicked(int mode)
    signal bufferEdited(int value)
    signal targetEdited(int value)
    signal historyEdited(int value)
    signal toleranceEdited(int value)
    readonly property int presetIndex: bufferPerMille === 500 && targetHundredths === 9900 && historySeconds === 60 && toleranceUs === 500 ? 0 :
                                       bufferPerMille === 1000 && targetHundredths === 9950 && historySeconds === 120 && toleranceUs === 500 ? 1 :
                                       bufferPerMille === 4000 && targetHundredths === 9995 && historySeconds === 300 && toleranceUs === 250 ? 2 : 3

    ColumnLayout {
        width: parent.width
        spacing: 8

        Label { text: qsTr("Preset") }
        AutoResizingComboBox {
            id: preset
            objectName: "vrrPreset"
            Layout.fillWidth: true
            model: [qsTr("Low Latency"), qsTr("Balanced"), qsTr("Smooth"), qsTr("Custom")]
            currentIndex: root.presetIndex
            onActivated: {
                if (currentIndex < 3) root.presetPicked([2, 1, 0][currentIndex])
                // Custom describes the current values; selecting it changes nothing.
                currentIndex = Qt.binding(function() { return root.presetIndex })
            }
        }

        Label { text: qsTr("Buffer allowance (source frames)"); Layout.fillWidth: true; wrapMode: Text.Wrap }
        SpinBox {
            id: buffer
            objectName: "vrrBuffer"
            Layout.fillWidth: true
            from: 250; to: 4000; stepSize: 250
            value: root.bufferPerMille
            editable: true
            Component.onCompleted: {
                // Handle adjustment keys before the editor moves its text cursor.
                contentItem.Keys.priority = Keys.BeforeItem
                contentItem.Keys.leftPressed.connect(function(event) { buffer.Keys.leftPressed(event) })
                contentItem.Keys.rightPressed.connect(function(event) { buffer.Keys.rightPressed(event) })
            }
            validator: DoubleValidator { bottom: 0.25; top: 4; decimals: 2; locale: buffer.locale.name }
            textFromValue: function(value, locale) { return Number(value / 1000).toLocaleString(locale, 'f', 2) }
            valueFromText: function(text, locale) { return Math.round(Number.fromLocaleString(locale, text) * 1000) }
            onValueModified: root.bufferEdited(value)
            Keys.onLeftPressed: root.bufferEdited(Math.max(from, value - stepSize))
            Keys.onRightPressed: root.bufferEdited(Math.min(to, value + stepSize))
            Accessible.name: qsTr("Buffer allowance in source frames")
        }

        Label { text: qsTr("Timing target (%)") }
        SpinBox {
            id: target
            objectName: "vrrTarget"
            Layout.fillWidth: true
            from: 9000; to: 9999; stepSize: 50
            value: root.targetHundredths
            editable: true
            Component.onCompleted: {
                // Handle adjustment keys before the editor moves its text cursor.
                contentItem.Keys.priority = Keys.BeforeItem
                contentItem.Keys.leftPressed.connect(function(event) { target.Keys.leftPressed(event) })
                contentItem.Keys.rightPressed.connect(function(event) { target.Keys.rightPressed(event) })
            }
            validator: DoubleValidator { bottom: 90; top: 99.99; decimals: 2; locale: target.locale.name }
            textFromValue: function(value, locale) { return Number(value / 100).toLocaleString(locale, 'f', 2) }
            valueFromText: function(text, locale) { return Math.round(Number.fromLocaleString(locale, text) * 100) }
            onValueModified: root.targetEdited(value)
            Keys.onLeftPressed: root.targetEdited(Math.max(from, value - stepSize))
            Keys.onRightPressed: root.targetEdited(Math.min(to, value + stepSize))
            Accessible.name: qsTr("Timing target percent")
        }

        Label { text: qsTr("History window (seconds)") }
        SpinBox {
            id: history
            objectName: "vrrHistory"
            Layout.fillWidth: true
            from: 10; to: 300; stepSize: 10
            value: root.historySeconds
            editable: true
            Component.onCompleted: {
                // Handle adjustment keys before the editor moves its text cursor.
                contentItem.Keys.priority = Keys.BeforeItem
                contentItem.Keys.leftPressed.connect(function(event) { history.Keys.leftPressed(event) })
                contentItem.Keys.rightPressed.connect(function(event) { history.Keys.rightPressed(event) })
            }
            onValueModified: root.historyEdited(value)
            Keys.onLeftPressed: root.historyEdited(Math.max(from, value - stepSize))
            Keys.onRightPressed: root.historyEdited(Math.min(to, value + stepSize))
            Accessible.name: qsTr("History window in seconds")
        }

        Label { text: qsTr("Frame interval tolerance (ms)") }
        SpinBox {
            id: tolerance
            objectName: "vrrTolerance"
            Layout.fillWidth: true
            from: 250; to: 2000; stepSize: 250
            value: root.toleranceUs
            editable: false
            textFromValue: function(value, locale) { return Number(value / 1000).toLocaleString(locale, 'f', 2) }
            onValueModified: root.toleranceEdited(value)
            Keys.onLeftPressed: root.toleranceEdited(Math.max(from, value - stepSize))
            Keys.onRightPressed: root.toleranceEdited(Math.min(to, value + stepSize))
            Accessible.name: qsTr("Interval tolerance in milliseconds")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Tolerance is the interval variation allowed before timing counts as a miss. A larger tolerance can reduce buffering, but accepts more uneven timing.")
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Presets fill in these four values. Adjust any value to customize it. Use left/right on a controller to adjust, and up/down to move between controls.")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Larger buffers and higher targets favor steadier timing over lower delay. The target is a goal, not a guarantee. History controls how much recent timing is considered. Actual delay may be below the allowance.")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Reconnect the stream after changing these settings.")
        }
    }
}
