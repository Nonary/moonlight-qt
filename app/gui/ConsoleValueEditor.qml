import QtQuick 2.9
import QtQuick.Controls 2.5
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
    width: Math.min(Style.px(560, hostWindow), hostWindow ? hostWindow.width - Style.px(48, hostWindow) : 560)
    tabNavigation: false
    focus: true
    enter: Transition { }
    exit: Transition { }
    footer: Item { height: 0 }
    function hintKey(action) {
        if (typeof window !== "undefined" && typeof window.controllerLabel === "function") return window.controllerLabel(action)
        return SdlGamepadKeyNavigation.inputMethod === "controller" ?
            (action === "confirm" ? "A" : "B") : (action === "confirm" ? "Enter" : "Esc")
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
    function step(direction, multiplier) {
        if (!readNumber()) return
        pendingValue = Math.max(target.from, Math.min(target.to, pendingValue + direction * target.stepSize * (multiplier || 1)))
        refreshNumber()
    }
    contentItem: FocusScope {
        id: editorFocus
        focus: true
        implicitHeight: editorColumn.implicitHeight
        Item { id: focusSink; focus: true }
        Keys.onPressed: function(event) {
            event.accepted = true
            if (event.key === Qt.Key_Left) dialog.step(-1)
            else if (event.key === Qt.Key_Right) dialog.step(1)
            else if (event.key === Qt.Key_PageUp) dialog.step(-1, 10)
            else if (event.key === Qt.Key_PageDown) dialog.step(1, 10)
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) dialog.finish()
            else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) dialog.reject()
            else if (event.key !== Qt.Key_Up && event.key !== Qt.Key_Down && event.key !== Qt.Key_PageUp && event.key !== Qt.Key_PageDown) event.accepted = false
        }
        Column {
            id: editorColumn
            width: parent.width; spacing: Style.px(24, dialog.hostWindow)
            ConsoleText {
                width: parent.width; role: "display"; horizontalAlignment: Text.AlignHCenter
                visible: dialog.rangeEditor
                text: !dialog.target ? "" : dialog.rangeEditor || typeof dialog.target.textFromValue !== "function" ? qsTr("%1 Mbps").arg(dialog.pendingValue / 1000) : dialog.target.textFromValue(dialog.pendingValue, dialog.target.locale)
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
                Keys.onLeftPressed: function(event) { if (readOnly) dialog.step(-1); else event.accepted = false }
                Keys.onRightPressed: function(event) { if (readOnly) dialog.step(1); else event.accepted = false }
                Keys.onReturnPressed: dialog.finish()
                Keys.onEnterPressed: dialog.finish()
            }
            ConsoleSlider {
                width: parent.width; visible: dialog.rangeEditor
                from: dialog.target ? dialog.target.from : 0; to: dialog.target ? dialog.target.to : 1
                stepSize: dialog.target ? dialog.target.stepSize : 1
                snapMode: Slider.SnapAlways
                value: dialog.pendingValue
                onMoved: dialog.pendingValue = value
                activeFocusOnTab: false
            }
            Flow {
                width: parent.width; spacing: Style.px(16, dialog.hostWindow)
                ConsoleText { role: "secondary"; text: qsTr("◀  ▶  Adjust") }
                Repeater {
                    model: ["confirm", "back"]
                    delegate: Row {
                        spacing: Style.px(8, dialog.hostWindow)
                        ConsoleGlyph { key: dialog.hintKey(modelData); gamepad: SdlGamepadKeyNavigation.inputMethod === "controller"; anchors.verticalCenter: parent.verticalCenter }
                        ConsoleText { role: "secondary"; text: modelData === "confirm" ? qsTr("Done") : qsTr("Cancel") }
                    }
                }
            }
            Row {
                spacing: Style.px(12, dialog.hostWindow)
                ConsoleButton { text: qsTr("Done"); onClicked: dialog.finish() }
                ConsoleButton { text: qsTr("Cancel"); onClicked: dialog.reject() }
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
