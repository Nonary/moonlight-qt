import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

import SdlGamepadKeyNavigation 1.0

// https://stackoverflow.com/questions/45029968/how-do-i-set-the-combobox-width-to-fit-the-largest-item
ComboBox {
    id: control
    property var settingsRowHost: null
    property bool previousNavigationMode: false
    function openSettingsEditor(row) { forceActiveFocus(); popup.open() }
    readonly property bool rowHosted: parent && parent.parent && typeof parent.parent.labelItem !== "undefined"
    implicitHeight: Style.px(48, Window.window)
    displayText: settingsRowHost ? currentText.replace(/\s*\(Recommended\)/, "") : currentText
    leftPadding: Style.px(16, Window.window)
    rightPadding: Style.px(40, Window.window)
    topPadding: 0; bottomPadding: 0
    font.family: "Inter"
    font.pixelSize: Style.typeSize("body", Window.window)
    background: Rectangle { color: control.activeFocus ? Style.raised : "transparent"; radius: Style.px(6, control.Window.window); ConsoleFocus { focused: control.activeFocus && !control.rowHosted; radius: parent.radius } }
    contentItem: ConsoleText { text: control.displayText; horizontalAlignment: control.settingsRowHost ? Text.AlignRight : Text.AlignLeft; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; color: control.enabled ? Style.text : Style.disabled }
    indicator: Canvas {
        x: control.width - width - Style.px(16, control.Window.window); y: (control.height-height)/2
        width: Style.px(12, control.Window.window); height: Style.px(8, control.Window.window)
        onPaint: { var c=getContext("2d"); c.reset(); c.strokeStyle=Style.muted; c.lineWidth=2; c.beginPath(); c.moveTo(0,0); c.lineTo(width/2,height); c.lineTo(width,0); c.stroke() }
        onWidthChanged: requestPaint()
    }
    delegate: ItemDelegate {
        width: control.popup.width
        height: Style.px(48, control.Window.window)
        text: (control.currentIndex === index ? "✓  " : "    ") + (control.textRole ? model[control.textRole] : modelData)
        highlighted: control.highlightedIndex === index
        contentItem: ConsoleText { text: parent.text; color: Style.text; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { color: parent.highlighted ? Style.control : Style.raised; radius: Style.px(6, control.Window.window); ConsoleFocus { focused: control.highlightedIndex === index; radius: parent.radius } }
    }
    property int textWidth
    property int desiredWidth : leftPadding + textWidth + indicator.width + rightPadding
    property int maximumWidth : parent.width

    implicitWidth: desiredWidth < maximumWidth ? desiredWidth : maximumWidth

    // Keep the menu opaque, including software-rendered Qt Quick previews.
    // The Material elevation layer can otherwise hide the menu background.
    popup.background: Rectangle {
        color: Style.raised
        radius: Style.px(10, control.Window.window)
        border.color: Style.outline
    }
    popup.enter: Transition { }
    popup.exit: Transition { }

    TextMetrics {
        id: popupMetrics
    }

    TextMetrics {
        id: textMetrics
    }

    function recalculateWidth() {
        textMetrics.font = font
        popupMetrics.font = popup.font
        textWidth = 0
        for (var i = 0; i < count; i++){
            textMetrics.text = textAt(i)
            popupMetrics.text = textAt(i)
            textWidth = Math.max(textMetrics.width, textWidth)
            textWidth = Math.max(popupMetrics.width, textWidth)
        }
    }

    // We call this every time the options change (and init)
    // so we can adjust the combo box width here too
    onActivated: recalculateWidth()

    popup.onAboutToShow: {
        previousNavigationMode = SdlGamepadKeyNavigation.currentUiNavMode()
        // Switch to normal navigation for combo boxes
        SdlGamepadKeyNavigation.setUiNavMode(false)
    }

    popup.onAboutToHide: {
        SdlGamepadKeyNavigation.setUiNavMode(previousNavigationMode)
    }
    popup.onClosed: if (settingsRowHost && settingsRowHost.settingsNavigation) Qt.callLater(function() { settingsRowHost.forceActiveFocus() })

    function confirmChoice() {
        if (!popup.visible) { popup.open(); return }
        var choice = highlightedIndex
        popup.close()
        if (choice >= 0 && choice < count) { currentIndex = choice; activated(choice) }
    }
    Keys.onReturnPressed: confirmChoice()
    Keys.onEnterPressed: confirmChoice()
    Keys.onSpacePressed: confirmChoice()
    // Opening from a row transfers focus during the press. Its matching
    // release belongs to that activation, not a second confirm in the popup.
    Keys.onReleased: function(event) { event.accepted = event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space }
    Keys.onPressed: function(event) {
        event.accepted = false
        if (popup.visible && event.key !== Qt.Key_Up && event.key !== Qt.Key_Down &&
                event.key !== Qt.Key_Escape && event.key !== Qt.Key_Back) {
            event.accepted = true
            return
        }
        if (!popup.visible && settingsRowHost && settingsRowHost.settingsNavigation) {
            event.accepted = settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)
            return
        }
        if (!popup.visible &&
                (event.key === Qt.Key_Left || event.key === Qt.Key_Right)) {
            var next = Math.max(0, Math.min(count - 1, currentIndex + (event.key === Qt.Key_Left ? -1 : 1)))
            if (next !== currentIndex) { currentIndex = next; activated(currentIndex) }
            event.accepted = true
        }
    }
}
