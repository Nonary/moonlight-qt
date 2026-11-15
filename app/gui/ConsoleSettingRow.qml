import QtQuick 2.9
import QtQuick.Window 2.2
import QtQuick.Controls 2.5
import "ConsoleStyle.js" as Style

Item {
    id: row
    property Item labelItem
    property Item editor
    property string description: ""
    property var settingsNavigation: null
    property Item disabledSettingsProxy: null
    readonly property bool settingsEntry: true
    readonly property string settingsLabel: labelItem ? labelItem.text : ""
    readonly property string settingsHelp: description || (editor ? editor.ToolTip.text : "")
    readonly property bool rowFocused: activeFocus || !!(editor && editor.activeFocus)
    activeFocusOnTab: !editor || editor.enabled
    width: parent ? parent.width : 600
    implicitHeight: Math.max(Style.px(64, Window.window), labelItem ? labelItem.implicitHeight + Style.px(24, Window.window) : 0)
    height: implicitHeight
    onLabelItemChanged: if (labelItem) { labelItem.parent = labelHolder; labelItem.width = Qt.binding(function() { return labelHolder.width }); labelItem.wrapMode = Text.Wrap }
    onEditorChanged: if (editor) {
        editor.parent = editorHolder
        editor.width = Qt.binding(function() { return editorHolder.width })
        editor.activeFocusOnTab = false
        if (editor.contentItem) editor.contentItem.activeFocusOnTab = false
        if (typeof editor.settingsRowHost !== "undefined") editor.settingsRowHost = row
    }
    onActiveFocusChanged: if (activeFocus && settingsNavigation) settingsNavigation.rowFocused(row)
    function activateSetting() { if (editor && typeof editor.openSettingsEditor === "function") editor.openSettingsEditor(row) }
    Keys.onPressed: function(event) { event.accepted = settingsNavigation ? settingsNavigation.handleRowKey(row, event) : false }
    Rectangle { anchors.fill: parent; color: row.rowFocused ? Style.control : "transparent"; radius: Style.px(6, row.Window.window) }
    Item { id: labelHolder; x: Style.px(16, row.Window.window); y: (row.height - (row.labelItem ? row.labelItem.implicitHeight : 0)) / 2; width: editorHolder.x - x - Style.px(24, row.Window.window); height: row.labelItem ? row.labelItem.implicitHeight : 0; opacity: !row.editor || row.editor.enabled ? 1 : 0.5 }
    Item { id: editorHolder; width: Math.min(Style.px(280, row.Window.window), row.width * 0.43); x: row.width - width - Style.px(16, row.Window.window); y: (row.height - (row.editor ? row.editor.implicitHeight : 0)) / 2; height: row.editor ? row.editor.implicitHeight : 0 }
    MouseArea { width: editorHolder.x; height: parent.height; enabled: !row.editor || row.editor.enabled; onClicked: { row.forceActiveFocus(); row.activateSetting() } }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Style.outline }
    ConsoleFocus { focused: row.rowFocused; radius: Style.px(6, row.Window.window) }
}
