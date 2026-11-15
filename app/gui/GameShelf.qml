import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import StreamingPreferences 1.0
import "ConsoleStyle.js" as Style

ListView {
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
    orientation: ListView.Horizontal
    clip: true
    activeFocusOnTab: true
    leftMargin: Style.px(64, Window.window)
    rightMargin: leftMargin
    spacing: Style.px(20, Window.window)
    boundsBehavior: Flickable.StopAtBounds
    highlightMoveDuration: StreamingPreferences.uiReducedMotion ? 0 : 120
    highlightRangeMode: ListView.ApplyRange
    preferredHighlightBegin: leftMargin
    preferredHighlightEnd: width - rightMargin

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
    Keys.onLeftPressed: { if (currentIndex > 0) decrementCurrentIndex() }
    Keys.onRightPressed: { if (currentIndex < count - 1) incrementCurrentIndex() }
    Keys.onUpPressed: moveUp()
    Keys.onDownPressed: moveDown()
    Keys.onReturnPressed: playRequested()
    Keys.onEnterPressed: playRequested()
    Keys.onSpacePressed: playRequested()
    Keys.onMenuPressed: detailsRequested()
    delegate: Item {
        width: cover.width
        height: shelf.height
        Rectangle {
            id: cover
            readonly property bool current: shelf.currentIndex === index && shelf.showCurrent
            readonly property bool selected: current && shelf.activeFocus
            readonly property bool appRunning: !!model.running
            readonly property bool appHidden: !!model.hidden
            x: 0; y: Style.px(6, shelf.Window.window)
            height: parent.height - 2 * y
            width: height * 0.72
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
