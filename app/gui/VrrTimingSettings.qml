import QtQuick 2.9
import "ConsoleStyle.js" as Style
import QtQuick.Controls 2.2
import QtQuick.Window 2.2
import QtQuick.Layouts 1.2

ConsoleSettingsSection {
    id: root
    title: qsTr("VRR timing")
    property int bufferPerMille: 2000
    property int targetHundredths: 9950
    property int historySeconds: 120
    property int toleranceUs: 500
    signal presetPicked(int mode)
    signal bufferEdited(int value)
    signal targetEdited(int value)
    signal historyEdited(int value)
    signal toleranceEdited(int value)
    readonly property int presetIndex: bufferPerMille === 1000 && targetHundredths === 9900 && historySeconds === 60 && toleranceUs === 500 ? 0 :
                                       bufferPerMille === 2000 && targetHundredths === 9950 && historySeconds === 120 && toleranceUs === 500 ? 1 :
                                       bufferPerMille === 4000 && targetHundredths === 9999 && historySeconds === 300 && toleranceUs === 200 ? 2 : 3

    ColumnLayout {
        width: parent.width
        spacing: 0

        ConsoleSettingRow {
            description: qsTr("Presets fill in these four values. Confirm to open an editor. Larger buffers and higher targets favor steadier timing over lower delay. Reconnect the stream after changing these settings.")
            Layout.fillWidth: true
            labelItem: ConsoleText { text: qsTr("Preset") }
            editor: AutoResizingComboBox {
            id: preset
            objectName: "vrrPreset"
            width: parent.width
            model: [qsTr("Low Latency"), qsTr("Balanced"), qsTr("Smooth"), qsTr("Custom")]
            currentIndex: root.presetIndex
            onActivated: {
                if (currentIndex < 3) root.presetPicked([2, 1, 0][currentIndex])
                // Custom describes the current values; selecting it changes nothing.
                currentIndex = Qt.binding(function() { return root.presetIndex })
            }
        }
        }

        ConsoleSettingRow {
            description: qsTr("The maximum buffering allowance, measured in source frames. Actual delay can be below this allowance. Reconnect the stream after changing this setting.")
            Layout.fillWidth: true
            labelItem: ConsoleText { text: qsTr("Buffer allowance (source frames)");  wrapMode: Text.Wrap }
            editor: ConsoleSpinBox {
            id: buffer
            objectName: "vrrBuffer"
            width: parent.width
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
            onSettingsCommitted: function(newValue) { root.bufferEdited(newValue) }
            Keys.onLeftPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) root.bufferEdited(Math.max(from, value - stepSize)) }
            Keys.onRightPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) root.bufferEdited(Math.min(to, value + stepSize)) }
            Accessible.name: qsTr("Buffer allowance in source frames")
        }
        }

        ConsoleSettingRow {
            description: qsTr("The timing target is a goal, not a guarantee. Higher targets favor steadier timing over lower delay. Reconnect the stream after changing this setting.")
            Layout.fillWidth: true
            labelItem: ConsoleText { text: qsTr("Timing target (%)") }
            editor: ConsoleSpinBox {
            id: target
            objectName: "vrrTarget"
            width: parent.width
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
            onSettingsCommitted: function(newValue) { root.targetEdited(newValue) }
            Keys.onLeftPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) root.targetEdited(Math.max(from, value - stepSize)) }
            Keys.onRightPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) root.targetEdited(Math.min(to, value + stepSize)) }
            Accessible.name: qsTr("Timing target percent")
        }
        }

        ConsoleSettingRow {
            description: qsTr("How much recent timing is considered when adjusting buffering. Reconnect the stream after changing this setting.")
            Layout.fillWidth: true
            labelItem: ConsoleText { text: qsTr("History window (seconds)") }
            editor: ConsoleSpinBox {
            id: history
            objectName: "vrrHistory"
            width: parent.width
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
            onSettingsCommitted: function(newValue) { root.historyEdited(newValue) }
            Keys.onLeftPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) root.historyEdited(Math.max(from, value - stepSize)) }
            Keys.onRightPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) root.historyEdited(Math.min(to, value + stepSize)) }
            Accessible.name: qsTr("History window in seconds")
        }
        }

        ConsoleSettingRow {
            description: qsTr("Interval variation allowed before timing counts as a miss. A larger tolerance can reduce buffering, but accepts more uneven timing. Reconnect the stream after changing this setting.")
            Layout.fillWidth: true
            labelItem: ConsoleText { text: qsTr("Frame interval tolerance (ms)") }
            editor: ConsoleSpinBox {
            id: tolerance
            objectName: "vrrTolerance"
            width: parent.width
            from: 200; to: 2000; stepSize: 50
            value: root.toleranceUs
            editable: false
            Component.onCompleted: {
                // The style's text item can receive controller arrows even
                // when editing is disabled. Reserve the entire key event.
                contentItem.Keys.priority = Keys.BeforeItem
                contentItem.Keys.leftPressed.connect(function(event) { tolerance.Keys.leftPressed(event) })
                contentItem.Keys.rightPressed.connect(function(event) { tolerance.Keys.rightPressed(event) })
                contentItem.Keys.released.connect(function(event) { tolerance.Keys.released(event) })
            }
            textFromValue: function(value, locale) { return Number(value / 1000).toLocaleString(locale, 'f', 2) }
            onValueModified: root.toleranceEdited(value)
            onSettingsCommitted: function(newValue) { root.toleranceEdited(newValue) }
            Keys.onLeftPressed: function(event) {
                if (settingsRowHost && settingsRowHost.settingsNavigation && settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) return
                event.accepted = true
                root.toleranceEdited(Math.max(from, value - stepSize))
            }
            Keys.onRightPressed: function(event) {
                if (settingsRowHost && settingsRowHost.settingsNavigation && settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) return
                event.accepted = true
                root.toleranceEdited(Math.min(to, value + stepSize))
            }
            Keys.onReleased: function(event) {
                if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) event.accepted = true
            }
            Accessible.name: qsTr("Interval tolerance in milliseconds")
        }
        }
    }
}
