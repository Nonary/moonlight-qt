import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import StreamingPreferences 1.0
import SystemProperties 1.0
import AppModel 1.0
import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0
import "ConsoleStyle.js" as Style
import "PopupFocus.js" as PopupFocus

FocusScope {
    id: appPage
    property int computerIndex
    property string hostUuid: ""
    property bool showHiddenGames: false
    property bool showGames: false
    property bool activated: false
    property bool sleepPending: false
    property bool detailsExpanded: false
    readonly property int currentIndex: activeShelf.currentIndex
    readonly property var currentItem: activeShelf.currentItem
    property bool ready: false
    // A view can defer countChanged while its model changes during navigation.
    // Availability and layout follow the source notifications instead.
    property int libraryCount: 0
    property int recentCount: 0
    function refreshLibraryCount() { libraryCount = appModel.rowCount() }
    function refreshRecentCount() { recentCount = recentModel.rowCount() }
    readonly property real shelfHeight: Math.max(Style.px(100, Window.window), Math.min(Style.px(196, Window.window),
        Math.floor((shelfScroll.height - recentHeading.height - libraryHeading.height - shelfRows.spacing -
                    recentRow.spacing - libraryRow.spacing - (appPage.recentCount ? 0 : recentEmpty.height + recentRow.spacing)) /
                   (appPage.recentCount ? 2 : 1))))
    property bool recentRowActive: false
    readonly property var activeShelf: recentRowActive ? recentShelf : shelf
    readonly property bool hasGames: appPage.libraryCount > 0
    readonly property bool hasMultipleShelves: appPage.recentCount > 0 && hasGames
    property var selectedApp: ({})
    readonly property int selectedAppId: selectedApp.appid || 0
    property var videoOverrides: ({})
    onSelectedAppIdChanged: refreshVideoOverrides()
    onHostUuidChanged: refreshVideoOverrides()
    property AppModel appModel: AppModel { includeHostActions: false }
    property RecentAppsModel recentModel: RecentAppsModel { sourceModel: appPage.appModel }
    readonly property real uiScale: Style.windowScale(Window.window)
    // Invokable lookups have no model-change notification for a QML binding.
    // Catalogue rows can move independently of the selected Recent shelf item.
    function selectedAppIndex() { return selectedApp.appid ? appModel.indexOfApp(selectedApp.appid) : -1 }
    focus: true

    function refreshVideoOverrides() { videoOverrides = StreamingPreferences.appVideoOverrides(hostUuid, selectedAppId) }

    function restoreSelection() {
        shelf.restoreSelection()
        recentShelf.restoreSelection()
        updateSelection()
    }
    function updateSelection() {
        if (!ready || activeShelf.modelChanging) return
        selectedApp = activeShelf.currentApp()
        if (selectedApp.appid) StreamingPreferences.rememberUiSelection(hostUuid, selectedApp.appid)
        else if (detailsExpanded) collapseDetails()
    }
    function focusShelf(recent) {
        if (!hasGames) { choosePcButton.forceActiveFocus(); return }
        recentRowActive = recent && appPage.recentCount > 0
        activeShelf.forceActiveFocus()
        updateSelection()
        Qt.callLater(revealShelf)
    }
    function revealShelf() {
        if (detailsExpanded) return
        var row = recentRowActive ? recentRow : libraryRow
        if (row.y < shelfScroll.contentY) shelfScroll.contentY = row.y
        else if (row.y + row.height > shelfScroll.contentY + shelfScroll.height)
            shelfScroll.contentY = row.y + row.height - shelfScroll.height
    }
    function expandDetails() {
        if (!selectedApp.appid) return
        detailsExpanded = true
        playButton.forceActiveFocus()
        Qt.callLater(function() { if (appPage.detailsExpanded && !PopupFocus.hasVisibleModal()) playButton.forceActiveFocus() })
    }
    function collapseDetails() { detailsExpanded = false; focusShelf(recentRowActive) }
    function launchSelected() {
        var row = selectedAppIndex()
        if (row < 0) return
        var runningId = appModel.getRunningAppId()
        if (runningId !== 0 && runningId !== selectedApp.appid) {
            quitAppDialog.appName = appModel.getRunningAppName()
            quitAppDialog.segueToStream = true
            quitAppDialog.nextAppName = selectedApp.name
            quitAppDialog.nextAppId = selectedApp.appid
            quitAppDialog.open()
            return
        }
        var session = appModel.createSessionForApp(row)
        if (!session) return
        var component = Qt.createComponent("StreamSegue.qml")
        stackView.push(component.createObject(stackView, {
            "appName": selectedApp.name, "artwork": selectedApp.boxart,
            "session": session, "isResume": runningId === selectedApp.appid
        }))
    }
    function quitSelected() {
        quitAppDialog.appName = appModel.getRunningAppName()
        quitAppDialog.segueToStream = false
        quitAppDialog.open()
    }
    Component.onCompleted: {
        appModel.initialize(ComputerManager, computerIndex, showHiddenGames)
        refreshLibraryCount()
        recentModel.appIds = StreamingPreferences.recentUiApps(hostUuid)
        refreshRecentCount()
        var wanted = StreamingPreferences.selectedUiApp(hostUuid)
        shelf.restoreSelection(wanted)
        recentShelf.restoreSelection(wanted)
        recentRowActive = appPage.recentCount > 0 && (!wanted || recentModel.indexOfApp(wanted) >= 0)
        ready = true
        updateSelection()
        Qt.callLater(function() { if (appPage.activeFocus && !appPage.detailsExpanded) appPage.focusShelf(appPage.recentRowActive) })
    }
    StackView.onActivated: {
        activated = true
        if (!PopupFocus.hasVisibleModal()) SdlGamepadKeyNavigation.setUiNavMode(false)
        restoreSelection()
        if (!showGames && !showHiddenGames) {
            showGames = true
            var direct = appModel.getDirectLaunchAppIndex()
            if (direct >= 0 && appModel.getRunningAppId() === 0) {
                recentRowActive = false; shelf.currentIndex = direct; updateSelection(); launchSelected()
            }
        }
    }
    StackView.onDeactivating: activated = false
    onActiveFocusChanged: {
        if (activeFocus && !activeShelf.activeFocus && !detailsExpanded) focusShelf(recentRowActive)
    }
    Connections {
        target: appPage.appModel
        function onRowsInserted() { appPage.refreshLibraryCount() }
        function onRowsRemoved() { appPage.refreshLibraryCount() }
        function onModelReset() { appPage.refreshLibraryCount() }
        function onComputerLost() { if (appPage.activated) stackView.pop(null) }
        function onHostSleepCompleted(error) {
            appPage.sleepPending = false
            sleepResult.imageSrc = error.length ? "qrc:/res/baseline-error_outline-24px.svg" : "qrc:/res/baseline-check_circle_outline-24px.svg"
            sleepResult.text = error.length ? error : qsTr("The host accepted the sleep request. Sleep completion depends on the host. No stream was started.")
            sleepResult.open()
        }
    }
    Connections {
        target: StreamingPreferences
        function onAppVideoOverridesChanged(uuid, appId) {
            if (uuid === appPage.hostUuid && appId === appPage.selectedAppId) appPage.refreshVideoOverrides()
        }
        function onRecentUiAppsChanged(uuid) {
            if (uuid === appPage.hostUuid) appPage.recentModel.appIds = StreamingPreferences.recentUiApps(uuid)
        }
    }
    Connections {
        target: appPage.recentModel
        function onRowsInserted() { appPage.refreshRecentCount() }
        function onRowsRemoved() { appPage.refreshRecentCount() }
        function onModelReset() { appPage.refreshRecentCount() }
    }
    onRecentCountChanged: {
        if (!ready || recentCount > 0 || !recentRowActive) return
        recentRowActive = false
        if (!detailsExpanded && visible && activeFocus) shelf.forceActiveFocus()
        updateSelection()
    }
    readonly property string primaryVerb: !hasGames ? (emptySleepButton.activeFocus ? qsTr("Sleep PC") : qsTr("Choose PC")) : selectedApp.running ? qsTr("Resume") : qsTr("Play")
    function primaryAction() { if (hasGames) launchSelected(); else if (emptySleepButton.activeFocus && emptySleepButton.enabled) sleepDialog.open(); else stackView.pop(null) }
    Keys.onMenuPressed: { if (detailsExpanded) collapseDetails(); else if (hasGames) expandDetails() }
    onHasGamesChanged: {
        if (!ready) return
        Qt.callLater(function() {
            if (!appPage.visible || !appPage.activeFocus) return
            if (!appPage.hasGames) appPage.collapseDetails()
            else appPage.focusShelf(appPage.recentRowActive)
        })
    }
    function optionsAction() { expandDetails() }
    function detailsAction() { expandDetails() }
    ConsoleBackdrop { anchors.fill: parent; artwork: appPage.selectedApp.boxart || "" }
    Item {
        id: libraryBrowser
        anchors.fill: parent
        visible: !appPage.detailsExpanded && appPage.hasGames
        Column {
            id: selectionSummary
            x: Style.px(64, appPage.Window.window); y: Style.px(12, appPage.Window.window)
            width: parent.width - 2 * x
            spacing: Style.px(4, appPage.Window.window)
            ConsoleText { width: parent.width; role: "title"; text: appPage.selectedApp.name || ""; elide: Text.ElideRight }
            ConsoleText {
                width: parent.width; role: "secondary"; elide: Text.ElideRight
                text: appPage.selectedApp.running ? qsTr("Running on %1").arg(appPage.objectName) :
                    appPage.objectName
            }
        }
        Flickable {
            id: shelfScroll
            objectName: "gameShelfScroll"
            y: selectionSummary.y + selectionSummary.height + Style.px(20, appPage.Window.window)
            width: parent.width; height: parent.height - y
            contentHeight: shelfRows.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            onHeightChanged: Qt.callLater(appPage.revealShelf)
            Column {
                id: shelfRows
                width: shelfScroll.width
                spacing: Style.px(16, appPage.Window.window)
                Column {
                    id: recentRow
                    width: parent.width
                    spacing: Style.px(4, appPage.Window.window)
                    Row {
                        id: recentHeading
                        x: Style.px(64, appPage.Window.window)
                        spacing: Style.px(16, appPage.Window.window)
                        ConsoleText { role: "heading"; text: qsTr("Recently played") }
                        ConsoleText { role: "caption"; text: qsTr("Latest first"); visible: appPage.recentCount > 0; anchors.verticalCenter: parent.verticalCenter }
                    }
                    ConsoleText {
                        id: recentEmpty
                        x: Style.px(64, appPage.Window.window)
                        width: parent.width - 2 * x
                        role: "secondary"; text: qsTr("Games you play will appear here.")
                        visible: appPage.recentCount === 0
                    }
                    GameShelf {
                        id: recentShelf; objectName: "recentGameShelf"
                        showCurrent: appPage.recentRowActive
                        width: parent.width; height: appPage.recentCount > 0 ? appPage.shelfHeight : 0
                        visible: appPage.recentCount > 0
                        model: appPage.recentModel
                        onSelectionChanged: {
                            if (activeFocus) appPage.recentRowActive = true
                            if (appPage.recentRowActive) appPage.updateSelection()
                        }
                        onActiveFocusChanged: { if (activeFocus) Qt.callLater(appPage.revealShelf) }

                        onMoveUp: { if (typeof window !== "undefined") window.focusNavigation() }
                        onMoveDown: appPage.focusShelf(false)
                        onPlayRequested: appPage.launchSelected()
                        onDetailsRequested: appPage.expandDetails()
                    }
                }
                Column {
                    id: libraryRow
                    width: parent.width
                    spacing: Style.px(4, appPage.Window.window)
                    ConsoleText {
                        x: Style.px(64, appPage.Window.window)
                        id: libraryHeading; role: "heading"; text: qsTr("All games · %1").arg(appPage.libraryCount)
                    }
                    GameShelf {
                        id: shelf; objectName: "gameShelf"
                        showCurrent: !appPage.recentRowActive
                        width: parent.width; height: appPage.shelfHeight
                        model: appPage.appModel
                        focus: true
                        onSelectionChanged: {
                            if (activeFocus) appPage.recentRowActive = false
                            if (!appPage.recentRowActive) appPage.updateSelection()
                        }
                        onActiveFocusChanged: { if (activeFocus) Qt.callLater(appPage.revealShelf) }
                        onMoveUp: {
                            if (appPage.recentCount > 0) appPage.focusShelf(true)
                            else if (typeof window !== "undefined") window.focusNavigation()
                        }
                        onMoveDown: { }
                        onPlayRequested: appPage.launchSelected()
                        onDetailsRequested: appPage.expandDetails()
                    }
                }
            }
            ScrollBar.vertical: ScrollBar { }
        }
    }
    Rectangle {
        visible: appPage.detailsExpanded
        x: Style.px(64, appPage.Window.window); y: Style.px(32, appPage.Window.window)
        height: Math.min(Style.px(338, appPage.Window.window), appPage.height * 0.7)
        width: height * 0.72
        color: Style.surface
        border.width: 1; border.color: Style.outline
        Image {
            anchors.fill: parent
            source: Style.placeholder(appPage.selectedApp.boxart) ? "" : appPage.selectedApp.boxart
            fillMode: Image.PreserveAspectCrop; asynchronous: true
        }
        ConsoleText {
            visible: Style.placeholder(appPage.selectedApp.boxart)
            anchors.fill: parent; anchors.margins: Style.px(16, appPage.Window.window)
            role: "heading"; text: appPage.selectedApp.name || ""
            wrapMode: Text.Wrap; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter
        }
    }
    Flickable {
        id: detailScroll
        objectName: "gameDetails"
        visible: appPage.detailsExpanded
        x: Style.px(340, appPage.Window.window); y: Style.px(24, appPage.Window.window)
        width: Math.min(Style.px(520, appPage.Window.window), appPage.width - x - Style.px(64, appPage.Window.window))
        height: appPage.height - y - Style.px(16, appPage.Window.window)
        contentHeight: detailColumn.height
        clip: true
        function reveal(item) {
            var pos = item.mapToItem(contentItem, 0, 0)
            if (pos.y < contentY) contentY = pos.y
            else if (pos.y + item.height > contentY + height) contentY = pos.y + item.height - height
        }
        Window.onActiveFocusItemChanged: {
            var item = Window.activeFocusItem
            if (item && item.parent === detailColumn) reveal(item)
        }
        Column {
            id: detailColumn; width: parent.width; spacing: Style.px(8, appPage.Window.window)
            ConsoleText { role: "title"; text: appPage.selectedApp.name || ""; width: parent.width; maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight }
            ConsoleText { role: "secondary"; text: appPage.objectName; bottomPadding: Style.px(16, appPage.Window.window) }
            ConsoleListRow {
                id: playButton; objectName: "gamePlayButton"
                width: parent.width; text: appPage.primaryVerb; textOnly: true; textAlignment: Text.AlignLeft
                onClicked: appPage.launchSelected()
                KeyNavigation.down: quitGame.visible ? quitGame : streamProfileButton
                Keys.onUpPressed: appPage.collapseDetails()
            }
            ConsoleListRow {
                id: quitGame; width: parent.width; visible: !!appPage.selectedApp.running
                text: qsTr("Quit game"); textOnly: true; textAlignment: Text.AlignLeft
                onClicked: appPage.quitSelected()
                KeyNavigation.up: playButton; KeyNavigation.down: streamProfileButton
            }
            ConsoleListRow {
                id: streamProfileButton; objectName: "gameStreamProfileButton"
                width: parent.width; text: qsTr("Stream profile")
                onClicked: streamProfile.openFor(appPage.hostUuid, appPage.selectedAppId, appPage.selectedApp.name, appPage.videoOverrides)
                KeyNavigation.up: quitGame.visible ? quitGame : playButton
                KeyNavigation.down: directButton
            }
            ConsoleListRow {
                id: directButton; width: parent.width
                text: qsTr("Direct launch") + (appPage.selectedApp.directLaunch ? qsTr(" · On") : qsTr(" · Off"))
                enabled: !!appPage.selectedApp.appid && !appPage.selectedApp.hidden
                textOnly: true; textAlignment: Text.AlignLeft
                onClicked: {
                    var row = appPage.selectedAppIndex()
                    if (row >= 0) appPage.appModel.setAppDirectLaunch(row, !appPage.selectedApp.directLaunch)
                }
                KeyNavigation.up: streamProfileButton; KeyNavigation.down: hideButton
            }
            ConsoleListRow {
                id: hideButton; objectName: "hideGameButton"; width: parent.width
                text: appPage.selectedApp.hidden ? qsTr("Show in library") : qsTr("Hide from library")
                enabled: !!appPage.selectedApp.appid && (appPage.selectedApp.hidden || (!appPage.selectedApp.running && !appPage.selectedApp.directLaunch))
                textOnly: true; textAlignment: Text.AlignLeft
                onClicked: {
                    var row = appPage.selectedAppIndex()
                    if (row >= 0) appPage.appModel.setAppHidden(row, !appPage.selectedApp.hidden)
                    appPage.collapseDetails()
                }
                KeyNavigation.up: directButton; KeyNavigation.down: streamSettingsButton
            }
            ConsoleListRow {
                id: streamSettingsButton; width: parent.width; text: qsTr("Stream settings"); textOnly: true; textAlignment: Text.AlignLeft
                onClicked: { if (typeof window !== "undefined") window.openSettings(0) }
                KeyNavigation.up: hideButton
                Keys.onDownPressed: { if (sleepButton.visible) sleepButton.forceActiveFocus() }
            }
            ConsoleListRow {
                id: sleepButton; objectName: "hostSleepAction"; width: parent.width
                visible: appPage.appModel.hostSleepAvailable; enabled: !appPage.sleepPending
                text: qsTr("Sleep PC"); textOnly: true; textAlignment: Text.AlignLeft
                onClicked: sleepDialog.open()
                KeyNavigation.up: streamSettingsButton
                Keys.onDownPressed: { }
            }
            Rectangle { width: parent.width; height: 1; color: Style.outline }
            GridLayout {
                columns: 2; columnSpacing: Style.px(24, appPage.Window.window); rowSpacing: Style.px(8, appPage.Window.window)
                readonly property var overrides: appPage.videoOverrides
                readonly property size resolution: overrides.resolution || Qt.size(StreamingPreferences.width, StreamingPreferences.height)
                readonly property int requestedFps: overrides.fps === undefined ? StreamingPreferences.fps : overrides.fps
                readonly property int fpsLimit: StreamingPreferences.enableVrr && StreamingPreferences.enableVsync &&
                                                StreamingPreferences.ultraLowLatencyVrr ? SystemProperties.getUltraLowLatencyVrrFpsLimit() : 0
                readonly property int fps: fpsLimit > 0 ? Math.min(requestedFps, fpsLimit) : requestedFps
                readonly property int bitrate: overrides.bitrateKbps === undefined ? StreamingPreferences.bitrateKbps : overrides.bitrateKbps
                readonly property bool hdr: overrides.hdr === undefined ? StreamingPreferences.enableHdr : overrides.hdr
                ConsoleText { role: "secondary"; text: qsTr("Resolution") }
                ConsoleText { role: "secondary"; text: qsTr("%1 × %2").arg(parent.resolution.width).arg(parent.resolution.height) }
                ConsoleText { role: "secondary"; text: qsTr("Frame rate") }
                ConsoleText { role: "secondary"; text: qsTr("%1 FPS").arg(parent.fps) }
                ConsoleText { role: "secondary"; text: qsTr("Bitrate") }
                ConsoleText { role: "secondary"; text: qsTr("%1 Mbps").arg(parent.bitrate / 1000) }
                ConsoleText { role: "secondary"; text: qsTr("HDR") }
                ConsoleText { role: "secondary"; text: parent.hdr ? qsTr("On") : qsTr("Off") }
            }
            ConsoleText { role: "caption"; text: qsTr("Requested settings. The negotiated stream mode may differ."); width: parent.width; wrapMode: Text.Wrap }
        }
        ScrollBar.vertical: ScrollBar { }
    }
    GameStreamProfile {
        id: streamProfile
        onClosed: if (appPage.activated && appPage.detailsExpanded) streamProfileButton.forceActiveFocus()
    }
    Column {
        x: Style.px(64, appPage.Window.window); y: Style.px(100, appPage.Window.window)
        width: parent.width - 2 * x; spacing: Style.px(24, appPage.Window.window); visible: !appPage.hasGames
        ConsoleText { objectName: "emptyLibraryTitle"; width: parent.width; text: appPage.appModel.appListLoading ? qsTr("Loading games…") : appPage.appModel.appListUnavailable ? qsTr("Games unavailable") : qsTr("No games available"); role: "display"; wrapMode: Text.Wrap }
        ConsoleText { width: parent.width; text: appPage.appModel.appListLoading ? qsTr("Waiting for the game list from %1.").arg(appPage.objectName) : appPage.appModel.appListUnavailable ? qsTr("Could not load games from this PC. Check the host and connection, or choose another PC.") : qsTr("Add apps on your host, or choose View all apps from its options."); role: "secondary"; wrapMode: Text.Wrap }
        ConsoleButton {
            id: choosePcButton; objectName: "emptyLibraryChoosePc"; text: qsTr("Choose PC"); onClicked: stackView.pop(null)
            Keys.onUpPressed: { if (typeof window !== "undefined") window.focusNavigation() }
            Keys.onDownPressed: { if (emptySleepButton.visible && emptySleepButton.enabled) emptySleepButton.forceActiveFocus() }
        }
        ConsoleButton { id: emptySleepButton; KeyNavigation.up: choosePcButton; Keys.onDownPressed: { }
            objectName: "emptyHostSleepAction"; visible: appPage.appModel.hostSleepAvailable; text: qsTr("Sleep PC"); enabled: !appPage.sleepPending; onClicked: sleepDialog.open() }
    }
    NavigableMessageDialog {
        id: quitAppDialog
        objectName: "quitRunningGameDialog"
        acceptText: qsTr("Quit game")
        rejectText: qsTr("Keep playing")
        property string appName : ""
        property bool segueToStream : false
        property string nextAppName: ""
        property int nextAppId: 0
        text:qsTr("Are you sure you want to quit %1? Any unsaved progress will be lost.").arg(appName)
        standardButtons: Dialog.Yes | Dialog.No
        initialFocusButton: Dialog.No

        function quitApp() {
            var component = Qt.createComponent("QuitSegue.qml")
            var params = {"appName": appName, "quitRunningAppFn": function() { appModel.quitRunningApp() }}
            if (segueToStream) {
                // Store the launch details if we're going to stream after
                // successfully quitting the old app.
                params.nextAppName = nextAppName
                var nextIndex = appModel.indexOfApp(nextAppId)
                if (nextIndex < 0) return
                params.nextAppArtwork = appModel.get(nextIndex).boxart
                params.nextSession = appModel.createSessionForApp(nextIndex)
            }
            else {
                params.nextAppName = null
                params.nextSession = null
            }

            stackView.push(component.createObject(stackView, params))
        }

        onAccepted: quitApp()
    }

    NavigableMessageDialog {
        id: sleepDialog
        acceptText: qsTr("Sleep PC")
        rejectText: qsTr("Cancel")
        objectName: "hostSleepConfirmation"
        title: qsTr("Put PC to Sleep")
        text: qsTr("Put this host PC to sleep? All host applications and streaming sessions must be closed first. You will need a way to wake the PC again.")
        standardButtons: Dialog.Yes | Dialog.No
        initialFocusButton: Dialog.No
        onAccepted: {
            if (!appPage.sleepPending) {
                appPage.sleepPending = true
                appModel.requestHostSleep()
            }
        }
    }

    NavigableMessageDialog {
        id: sleepResult
        objectName: "hostSleepResult"
        title: qsTr("Host Sleep")
        standardButtons: Dialog.Ok
    }


}
