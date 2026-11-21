import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import StreamingPreferences 1.0
import "ConsoleStyle.js" as Style

GridView {
    id: shelf
    property int selectedAppId: 0
    property bool modelChanging: false
    // Keep the selected cover identifiable when focus moves to navigation.
    property bool showCurrent: activeFocus
    signal selectionChanged()
    signal moveUp()
    signal moveDown()
    signal playRequested()
    signal detailsRequested()
    property bool gridMode: false
    property int columns: 1
    readonly property int pageRows: gridMode ? Math.max(1, Math.floor(height / Style.px(170, Window.window))) : 1
    readonly property int gap: Style.px(16, Window.window)
    readonly property int captionHeight: Style.px(28, Window.window)
    flow: gridMode ? GridView.FlowLeftToRight : GridView.FlowTopToBottom
    clip: true
    activeFocusOnTab: true
    keyNavigationEnabled: false
    leftMargin: gridMode ? 0 : Style.px(64, Window.window)
    rightMargin: leftMargin
    cellHeight: Math.max(1, Math.floor(height / pageRows))
    cellWidth: gridMode ? Math.max(1, Math.floor(width / columns)) :
                          Math.max(1, (cellHeight - captionHeight - Style.px(12, Window.window)) * 0.72 + gap)
    boundsBehavior: Flickable.StopAtBounds
    highlightMoveDuration: StreamingPreferences.uiReducedMotion ? 0 : 120

    function jumpPage(direction) {
        if (!gridMode || count === 0) return
        currentIndex = Math.max(0, Math.min(count - 1, currentIndex + direction * columns * pageRows))
        positionViewAtIndex(currentIndex, GridView.Contain)
    }
    function currentApp() { return model ? model.get(currentIndex) : ({}) }
    function updateSelection() {
        if (modelChanging) return
        selectedAppId = currentApp().appid || 0
        selectionChanged()
    }
    function restoreSelection(wanted) {
        var row = model.indexOfApp(wanted || selectedAppId)
        if (row < 0 && count > 0) row = Math.min(Math.max(currentIndex, 0), count - 1)
        currentIndex = row
        modelChanging = false
        updateSelection()
    }
    function queueRestore() { Qt.callLater(restoreSelection) }
    onCurrentIndexChanged: updateSelection()
    onActiveFocusChanged: { if (activeFocus) selectionChanged() }
    Connections {
        target: shelf.model
        function onRowsAboutToBeInserted() { shelf.modelChanging = true }
        function onRowsAboutToBeRemoved() { shelf.modelChanging = true }
        function onModelAboutToBeReset() { shelf.modelChanging = true }
        function onLayoutAboutToBeChanged() { shelf.modelChanging = true }
        function onRowsInserted() { shelf.queueRestore() }
        function onRowsRemoved() { shelf.queueRestore() }
        function onModelReset() { shelf.queueRestore() }
        function onLayoutChanged() { shelf.queueRestore() }
        function onDataChanged() { shelf.selectionChanged() }
    }
    Keys.onPressed: function(event) {
        if (gridMode && (event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown)) {
            jumpPage(event.key === Qt.Key_PageUp ? -1 : 1)
            event.accepted = true
        }
    }
    Keys.onLeftPressed: if (currentIndex > 0) --currentIndex
    Keys.onRightPressed: if (currentIndex + 1 < count) ++currentIndex
    Keys.onUpPressed: {
        if (gridMode && currentIndex >= columns) currentIndex -= columns
        else moveUp()
    }
    Keys.onDownPressed: {
        if (gridMode && Math.floor(currentIndex / columns) < Math.floor((count - 1) / columns))
            currentIndex = Math.min(count - 1, currentIndex + columns)
        else moveDown()
    }
    Keys.onReturnPressed: playRequested()
    Keys.onEnterPressed: playRequested()
    Keys.onSpacePressed: playRequested()
    Keys.onMenuPressed: detailsRequested()
    delegate: Item {
        width: shelf.cellWidth
        height: shelf.cellHeight
        Accessible.role: Accessible.ListItem
        Accessible.name: model.name
        ConsoleText {
            anchors.top: cover.bottom; anchors.topMargin: Style.px(2, shelf.Window.window)
            x: shelf.gridMode ? shelf.gap / 2 : 0; width: parent.width - shelf.gap
            horizontalAlignment: shelf.gridMode ? Text.AlignHCenter : Text.AlignLeft
            role: "secondary"; text: model.name; elide: Text.ElideRight
            visible: cover.current
        }
        Rectangle {
            id: cover
            readonly property bool current: shelf.currentIndex === index && shelf.showCurrent
            readonly property bool selected: current && shelf.activeFocus
            readonly property bool appRunning: !!model.running
            readonly property bool appHidden: !!model.hidden
            x: shelf.gridMode ? (parent.width - width) / 2 : 0
            y: Style.px(6, shelf.Window.window)
            width: Math.max(1, Math.min(parent.width - shelf.gap, (parent.height - shelf.captionHeight - 2 * y) * 0.72))
            height: width / 0.72
            color: Style.surface
            border.width: 1; border.color: Style.outline
            Image {
                anchors.fill: parent
                source: Style.placeholder(model.boxart) ? "" : model.boxart
                sourceSize.width: 365; sourceSize.height: 507
                fillMode: Image.PreserveAspectCrop; asynchronous: true
            }
            Rectangle {
                anchors.fill: parent; visible: Style.placeholder(model.boxart)
                gradient: Gradient { GradientStop { position: 0; color: Style.control } GradientStop { position: 1; color: Style.surface } }
                ConsoleText { x: Style.px(12, shelf.Window.window); y: Style.px(12, shelf.Window.window); role: "caption"; text: qsTr("App") }
            }
            ConsoleText {
                visible: Style.placeholder(model.boxart)
                anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Style.px(12, shelf.Window.window)
                role: "secondary"; text: model.name
                wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight
            }
            Rectangle { anchors.fill: parent; color: "black"; opacity: cover.current ? 0 : 0.28 }
            Column {
                anchors.right: parent.right; anchors.top: parent.top
                anchors.margins: Style.px(8, shelf.Window.window)
                spacing: Style.px(4, shelf.Window.window)
                Repeater {
                    model: [qsTr("Running"), qsTr("Hidden")]
                    delegate: Rectangle {
                        visible: index === 0 ? cover.appRunning : cover.appHidden
                        width: Math.min(cover.width - Style.px(16, shelf.Window.window), stateLabel.implicitWidth + Style.px(16, shelf.Window.window))
                        height: visible ? stateLabel.implicitHeight + Style.px(8, shelf.Window.window) : 0
                        radius: Style.px(4, shelf.Window.window)
                        color: Style.background
                        ConsoleText {
                            id: stateLabel; anchors.centerIn: parent; role: "caption"
                            width: parent.width - Style.px(16, shelf.Window.window); elide: Text.ElideRight
                            text: modelData; color: index === 0 ? Style.ok : Style.muted
                        }
                    }
                }
            }
            ConsoleFocus { focused: cover.selected }
            MouseArea {
                anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: function(mouse) {
                    shelf.currentIndex = index; shelf.forceActiveFocus()
                    if (mouse.button === Qt.RightButton) shelf.detailsRequested()
                }
                onDoubleClicked: { shelf.currentIndex = index; shelf.forceActiveFocus(); shelf.playRequested() }
                onPressAndHold: { shelf.currentIndex = index; shelf.forceActiveFocus(); shelf.detailsRequested() }
            }
        }
    }
}
