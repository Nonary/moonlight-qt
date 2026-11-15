import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2

import ComputerModel 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0
import "ConsoleStyle.js" as Style

CenteredGridView {
    property ComputerModel computerModel : createModel()

    property bool startupRestorePending: true
    readonly property string rememberedStatus: selectedHost.uuid === StreamingPreferences.uiLastHostUuid && !selectedHost.online ?
        qsTr("%1 is unavailable. Use Options to wake it, or choose another PC.").arg(selectedHost.name) : ""
    function openHost(row, allApps, automatic) {
        var host = computerModel.get(row)
        if (!host.online || !host.paired || !host.serverSupported) return false
        startupRestorePending = false
        StreamingPreferences.rememberUiSelection(host.uuid, 0)
        var component = Qt.createComponent("AppView.qml")
        stackView.push(component.createObject(stackView, {
            "computerIndex": row, "hostUuid": host.uuid, "objectName": host.name,
            "showHiddenGames": allApps, "showGames": automatic
        }))
        return true
    }
    function openRememberedHost() {
        var row = computerModel.indexOfUuid(StreamingPreferences.uiLastHostUuid)
        if (row < 0) return false
        currentIndex = row
        return openHost(row, false, true)
    }
    function tryStartupRestore() {
        if (!startupRestorePending || StackView.status !== StackView.Active) return
        if (typeof window !== "undefined" && window.popupHasFocus()) return
        var row = computerModel.indexOfUuid(StreamingPreferences.uiLastHostUuid)
        if (row >= 0 && !computerModel.get(row).statusUnknown) {
            openRememberedHost()
            startupRestorePending = false
        }
    }
    Connections {
        target: pcGrid.computerModel
        function onDataChanged() { pcGrid.refreshHost(); Qt.callLater(pcGrid.tryStartupRestore) }
        function onRowsInserted() { Qt.callLater(pcGrid.refreshAfterModelChange) }
        function onRowsRemoved() { Qt.callLater(pcGrid.refreshAfterModelChange) }
        function onModelReset() { Qt.callLater(pcGrid.refreshAfterModelChange) }
        function onLayoutChanged() { Qt.callLater(pcGrid.refreshAfterModelChange) }
    }
    Timer { interval: 200; running: true; onTriggered: pcGrid.tryStartupRestore() }
    ConsoleBackdrop { parent: pcGrid; anchors.fill: parent; z: -1 }
    readonly property real uiScale: Style.windowScale(Window.window)
    property var selectedHost: ({})
    readonly property bool optionsAvailable: count > 0 && !addingPc
    readonly property bool addingPc: footerItem && footerItem.button.activeFocus
    readonly property string primaryVerb: count === 0 || addingPc ? qsTr("Add PC") : !selectedHost.online ? qsTr("Wake") : !selectedHost.paired ? qsTr("Pair") : qsTr("Open")
    function focusContent() {
        if (count === 0 && footerItem) footerItem.button.forceActiveFocus()
        else if (currentItem) currentItem.forceActiveFocus()
    }
    onCountChanged: Qt.callLater(function() {
        // Discovery continues while another page or the navigation rail owns focus.
        if (pcGrid.StackView.status !== StackView.Active || !pcGrid.visible || !pcGrid.activeFocus) return
        if (typeof window !== "undefined" && window.popupHasFocus()) return
        pcGrid.focusContent()
    })
    function primaryAction() {
        if (count === 0 || addingPc) addPcDialog.open()
        else if (currentItem) currentItem.activateHost()
    }
    function optionsAction() { if (currentItem) currentItem.pcContextMenu.open() }
    function refreshHost() { selectedHost = computerModel.get(currentIndex) }
    function refreshAfterModelChange() {
        // A reset or reorder can replace the host at an unchanged row number.
        refreshHost()
        tryStartupRestore()
    }
    onCurrentIndexChanged: refreshHost()
    Column {
        parent: pcGrid
        x: Style.px(64, pcGrid.Window.window)
        y: pcGrid.cardY + Style.px(176 + 24, pcGrid.Window.window)
        width: parent.width - 2 * x; spacing: Style.px(12, pcGrid.Window.window)
        ConsoleText { width: parent.width; role: "display"; text: pcGrid.count === 0 || pcGrid.addingPc ? qsTr("Add a PC") : pcGrid.selectedHost.name || qsTr("PCs"); elide: Text.ElideRight }
        ConsoleText {
            width: parent.width; role: "secondary"; wrapMode: Text.Wrap
            text: pcGrid.count === 0 ? qsTr("PCs on your network will appear here. You can also enter an address.") : pcGrid.addingPc ? qsTr("Enter the address of your gaming PC.") : pcGrid.rememberedStatus || (pcGrid.selectedHost.online ? qsTr("Ready to connect") : qsTr("Wake your PC, or choose another host."))
        }
    }
    readonly property real cardY: Math.max(Style.px(24, Window.window), Math.min(Style.px(178, Window.window), height - Style.px(300, Window.window)))
    footer: Item {
        width: pcGrid.cellWidth; height: pcGrid.height
        property alias button: addPcTile
        ConsoleButton {
            id: addPcTile; focus: false; objectName: "addPcTile"
            width: Style.px(296, pcGrid.Window.window); height: Style.px(176, pcGrid.Window.window)
            y: pcGrid.cardY; text: qsTr("+ Add PC"); textOnly: true
            background: Rectangle {
                color: addPcTile.focusVisible || addPcTile.hovered ? Style.control : Style.surface
                radius: Style.px(6, pcGrid.Window.window); border.width: 1; border.color: Style.outline
                ConsoleFocus { focused: addPcTile.activeFocus; radius: parent.radius }
            }
            Keys.onMenuPressed: { }
            onClicked: addPcDialog.open()
            Keys.onLeftPressed: pcGrid.focusContent()
            Keys.onUpPressed: { if (typeof window !== "undefined") window.focusNavigation() }
        }
    }
    Rectangle {
        parent: pcGrid; anchors.fill: parent; color: Style.background; z: 10
        visible: pcGrid.startupRestorePending && StreamingPreferences.uiLastHostUuid.length > 0 && pcGrid.computerModel.indexOfUuid(StreamingPreferences.uiLastHostUuid) >= 0
        ConsoleText { anchors.centerIn: parent; role: "title"; text: qsTr("Connecting to your PC…") }
    }
    Keys.onPressed: startupRestorePending = false
    id: pcGrid
    focus: true
    activeFocusOnTab: true
    topMargin: 0; bottomMargin: 0
    minMargin: Style.px(64, Window.window)
    horizontalMargin: minMargin
    flow: GridView.FlowTopToBottom
    cellWidth: Style.px(320, Window.window); cellHeight: height
    boundsBehavior: Flickable.StopAtBounds
    objectName: qsTr("Computers")

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = 0
    }

    // Note: Any initialization done here that is critical for streaming must
    // also be done in CliStartStreamSegue.qml, since this code does not run
    // for command-line initiated streams.
    StackView.onActivated: {
        Qt.callLater(function() {
            if (pcGrid.StackView.status !== StackView.Active || !pcGrid.visible) return
            if (typeof window !== "undefined" && window.popupHasFocus()) return
            pcGrid.focusContent()
        })
        // Setup signals on CM
        ComputerManager.computerAddCompleted.connect(addComplete)

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }
    }

    StackView.onDeactivating: {
        startupRestorePending = false
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function pairingComplete(error)
    {
        // Close the PIN dialog
        pairDialog.close()

        // Display a failed dialog if we got an error
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.helpText = ""
            errorDialog.open()
        }
    }

    function addComplete(success, detectedPortBlocking)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            if (detectedPortBlocking) {
                errorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Moonlight. Streaming over the Internet may not work while connected to this network.")
            }
            else {
                errorDialog.helpText = qsTr("Click the Help button for possible solutions.")
            }

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.pairingCompleted.connect(pairingComplete)
        model.connectionTestCompleted.connect(testConnectionDialog.connectionTestComplete)
        return model
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        width: pcGrid.cellWidth; height: pcGrid.height
        grid: pcGrid
        property alias pcContextMenu: pcContextMenuLoader.item
        background: Item { }
        Rectangle {
            x: 0; y: pcGrid.cardY
            width: Style.px(296, pcGrid.Window.window); height: Style.px(176, pcGrid.Window.window)
            radius: Style.px(6, pcGrid.Window.window); color: Style.surface
            Row {
                x: Style.px(24, pcGrid.Window.window); y: Style.px(24, pcGrid.Window.window); spacing: Style.px(8, pcGrid.Window.window)
                Rectangle { width: Style.px(8, pcGrid.Window.window); height: width; radius: width/2; color: model.online ? Style.ok : Style.caption; anchors.verticalCenter: parent.verticalCenter }
                ConsoleText { role: "caption"; text: model.statusUnknown ? qsTr("Checking…") : !model.online ? qsTr("Offline") : !model.paired ? qsTr("Pairing required") : qsTr("Online") }
            }
            ConsoleText {
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                anchors.margins: Style.px(24, pcGrid.Window.window)
                role: "heading"; text: model.name; maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight
            }
            ConsoleFocus { focused: pcGrid.currentItem && pcGrid.currentItem.activeFocus && pcGrid.currentIndex === index; radius: parent.radius }
        }
        Keys.onRightPressed: {
            if (index === pcGrid.count - 1 && pcGrid.footerItem) pcGrid.footerItem.button.forceActiveFocus()
            else pcGrid.moveCurrentIndexRight()
        }
        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                initiator: pcContextMenuLoader.parent
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    text: qsTr("View All Apps")
                    onTriggered: {
                        pcGrid.openHost(index, true, true)
                    }
                    visible: model.online && model.paired
                }
                NavigableMenuItem {
                    text: qsTr("Wake PC")
                    onTriggered: computerModel.wakeComputer(index)
                    visible: !model.online && model.wakeable
                }
                NavigableMenuItem {
                    text: qsTr("Test Network")
                    onTriggered: {
                        computerModel.testConnectionForComputer(index)
                        testConnectionDialog.open()
                    }
                }

                NavigableMenuItem {
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("View Details")
                    onTriggered: {
                        showPcDetailsDialog.pcDetails = model.details
                        showPcDetailsDialog.open()
                    }
                }
            }
        }

        onClicked: { pcGrid.currentIndex = index; pcGrid.forceActiveFocus() }
        onDoubleClicked: activateHost()
        Keys.onReturnPressed: activateHost()
        Keys.onEnterPressed: activateHost()
        Keys.onSpacePressed: activateHost()
        function activateHost() {
            pcGrid.startupRestorePending = false
            pcGrid.currentIndex = index
            if (model.online) {
                if (!model.serverSupported) {
                    errorDialog.text = qsTr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(model.name)
                    errorDialog.helpText = ""
                    errorDialog.open()
                }
                else if (model.paired) {
                    // go to game view
                    pcGrid.openHost(index, false, false)
                }
                else {
                    var pin = computerModel.generatePinString()

                    // Kick off pairing in the background
                    computerModel.pairComputer(index, pin)

                    // Display the pairing dialog
                    pairDialog.pin = pin
                    pairDialog.open()
                }
            } else if (!model.online) {
                // Using open() here because it may be activated by keyboard
                if (model.wakeable) computerModel.wakeComputer(index)
                else pcContextMenu.open()
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            // We must use open() here so the menu is positioned on
            // the ItemDelegate and not where the mouse cursor is
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog

        // Using Setup-Guide here instead of Troubleshooting because it's likely that users
        // will arrive here by forgetting to enable GameStream or not forwarding ports.
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide"
    }

    NavigableDialog {
        id: pairDialog
        objectName: "pairingDialog"
        property string pin: "0000"
        title: qsTr("Pair PC")
        standardButtons: Dialog.Cancel
        closePolicy: Popup.CloseOnEscape
        ColumnLayout {
            width: Math.min(Style.px(496, pairDialog.hostWindow), (Overlay.overlay ? Overlay.overlay.width : 800) - 2 * pairDialog.padding)
            spacing: Style.px(24, pairDialog.hostWindow)
            ConsoleText { Layout.fillWidth: true; text: qsTr("Enter this PIN on your host PC."); wrapMode: Text.Wrap }
            ConsoleText { text: pairDialog.pin; font.pixelSize: Style.typePx(56, pairDialog.hostWindow); font.weight: Font.DemiBold; font.letterSpacing: Style.px(8, pairDialog.hostWindow) }
            ConsoleText { Layout.fillWidth: true; role: "secondary"; text: qsTr("For Sunshine, open the PIN page in its web interface."); wrapMode: Text.Wrap }
            ConsoleText { role: "secondary"; text: qsTr("Waiting for host…") }
        }
    }

    NavigableMessageDialog {
        id: deletePcDialog
        acceptText: qsTr("Remove PC")
        rejectText: qsTr("Keep PC")
        // don't allow edits to the rest of the window while open
        property int pcIndex : -1
        property string pcName : ""
        initialFocusButton: Dialog.No
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableMessageDialog {
        id: testConnectionDialog
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok

        onAboutToShow: {
            testConnectionDialog.text = qsTr("Moonlight is testing your network connection to determine if any required ports are blocked.") + "\n\n" + qsTr("This may take a few seconds…")
            showSpinner = true
        }

        function connectionTestComplete(result, blockedPorts)
        {
            if (result === -1) {
                text = qsTr("The network test could not be performed because none of Moonlight's connection testing servers were reachable from this PC. Check your Internet connection or try again later.")
                imageSrc = "qrc:/res/baseline-warning-24px.svg"
            }
            else if (result === 0) {
                text = qsTr("This network does not appear to be blocking Moonlight. If you still have trouble connecting, check your PC's firewall settings.") + "\n\n" + qsTr("If you are trying to stream over the Internet, install the Moonlight Internet Hosting Tool on your gaming PC and run the included Internet Streaming Tester to check your gaming PC's Internet connection.")
                imageSrc = "qrc:/res/baseline-check_circle_outline-24px.svg"
            }
            else {
                text = qsTr("Your PC's current network connection seems to be blocking Moonlight. Streaming over the Internet may not work while connected to this network.") + "\n\n" + qsTr("The following network ports were blocked:") + "\n"
                text += blockedPorts
                imageSrc = "qrc:/res/baseline-error_outline-24px.svg"
            }

            // Stop showing the spinner and show the image instead
            showSpinner = false
        }
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex : -1;

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            ConsoleTextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    if (controllerEditing) openKeyboard()
                    else renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    if (controllerEditing) openKeyboard()
                    else renamePcDialog.accept()
                }
            }
        }
    }

    NavigableMessageDialog {
        id: showPcDetailsDialog
        property string pcDetails : "";
        text: showPcDetailsDialog.pcDetails
        imageSrc: "qrc:/res/baseline-help_outline-24px.svg"
        standardButtons: Dialog.Ok
    }

    ScrollBar.vertical: ScrollBar {}
}
