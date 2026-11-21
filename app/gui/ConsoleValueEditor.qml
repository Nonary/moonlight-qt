import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import SdlGamepadKeyNavigation 1.0
import "ConsoleStyle.js" as Style

NavigableDialog {
    id: dialog
    objectName: "settingsValueEditor"
    property var target: null
    property var ownerRow: null
    property real pendingValue: 0
    property bool rangeEditor: false
    readonly property bool controllerInput: SdlGamepadKeyNavigation.inputMethod === "controller"
    readonly property int shoulderJumpKbps: 10000
    readonly property int triggerJumpKbps: 100000
    function mbps(kbps) { return qsTr("%1 Mbps").arg(kbps / 1000) }
    width: Math.min(Style.px(560, hostWindow), hostWindow ? hostWindow.width - Style.px(48, hostWindow) : 560)
    tabNavigation: false
    focus: true
    enter: Transition { }
    exit: Transition { }
    footer: Item { height: 0 }
    function hintKey(action) {
        if (hostWindow && typeof hostWindow.controllerLabel === "function") return hostWindow.controllerLabel(action)
        var controller = SdlGamepadKeyNavigation.inputMethod === "controller"
        if (action === "sections") return controller ? "LB / RB" : "PgUp / PgDn"
        if (action === "triggers") return controller ? "LT / RT" : "Ctrl+PgUp / PgDn"
        return action === "confirm" ? (controller ? "A" : "Enter") : (controller ? "B" : "Esc")
    }
    function openFor(row, control, range) {
        ownerRow = row; target = control; rangeEditor = range
        title = row.settingsLabel; pendingValue = control.value
        refreshNumber()
        open()
    }
    function refreshNumber() {
        numberInput.edited = false
        if (target && !rangeEditor) numberInput.text = target.textFromValue(pendingValue, target.locale)
    }
    function readNumber() {
        if (rangeEditor || !numberInput.edited) return true
        if (!numberInput.acceptableInput) return false
        var parsed = target.valueFromText(numberInput.text, target.locale)
        if (!isFinite(parsed)) return false
        pendingValue = Math.max(target.from, Math.min(target.to, parsed))
        refreshNumber()
        return true
    }
    function finish() { if (readNumber()) accept(); else numberInput.forceActiveFocus() }
    function focusEditor() {
        if (!rangeEditor && !numberInput.readOnly && SdlGamepadKeyNavigation.inputMethod !== "controller") {
            numberInput.forceActiveFocus()
            numberInput.selectAll()
        } else focusSink.forceActiveFocus()
    }
    function step(amount) {
        if (!readNumber()) return
        pendingValue = Math.max(target.from, Math.min(target.to, pendingValue + amount))
        refreshNumber()
    }
    contentItem: FocusScope {
        id: editorFocus
        focus: true
        implicitHeight: editorColumn.implicitHeight
        Item { id: focusSink; focus: true }
        Keys.onPressed: function(event) {
            event.accepted = true
            if (event.key === Qt.Key_Left) dialog.step(-dialog.target.stepSize)
            else if (event.key === Qt.Key_Right) dialog.step(dialog.target.stepSize)
            else if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
                var amount = dialog.rangeEditor ? ((event.modifiers & Qt.ControlModifier) ? dialog.triggerJumpKbps : dialog.shoulderJumpKbps) : 10 * dialog.target.stepSize
                dialog.step(event.key === Qt.Key_PageUp ? -amount : amount)
            }
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) dialog.finish()
            else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) dialog.reject()
            else if (event.key !== Qt.Key_Up && event.key !== Qt.Key_Down) event.accepted = false
        }
        Column {
            id: editorColumn
            width: parent.width; spacing: Style.px(24, dialog.hostWindow)
            ConsoleText {
                width: parent.width; role: "display"; horizontalAlignment: Text.AlignHCenter
                visible: dialog.rangeEditor
                text: dialog.mbps(dialog.pendingValue)
            }
            TextField {
                id: numberInput
                objectName: "settingsNumberInput"
                property bool edited: false
                width: parent.width; visible: !dialog.rangeEditor
                font.family: "Inter"; font.pixelSize: Style.typeSize("display", dialog.hostWindow)
                horizontalAlignment: Text.AlignHCenter; selectByMouse: true
                selectionColor: Style.text; selectedTextColor: Style.surface
                readOnly: !!dialog.target && dialog.target.editable === false
                validator: !dialog.rangeEditor && dialog.target && typeof dialog.target.validator !== "undefined" ? dialog.target.validator : null
                onTextEdited: edited = true
                Keys.onLeftPressed: function(event) { if (readOnly) dialog.step(-dialog.target.stepSize); else event.accepted = false }
                Keys.onRightPressed: function(event) { if (readOnly) dialog.step(dialog.target.stepSize); else event.accepted = false }
                Keys.onReturnPressed: dialog.finish()
                Keys.onEnterPressed: dialog.finish()
            }
            Column {
                width: parent.width; visible: dialog.rangeEditor
                ConsoleSlider {
                    id: rangeSlider
                    width: parent.width
                    from: dialog.target ? dialog.target.from : 0; to: dialog.target ? dialog.target.to : 1
                    stepSize: dialog.target ? dialog.target.stepSize : 1
                    snapMode: Slider.SnapAlways
                    value: dialog.pendingValue
                    onMoved: dialog.pendingValue = value
                    focusPolicy: Qt.NoFocus
                    Accessible.name: dialog.title
                }
                RowLayout {
                    width: parent.width
                    ConsoleText { role: "caption"; text: dialog.mbps(rangeSlider.from); Layout.fillWidth: true }
                    ConsoleText { role: "caption"; text: dialog.mbps(rangeSlider.to) }
                }
            }
            Flow {
                width: parent.width; spacing: Style.px(16, dialog.hostWindow)
                ConsoleText { role: "secondary"; text: dialog.rangeEditor && dialog.target ? qsTr("◀ ▶  %1").arg(dialog.mbps(dialog.target.stepSize)) : qsTr("◀ ▶  Adjust") }
                ConsoleText { role: "secondary"; visible: dialog.rangeEditor; text: qsTr("%1  %2").arg(dialog.hintKey("sections")).arg(dialog.mbps(dialog.shoulderJumpKbps)) }
                ConsoleText { role: "secondary"; visible: dialog.rangeEditor; text: qsTr("%1  %2").arg(dialog.hintKey("triggers")).arg(dialog.mbps(dialog.triggerJumpKbps)) }
            }
            Row {
                anchors.right: parent.right
                spacing: Style.px(12, dialog.hostWindow)
                Repeater {
                    model: ["back", "confirm"]
                    delegate: ConsoleButton {
                        id: actionButton
                        text: modelData === "confirm" ? qsTr("Done") : qsTr("Cancel")
                        primary: modelData === "confirm"
                        contentItem: RowLayout {
                            spacing: Style.px(8, dialog.hostWindow)
                            ConsoleGlyph {
                                key: dialog.hintKey(modelData); gamepad: dialog.controllerInput
                                visible: dialog.controllerInput || SdlGamepadKeyNavigation.inputMethod === "keyboard"
                            }
                            ConsoleText { text: actionButton.text; role: "body" }
                        }
                        onClicked: modelData === "confirm" ? dialog.finish() : dialog.reject()
                    }
                }
            }
        }
    }
    onOpened: focusEditor()
    onVisibleChanged: if (visible) focusEditor()
    onAccepted: {
        target.settingsCommitted(pendingValue)
    }
    onClosed: Qt.callLater(function() { if (ownerRow && ownerRow.visible && ownerRow.enabled) ownerRow.forceActiveFocus() })
}
