import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0
import AutoUpdateChecker 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0
import "PopupFocus.js" as PopupFocus
import "ConsoleStyle.js" as Style

ApplicationWindow {
    property bool pollingActive: false

    // Set by SettingsView to force the back operation to pop all
    // pages except the initial view. This is required when doing
    // a retranslate() because AppView breaks for some reason.
    property bool clearOnBack: false

    id: window
    width: 1280
    height: 720
    minimumWidth: 800
    minimumHeight: 500
    color: Style.background
    readonly property real uiScale: Style.scale(width, height)
    font.family: "Inter"
    font.pixelSize: Style.typeSize("body", window)
    Material.theme: Material.Dark
    Material.accent: Style.accent
    Material.primary: Style.surface
    Material.background: Style.background
    Material.foreground: Style.text
    readonly property bool toolbarFocused: homeButton.activeFocus || pcsButton.activeFocus || settingsButton.activeFocus || uiFullscreenButton.activeFocus
    readonly property bool controllerInput: SdlGamepadKeyNavigation.inputMethod === "controller"
    property bool uiDisplayReady: false
    property bool controllerFullscreenRequested: false
    // -1 follows the startup policy; explicit choices last for this app session.
    property int uiFullscreenOverride: -1
    property bool applyingUiDisplayMode: false
    property int previousUiVisibility: Window.Hidden
    readonly property bool automaticControllerFullscreen: StreamingPreferences.uiControllerFullscreen && controllerFullscreenRequested
    readonly property string selectedHostName: stackView.currentItem instanceof AppView ? stackView.currentItem.objectName : ""

    // Controller fullscreen stays latched until explicitly exited. Input hints
    // can follow the mouse/keyboard without resizing the launcher.
    function applyUiDisplayMode(showInitially) {
        if (!uiDisplayReady || (!visible && !showInitially) || !toolBar.visible) return
        applyingUiDisplayMode = true
        var fullscreen = uiFullscreenOverride >= 0 ? uiFullscreenOverride === 1 :
            automaticControllerFullscreen || StreamingPreferences.uiDisplayMode === StreamingPreferences.UI_FULLSCREEN
        if (!SystemProperties.hasDesktopEnvironment || fullscreen) {
            if (visibility !== Window.FullScreen) showFullScreen()
        }
        else if (StreamingPreferences.uiDisplayMode === StreamingPreferences.UI_MAXIMIZED) {
            if (visibility !== Window.Maximized) showMaximized()
        }
        else if (visibility !== Window.Windowed) showNormal()
        applyingUiDisplayMode = false
    }
    function toggleUiFullscreen() {
        uiFullscreenOverride = visibility === Window.FullScreen ? 0 : 1
        applyUiDisplayMode()
    }
    onVisibilityChanged: function(visibility) {
        var previous = previousUiVisibility
        previousUiVisibility = visibility
        // Honor an explicit native window action as well as our button/F11.
        if (uiDisplayReady && !applyingUiDisplayMode && toolBar.visible && previous === Window.FullScreen &&
                (visibility === Window.Windowed || visibility === Window.Maximized)) uiFullscreenOverride = 0
    }
    onAutomaticControllerFullscreenChanged: Qt.callLater(applyUiDisplayMode)
    Connections {
        target: SdlGamepadKeyNavigation
        function onNavigationStateChanged() {
            if (SdlGamepadKeyNavigation.connectedGamepads > 0) window.controllerFullscreenRequested = true
        }
    }
    Connections {
        target: StreamingPreferences
        function onUiDisplayModeChanged() {
            // Choosing a display mode in Settings is an explicit window action.
            window.uiFullscreenOverride = StreamingPreferences.uiDisplayMode === StreamingPreferences.UI_FULLSCREEN ? 1 : 0
            Qt.callLater(window.applyUiDisplayMode)
        }
    }

    function focusNavigation() {
        if (stackView.currentItem instanceof SettingsView) settingsButton.forceActiveFocus()
        else if (stackView.currentItem instanceof PcView) pcsButton.forceActiveFocus()
        else homeButton.forceActiveFocus()
    }
    function requestQuit() { quitConfirmationDialog.open() }
    function openSettings(category) {
        navigateTo(Qt.resolvedUrl("SettingsView.qml"), SettingsView)
        if (stackView.currentItem instanceof SettingsView && category !== undefined) stackView.currentItem.category = category
    }
    function activateFocused() {
        var item = window.activeFocusItem
        while (item && item !== window.contentItem) {
            if (typeof item.clicked === "function") { item.clicked(); return }
            if (item.popup) { item.popup.open(); return }
            item = item.parent
        }
    }
    function activatePrimary() {
        if (window.popupHasFocus() || toolbarFocused) { activateFocused(); return }
        var page = stackView.currentItem
        if (page instanceof AppView && page.detailsExpanded) activateFocused()
        else if (page && typeof page.primaryAction === "function") page.primaryAction()
        else activateFocused()
    }
    function activateOptions() {
        var page = stackView.currentItem
        if (page && typeof page.optionsAction === "function") page.optionsAction()
    }
    function showPcs() { if (toolBar.visible) stackView.pop(null, StackView.Immediate) }
    function showHome() {
        if (!toolBar.visible) return
        var library = stackView.find(function(item) { return item instanceof AppView })
        if (library) { stackView.pop(library, StackView.Immediate); library.forceActiveFocus() }
        else {
            stackView.pop(null, StackView.Immediate)
            if (stackView.currentItem instanceof PcView) stackView.currentItem.openRememberedHost()
        }
    }
    function popupHasFocus() {
        var item = window.activeFocusItem
        while (item) {
            if (item === Overlay.overlay) return true
            if (typeof item.popup !== "undefined" && item.popup && item.popup.visible) return true
            item = item.parent
        }
        return PopupFocus.hasVisibleModal()
    }
    function switchSection(direction) {
        if (!toolBar.visible || popupHasFocus()) return
        var section = stackView.currentItem instanceof SettingsView ? 2 : stackView.currentItem instanceof AppView ? 0 : 1
        section = (section + direction + 3) % 3
        if (section === 0) showHome()
        else if (section === 1) showPcs()
        else settingsButton.clicked()
    }
    function controllerLabel(action) {
        if (!controllerInput) return action === "confirm" ? "Enter" : action === "back" ? "Esc" : action === "options" ? "Menu" : action === "done" ? "Enter" : "PgUp / PgDn"
        var family = SdlGamepadKeyNavigation.controllerFamily
        var swapped = StreamingPreferences.swapFaceButtons
        if (action === "sections") return family === "playstation" ? "L1 / R1" : family === "nintendo" ? "L / R" : "LB / RB"
        var labels = family === "playstation" ? ["×", "○", "□", "△"] : family === "nintendo" ? ["B", "A", "Y", "X"] : ["A", "B", "X", "Y"]
        return action === "done" ? labels[swapped ? 2 : 3] : action === "confirm" ? labels[swapped ? 1 : 0] : action === "back" ? labels[swapped ? 0 : 1] : labels[swapped ? 3 : 2]
    }


    // This function runs prior to creation of the initial StackView item
    function doEarlyInit() {
        // Override the background color to Material 2 colors for Qt 6.5+
        // in order to improve contrast between GFE's placeholder box art
        // and the background of the app grid.
        if (SystemProperties.usesMaterial3Theme) {
            Material.background = Style.background
        }

        SdlGamepadKeyNavigation.enable()
    }

    Component.onCompleted: {
        controllerFullscreenRequested = SdlGamepadKeyNavigation.connectedGamepads > 0
        uiDisplayReady = true
        applyUiDisplayMode(true)

        // Request keyboard focus once the startup window has been shown.
        Qt.callLater(function() {
            if (window.visible && window.visibility !== Window.Minimized && !window.active) {
                window.requestActivate()
            }
        })

        // Display any modal dialogs for configuration warnings
        if (runConfigChecks) {
            if (SystemProperties.isWow64) {
                wow64Dialog.open()
            }

            // Hardware acceleration and unmapped gamepads are checked asynchronously
            SystemProperties.hasHardwareAccelerationChanged.connect(hasHardwareAccelerationChanged)
            SystemProperties.unmappedGamepadsChanged.connect(hasUnmappedGamepadsChanged)
            SystemProperties.startAsyncLoad()
        }
    }

    function hasHardwareAccelerationChanged() {
        if (!SystemProperties.hasHardwareAcceleration && StreamingPreferences.videoDecoderSelection !== StreamingPreferences.VDS_FORCE_SOFTWARE) {
            if (SystemProperties.isRunningXWayland) {
                xWaylandDialog.open()
            }
            else {
                noHwDecoderDialog.open()
            }
        }
    }

    function hasUnmappedGamepadsChanged() {
        if (SystemProperties.unmappedGamepads) {
            unmappedGamepadDialog.unmappedGamepads = SystemProperties.unmappedGamepads
            unmappedGamepadDialog.open()
        }
    }

    // It would be better to use TextMetrics here, but it always lays out
    // the text slightly more compactly than real Text does in ToolTip,
    // causing unexpected line breaks to be inserted
    Item {
        visible: false
        Text {
            id: tooltipTextLayoutHelper
            font: ToolTip.toolTip.font
            text: ToolTip.toolTip.text
        }
        ToolTip.toolTip.contentWidth: Math.min(tooltipTextLayoutHelper.width, 400)
    }

    function goBack() {
        if (PopupFocus.rejectTopModal()) return
        if (stackView.currentItem instanceof SettingsView && !stackView.currentItem.categoryMenuFocused) {
            stackView.currentItem.backAction()
            return
        }
        if (stackView.currentItem instanceof AppView && stackView.currentItem.detailsExpanded) {
            stackView.currentItem.collapseDetails()
            return
        }
        if (clearOnBack) {
            // Pop all items except the first one
            stackView.pop(null)
            clearOnBack = false
        }
        else {
            stackView.pop()
        }
    }

    StackView {
        id: stackView
        property bool focusViewAfterDialog: false
        anchors.fill: parent
        focus: true
        pushEnter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: StreamingPreferences.uiReducedMotion ? 0 : 130 } }
        pushExit: Transition { }
        popEnter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: StreamingPreferences.uiReducedMotion ? 0 : 130 } }
        popExit: Transition { }

        Component.onCompleted: {
            // Perform our early initialization before constructing
            // the initial view and pushing it to the StackView
            doEarlyInit()
            push(initialView)
        }

        onCurrentItemChanged: {
            // Ensure focus travels to the next view when going back
            PopupFocus.focusCurrentView(stackView)
        }

        Window.onActiveFocusItemChanged: PopupFocus.keepModalFocus(Overlay.overlay)

        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
                window.switchSection(event.key === Qt.Key_PageUp ? -1 : 1)
                event.accepted = true
            }
        }

        Keys.onEscapePressed: {
            if (PopupFocus.rejectTopModal()) {
                return
            }
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onBackPressed: {
            if (PopupFocus.rejectTopModal()) {
                return
            }
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onMenuPressed: {
            settingsButton.clicked()
        }

        // This is a keypress we've reserved for letting the
        // SdlGamepadKeyNavigation object tell us to show settings
        // when Menu is consumed by a focused control.
        Keys.onHangupPressed: {
            settingsButton.clicked()
        }
    }

    // This timer keeps us polling for 5 minutes of inactivity
    // to allow the user to work with Moonlight on a second display
    // while dealing with configuration issues. This will ensure
    // machines come online even if the input focus isn't on Moonlight.
    Timer {
        id: inactivityTimer
        interval: 5 * 60000
        onTriggered: {
            if (!active && pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
    }

    onVisibleChanged: {
        // When we become invisible while streaming is going on,
        // stop polling immediately.
        if (!visible) {
            inactivityTimer.stop()

            if (pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
        else if (active) {
            // When we become visible and active again, start polling
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }

        if (visible) Qt.callLater(applyUiDisplayMode)

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    onActiveChanged: {
        if (active) {
            // Stop the inactivity timer
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
        else {
            // Start the inactivity timer to stop polling
            // if focus does not return within a few minutes.
            inactivityTimer.restart()
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    function navigateTo(url, objectType)
    {
        var existingItem = stackView.find(function(item, index) {
            return item instanceof objectType
        })

        if (existingItem !== null) {
            // Pop to the existing item
            stackView.pop(existingItem)
        }
        else {
            // Create a new item
            stackView.push(url, StackView.Immediate)
        }
    }

    header: ToolBar {
        id: toolBar
        Keys.forwardTo: [stackView]
        height: Style.px(72, window)
        background: Rectangle { color: Style.background }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: Style.px(64, window); anchors.rightMargin: Style.px(64, window)
            spacing: Style.px(24, window)
            Image { source: "qrc:/res/moonlight.svg"; sourceSize.width: Style.px(24, window); sourceSize.height: Style.px(24, window); visible: window.width >= 1100 }
            ConsoleText { text: qsTr("Moonlight"); font.weight: Font.DemiBold; visible: window.width >= 1100; Layout.rightMargin: Style.px(24, window) }
            ConsoleButton {
                id: homeButton; text: qsTr("Home"); textOnly: true; selected: stackView.currentItem instanceof AppView
                onClicked: window.showHome(); KeyNavigation.right: pcsButton
                Keys.onDownPressed: stackView.currentItem.forceActiveFocus()
            }
            ConsoleButton {
                id: pcsButton; text: qsTr("PCs"); textOnly: true; selected: stackView.currentItem instanceof PcView
                onClicked: window.showPcs(); KeyNavigation.left: homeButton; KeyNavigation.right: settingsButton
                Keys.onDownPressed: stackView.currentItem.forceActiveFocus()
            }
            ConsoleButton {
                id: settingsButton; text: qsTr("Settings"); textOnly: true; selected: stackView.currentItem instanceof SettingsView
                onClicked: window.openSettings(); KeyNavigation.left: pcsButton; KeyNavigation.right: uiFullscreenButton
                Keys.onDownPressed: stackView.currentItem.forceActiveFocus()
                Shortcut { sequences: [StandardKey.Preferences]; onActivated: settingsButton.clicked() }
            }
            Item { Layout.fillWidth: true }
            Rectangle { visible: window.selectedHostName.length > 0 && window.width >= 1100; Layout.preferredWidth: Style.px(8, window); Layout.preferredHeight: Style.px(8, window); radius: width/2; color: Style.ok }
            ConsoleText { text: window.selectedHostName; role: "secondary"; elide: Text.ElideRight; Layout.maximumWidth: window.width * 0.2; visible: window.width >= 1100 }
            ConsoleButton {
                id: uiFullscreenButton; objectName: "uiFullscreenButton"
                visible: SystemProperties.hasDesktopEnvironment
                text: window.visibility === Window.FullScreen ? qsTr("Exit fullscreen") : qsTr("Fullscreen")
                textOnly: true
                onClicked: window.toggleUiFullscreen()
                KeyNavigation.left: settingsButton
                Keys.onDownPressed: stackView.currentItem.forceActiveFocus()
                ToolTip.text: qsTr("F11")
                ToolTip.visible: hovered
            }
        }
    }
    footer: Item {
        height: toolBar.visible ? Style.px(56, window) : 0; visible: toolBar.visible
        RowLayout {
            visible: !window.popupHasFocus()
            anchors.fill: parent; anchors.leftMargin: Style.px(64, window); anchors.rightMargin: Style.px(64, window)
            spacing: Style.px(32, window)
            Repeater {
                model: ["confirm", "back", "options", "shelves"]
                delegate: Item {
                    id: hint
                    readonly property string action: modelData
                    objectName: "footerHint_" + action
                    visible: action === "confirm" || action === "back" || !window.toolbarFocused && (action === "options" && ((stackView.currentItem instanceof PcView || stackView.currentItem instanceof SettingsView) && stackView.currentItem.optionsAvailable || stackView.currentItem instanceof AppView && stackView.currentItem.hasGames && !stackView.currentItem.detailsExpanded) || action === "shelves" && stackView.currentItem instanceof AppView && stackView.currentItem.hasMultipleShelves && !stackView.currentItem.detailsExpanded)
                    implicitWidth: hintRow.implicitWidth; implicitHeight: hintRow.implicitHeight
                    Row {
                        id: hintRow; spacing: Style.px(8, window)
                        ConsoleGlyph { anchors.verticalCenter: parent.verticalCenter; key: hint.action === "shelves" ? "↑↓" : window.controllerLabel(hint.action); gamepad: window.controllerInput && hint.action !== "shelves" }
                        ConsoleText {
                            role: "secondary"
                            text: hint.action === "back" ? qsTr("Back") : hint.action === "options" ? stackView.currentItem instanceof AppView ? qsTr("Details") : stackView.currentItem instanceof SettingsView ? qsTr("Help") : qsTr("Options") : hint.action === "shelves" ? qsTr("Rows") :
                                window.toolbarFocused ? qsTr("Select") : stackView.currentItem && stackView.currentItem.primaryVerb && !stackView.currentItem.detailsExpanded ? stackView.currentItem.primaryVerb : qsTr("Select")
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: hint.action !== "shelves"
                        onClicked: {
                            if (hint.action === "confirm") window.activatePrimary()
                            else if (hint.action === "back") { if (stackView.depth > 1) goBack(); else quitConfirmationDialog.open() }
                            else if (hint.action === "options") window.activateOptions()
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
              ConsoleText { role: "caption"; text: qsTr("◀  Categories"); visible: stackView.currentItem instanceof SettingsView && !stackView.currentItem.categoryMenuFocused && !window.toolbarFocused && !window.popupHasFocus() }
            ConsoleText { role: "caption"; text: window.controllerLabel("sections") + "  " + qsTr("Sections"); visible: window.width >= 1100 }
        }
    }
    // Settings > About owns secondary application actions and update notices.
    Shortcut { sequence: "F11"; enabled: toolBar.visible; onActivated: window.toggleUiFullscreen() }
    QtObject {
        id: updateButton
        property string browserUrl: ""
        property string updateVersion: ""
        property bool available: false
        function updateAvailable(version, url) { updateVersion = version; browserUrl = url; available = true }
        Component.onCompleted: { AutoUpdateChecker.onUpdateAvailable.connect(updateAvailable); AutoUpdateChecker.start() }
    }
    Connections { target: AutoUpdateChecker; function onNotifyPrereleasesChanged() { updateButton.available = false; AutoUpdateChecker.start() } }
    Shortcut { sequences: [StandardKey.New]; onActivated: addPcDialog.open() }
    Shortcut { sequences: [StandardKey.HelpContents]; enabled: SystemProperties.hasBrowser; onActivated: Qt.openUrlExternally("https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide") }

    ErrorMessageDialog {
        id: noHwDecoderDialog
        text: qsTr("No functioning hardware accelerated video decoder was detected by Moonlight. " +
                   "Your streaming performance may be severely degraded in this configuration.")
        helpText: qsTr("Click the Help button for more information on solving this problem.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Fixing-Hardware-Decoding-Problems"
    }

    ErrorMessageDialog {
        id: xWaylandDialog
        text: qsTr("Hardware acceleration doesn't work on XWayland. Continuing on XWayland may result in poor streaming performance. " +
                   "Try running with QT_QPA_PLATFORM=wayland or switch to X11.")
        helpText: qsTr("Click the Help button for more information.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Fixing-Hardware-Decoding-Problems"
    }

    NavigableMessageDialog {
        id: wow64Dialog
        standardButtons: Dialog.Ok | Dialog.Cancel
        text: qsTr("This version of Moonlight isn't optimized for your PC. Please download the '%1' version of Moonlight for the best streaming performance.").arg(SystemProperties.friendlyNativeArchName)
        onAccepted: {
            Qt.openUrlExternally("https://github.com/Nonary/moonlight-qt/releases");
        }
    }

    ErrorMessageDialog {
        id: unmappedGamepadDialog
        property string unmappedGamepads : ""
        text: qsTr("Moonlight detected gamepads without a mapping:") + "\n" + unmappedGamepads
        helpTextSeparator: "\n\n"
        helpText: qsTr("Click the Help button for information on how to map your gamepads.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Gamepad-Mapping"
    }

    // This dialog appears when quitting via keyboard or gamepad button
    NavigableMessageDialog {
        id: quitConfirmationDialog
        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Are you sure you want to quit?")
        // For keyboard/gamepad navigation
        initialFocusButton: Dialog.No
        acceptText: qsTr("Quit Moonlight")
        rejectText: qsTr("Keep playing")
        onAccepted: Qt.quit()
    }

    // HACK: This belongs in StreamSegue but keeping a dialog around after the parent
    // dies can trigger bugs in Qt 5.12 that cause the app to crash. For now, we will
    // host this dialog in a QML component that is never destroyed.
    //
    // To repro: Start a stream, cut the network connection to trigger the "Connection
    // terminated" dialog, wait until the app grid times out back to the PC grid, then
    // try to dismiss the dialog.
    ErrorMessageDialog {
        id: streamSegueErrorDialog
        objectName: "streamError"

        property bool quitAfter: false

        onClosed: {
            if (quitAfter) {
                Qt.quit()
            }

            // StreamSegue assumes its dialog will be re-created each time we
            // start streaming, so fake it by wiping out the text each time.
            text = ""
        }
    }

    NavigableDialog {
        id: addPcDialog
        property string label: qsTr("Enter the IP address of your host PC:")

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            SdlGamepadKeyNavigation.setUiNavMode(true)
            editText.forceActiveFocus()
        }

        onClosed: {
            SdlGamepadKeyNavigation.setUiNavMode(false)
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                ComputerManager.addNewHostManually(editText.text.trim())
            }
        }

        ColumnLayout {
            Label {
                text: addPcDialog.label
                font.bold: true
            }

            ConsoleTextField {
                id: editText
                objectName: "hostAddress"
                addressInput: true
                placeholderText: qsTr("PC address")
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    if (controllerEditing) openKeyboard()
                    else addPcDialog.accept()
                }

                Keys.onEnterPressed: {
                    if (controllerEditing) openKeyboard()
                    else addPcDialog.accept()
                }
            }
        }
    }
}
