import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2

import StreamingPreferences 1.0
import ComputerManager 1.0
import ComputerModel 1.0
import SdlGamepadKeyNavigation 1.0
import SystemProperties 1.0
import PyroWaveCalibrator 1.0
import NetworkBuffers 1.0

Flickable {
    id: settingsPage
    ComputerModel {
        id: calibrationHosts
        Component.onCompleted: if (!settingsPage.gameMode) initialize(ComputerManager)
    }
    objectName: qsTr("Settings")
    property var preferences: StreamingPreferences
    property bool gameMode: false
    property bool manageLifecycle: true
    readonly property bool singleColumn: width < 1100

    signal languageChanged()

    boundsBehavior: Flickable.OvershootBounds

    contentWidth: width
    contentHeight: singleColumn ? settingsColumn1.height + settingsColumn2.height : Math.max(settingsColumn1.height, settingsColumn2.height)

    ScrollBar.vertical: ScrollBar {
        anchors {
            left: parent.right
            leftMargin: -10
        }
    }

    function isChildOfFlickable(item) {
        while (item) {
            if (item.parent === contentItem) {
                return true
            }

            item = item.parent
        }
        return false
    }

    NumberAnimation on contentY {
        id: autoScrollAnimation
        duration: 100
    }

    Window.onActiveFocusItemChanged: {
        var item = Window.activeFocusItem
        if (item) {
            // Ignore non-child elements like the toolbar buttons
            if (!isChildOfFlickable(item)) {
                return
            }

            // Map the focus item's position into our content item's coordinate space
            var pos = item.mapToItem(contentItem, 0, 0)

            // Ensure some extra space is visible around the element we're scrolling to
            var scrollMargin = height > 100 ? 50 : 0

            if (pos.y - scrollMargin < contentY) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.max(pos.y - scrollMargin, 0)
                autoScrollAnimation.start()
            }
            else if (pos.y + item.height + scrollMargin > contentY + height) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.min(pos.y + item.height + scrollMargin - height, contentHeight - height)
                autoScrollAnimation.start()
            }
        }
    }

    function activate() {
        // This enables Tab and BackTab based navigation rather than arrow keys.
        // It is required to shift focus between controls on the settings page.
        SdlGamepadKeyNavigation.setUiNavMode(true)

        // Highlight the first item if a gamepad is connected
        if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            resolutionComboBox.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    function deactivate() {
        SdlGamepadKeyNavigation.setUiNavMode(false)

        // Save the prefs so the Session can observe the changes
        if (!gameMode) preferences.save()
    }

    StackView.onActivated: if (manageLifecycle) activate()
    StackView.onDeactivating: if (manageLifecycle) deactivate()

    Component.onDestruction: {
        // Also save preferences on destruction, since we won't get a
        // deactivating callback if the user just closes Moonlight
        if (!gameMode) preferences.save()
    }

    Column {
        padding: 10
        id: settingsColumn1
        width: settingsPage.singleColumn ? settingsPage.width : settingsPage.width / 2
        spacing: 15

        GroupBox {
            id: basicSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Basic Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resFPStitle
                    text: qsTr("Resolution and FPS")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: resFPSdesc
                    text: qsTr("Setting values too high for your PC or network connection may cause lag, stuttering, or errors.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Row {
                    spacing: 5
                    width: parent.width

                    AutoResizingComboBox {
                        property int lastIndexValue

                        function addDetectedResolution(friendlyNamePrefix, rect) {
                            var indexToAdd = 0
                            for (var j = 0; j < resolutionComboBox.count; j++) {
                                var existing_width = parseInt(resolutionListModel.get(j).video_width);
                                var existing_height = parseInt(resolutionListModel.get(j).video_height);

                                if (rect.width === existing_width && rect.height === existing_height) {
                                    // Duplicate entry, skip
                                    indexToAdd = -1
                                    break
                                }
                                else if (rect.width * rect.height > existing_width * existing_height) {
                                    // Candidate entrypoint after this entry
                                    indexToAdd = j + 1
                                }
                            }

                            // Insert this display's resolution if it's not a duplicate
                            if (indexToAdd >= 0) {
                                resolutionListModel.insert(indexToAdd,
                                                           {
                                                               "text": friendlyNamePrefix+" ("+rect.width+"x"+rect.height+")",
                                                               "video_width": ""+rect.width,
                                                               "video_height": ""+rect.height,
                                                               "is_custom": false
                                                           })
                            }
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            // Refresh display data before using it to build the list
                            SystemProperties.refreshDisplays()

                            // Add native and safe area resolutions for all attached displays
                            var done = false
                            for (var displayIndex = 0; !done; displayIndex++) {
                                var screenRect = SystemProperties.getNativeResolution(displayIndex);
                                var safeAreaRect = SystemProperties.getSafeAreaResolution(displayIndex);

                                if (screenRect.width === 0) {
                                    // Exceeded max count of displays
                                    done = true
                                    break
                                }

                                addDetectedResolution(qsTr("Native"), screenRect)
                                addDetectedResolution(qsTr("Native (Excluding Notch)"), safeAreaRect)
                            }

                            // Prune resolutions that are over the decoder's maximum
                            var max_pixels = SystemProperties.maximumResolution.width * SystemProperties.maximumResolution.height;
                            if (max_pixels > 0) {
                                for (var j = 0; j < resolutionComboBox.count; j++) {
                                    var existing_width = parseInt(resolutionListModel.get(j).video_width);
                                    var existing_height = parseInt(resolutionListModel.get(j).video_height);

                                    if (existing_width * existing_height > max_pixels) {
                                        resolutionListModel.remove(j)
                                        j--
                                    }
                                }
                            }

                            // load the saved width/height, and iterate through the ComboBox until a match is found
                            // and set it to that index.
                            var saved_width = preferences.width
                            var saved_height = preferences.height
                            var index_set = false
                            for (var i = 0; i < resolutionListModel.count; i++) {
                                var el_width = parseInt(resolutionListModel.get(i).video_width);
                                var el_height = parseInt(resolutionListModel.get(i).video_height);

                                if (saved_width === el_width && saved_height === el_height) {
                                    currentIndex = i
                                    index_set = true
                                    break
                                }
                            }

                            if (!index_set) {
                                // We did not find a match. This must be a custom resolution.
                                resolutionListModel.append({
                                                               "text": qsTr("Custom")+" ("+preferences.width+"x"+preferences.height+")",
                                                               "video_width": ""+preferences.width,
                                                               "video_height": ""+preferences.height,
                                                               "is_custom": true
                                                           })
                                currentIndex = resolutionListModel.count - 1
                            }
                            else {
                                resolutionListModel.append({
                                                               "text": qsTr("Custom"),
                                                               "video_width": "",
                                                               "video_height": "",
                                                               "is_custom": true
                                                           })
                            }

                            // Since we don't call activate() here, we need to trigger
                            // width calculation manually
                            recalculateWidth()

                            lastIndexValue = currentIndex
                        }

                        id: resolutionComboBox
                        maximumWidth: parent.width / 2
                        textRole: "text"
                        model: ListModel {
                            id: resolutionListModel
                            // Other elements may be added at runtime
                            // based on attached display resolution
                            ListElement {
                                text: qsTr("720p")
                                video_width: "1280"
                                video_height: "720"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("1080p")
                                video_width: "1920"
                                video_height: "1080"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("1440p")
                                video_width: "2560"
                                video_height: "1440"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("4K")
                                video_width: "3840"
                                video_height: "2160"
                                is_custom: false
                            }
                        }

                        function updateBitrateForSelection() {
                            var selectedWidth = parseInt(resolutionListModel.get(currentIndex).video_width)
                            var selectedHeight = parseInt(resolutionListModel.get(currentIndex).video_height)

                            // Only modify the bitrate if the values actually changed
                            if (preferences.width !== selectedWidth || preferences.height !== selectedHeight) {
                                preferences.width = selectedWidth
                                preferences.height = selectedHeight

                                if (preferences.autoAdjustBitrate) {
                                    preferences.bitrateKbps = slider.defaultBitrate();
                                    slider.value = preferences.bitrateKbps
                                }
                            }

                            lastIndexValue = currentIndex
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated : {
                            if (resolutionListModel.get(currentIndex).is_custom) {
                                customResolutionDialog.open()
                            }
                            else {
                                updateBitrateForSelection()
                            }
                        }

                        NavigableDialog {
                            id: customResolutionDialog
                            standardButtons: Dialog.Ok | Dialog.Cancel
                            onOpened: {
                                // Force keyboard focus on the textbox so keyboard navigation works
                                widthField.forceActiveFocus()

                                // standardButton() was added in Qt 5.10, so we must check for it first
                                if (customResolutionDialog.standardButton) {
                                    customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                }
                            }

                            onClosed: {
                                widthField.clear()
                                heightField.clear()
                            }

                            onRejected: {
                                resolutionComboBox.currentIndex = resolutionComboBox.lastIndexValue
                            }

                            function isInputValid() {
                                // If we have text in either textbox that isn't valid,
                                // reject the input.
                                if ((!widthField.acceptableInput && widthField.text) ||
                                        (!heightField.acceptableInput && heightField.text)) {
                                    return false
                                }

                                // The textboxes need to have text or placeholder text
                                if ((!widthField.text && !widthField.placeholderText) ||
                                        (!heightField.text && !heightField.placeholderText)) {
                                    return false
                                }

                                return true
                            }

                            onAccepted: {
                                // Reject if there's invalid input
                                if (!isInputValid()) {
                                    reject()
                                    return
                                }

                                var width = widthField.text ? widthField.text : widthField.placeholderText
                                var height = heightField.text ? heightField.text : heightField.placeholderText

                                // Find and update the custom entry
                                for (var i = 0; i < resolutionListModel.count; i++) {
                                    if (resolutionListModel.get(i).is_custom) {
                                        resolutionListModel.setProperty(i, "video_width", width)
                                        resolutionListModel.setProperty(i, "video_height", height)
                                        resolutionListModel.setProperty(i, "text", "Custom ("+width+"x"+height+")")

                                        // Now update the bitrate using the custom resolution
                                        resolutionComboBox.currentIndex = i
                                        resolutionComboBox.updateBitrateForSelection()

                                        // Update the combobox width too
                                        resolutionComboBox.recalculateWidth()
                                        break
                                    }
                                }
                            }

                            ColumnLayout {
                                Label {
                                    text: qsTr("Custom resolutions are not officially supported by GeForce Experience, so it will not set your host display resolution. You will need to set it manually while in game.") + "\n\n" +
                                          qsTr("Resolutions that are not supported by your client or host PC may cause streaming errors.") + "\n"
                                    wrapMode: Label.WordWrap
                                    Layout.maximumWidth: 300
                                }

                                Label {
                                    text: qsTr("Enter a custom resolution:")
                                    font.bold: true
                                }

                                RowLayout {
                                    TextField {
                                        id: widthField
                                        maximumLength: 5
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_width
                                        validator: IntValidator{bottom:256; top:8192}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customResolutionDialog.standardButton) {
                                                customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customResolutionDialog.accept()
                                        }
                                    }

                                    Label {
                                        text: "x"
                                        font.bold: true
                                    }

                                    TextField {
                                        id: heightField
                                        maximumLength: 5
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_height
                                        validator: IntValidator{bottom:256; top:8192}

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customResolutionDialog.standardButton) {
                                                customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customResolutionDialog.accept()
                                        }
                                    }
                                }
                            }
                        }
                    }

                    AutoResizingComboBox {
                        property int lastIndexValue

                        function updateBitrateForSelection() {
                            var selectedFps = parseInt(model.get(fpsComboBox.currentIndex).video_fps)
                            var fpsChanged = preferences.fps !== selectedFps
                            preferences.fps = selectedFps

                            if (fpsChanged && preferences.autoAdjustBitrate) {
                                preferences.bitrateKbps = slider.defaultBitrate();
                                slider.value = preferences.bitrateKbps
                            }

                            lastIndexValue = currentIndex
                        }

                        NavigableDialog {
                            function isInputValid() {
                                // If we have text that isn't valid, reject the input.
                                if (!fpsField.acceptableInput && fpsField.text) {
                                    return false
                                }

                                // The textbox needs to have text or placeholder text
                                if (!fpsField.text && !fpsField.placeholderText) {
                                    return false
                                }

                                return true
                            }

                            id: customFpsDialog
                            standardButtons: Dialog.Ok | Dialog.Cancel
                            onOpened: {
                                // Force keyboard focus on the textbox so keyboard navigation works
                                fpsField.forceActiveFocus()

                                // standardButton() was added in Qt 5.10, so we must check for it first
                                if (customFpsDialog.standardButton) {
                                    customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                }
                            }

                            onClosed: {
                                fpsField.clear()
                            }

                            onRejected: {
                                fpsComboBox.currentIndex = fpsComboBox.lastIndexValue
                            }

                            onAccepted: {
                                // Reject if there's invalid input
                                if (!isInputValid()) {
                                    reject()
                                    return
                                }

                                var fps = fpsField.text ? fpsField.text : fpsField.placeholderText

                                // Find and update the custom entry
                                for (var i = 0; i < fpsListModel.count; i++) {
                                    if (fpsListModel.get(i).is_custom) {
                                        fpsListModel.setProperty(i, "video_fps", fps)
                                        fpsListModel.setProperty(i, "text", qsTr("Custom (%1 FPS)").arg(fps))

                                        // Now update the bitrate using the custom resolution
                                        fpsComboBox.currentIndex = i
                                        fpsComboBox.updateBitrateForSelection()

                                        // Update the combobox width too
                                        fpsComboBox.recalculateWidth()
                                        break
                                    }
                                }
                            }

                            ColumnLayout {
                                Label {
                                    text: qsTr("Enter a custom frame rate:")
                                    font.bold: true
                                }

                                RowLayout {
                                    TextField {
                                        id: fpsField
                                        maximumLength: 4
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: {
                                            var entry = fpsComboBox.currentIndex >= 0 && fpsComboBox.currentIndex < fpsListModel.count
                                                ? fpsListModel.get(fpsComboBox.currentIndex) : null
                                            return entry ? entry.video_fps : preferences.fps
                                        }
                                        validator: IntValidator{bottom:10; top:9999}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customFpsDialog.standardButton) {
                                                customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customFpsDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customFpsDialog.accept()
                                        }
                                    }
                                }
                            }
                        }

                        function getRefreshRates() {
                            var refreshRates = []
                            for (var displayIndex = 0; ; displayIndex++) {
                                var refreshRate = SystemProperties.getRefreshRate(displayIndex)
                                if (refreshRate === 0) {
                                    break
                                }

                                refreshRates.push(refreshRate)
                            }

                            return refreshRates
                        }

                        function choiceText(choice) {
                            switch (choice.kind) {
                            case "vrr":
                                return qsTr("VRR (%1 FPS)").arg(choice.video_fps)
                            case "low-latency-vrr":
                                return qsTr("Low-latency VRR (%1 FPS)").arg(choice.video_fps)
                            case "custom":
                                return qsTr("Custom (%1 FPS)").arg(choice.video_fps)
                            default:
                                return qsTr("%1 FPS").arg(choice.video_fps)
                            }
                        }

                        function reinitialize() {
                            var choices = preferences.getFpsChoices(getRefreshRates())
                            model.clear()
                            var hasCustomChoice = false

                            for (var i = 0; i < choices.length; i++) {
                                var choice = choices[i]
                                hasCustomChoice = hasCustomChoice || choice.is_custom
                                model.append({
                                                 "text": choiceText(choice),
                                                 "video_fps": choice.video_fps,
                                                 "is_custom": choice.is_custom
                                             })
                            }

                            var saved_fps = preferences.fps
                            var found = false
                            for (var i = 0; i < model.count; i++) {
                                var el_fps = parseInt(model.get(i).video_fps);

                                // Look for a matching frame rate
                                if (saved_fps === el_fps) {
                                    currentIndex = i
                                    found = true
                                    break
                                }
                            }

                            // Saved custom and native maximum choices remain visible.
                            if (!found) {
                                currentIndex = model.count > 0 ? 0 : -1
                            }

                            if (!hasCustomChoice) {
                                model.append({
                                                 "text": qsTr("Custom"),
                                                 "video_fps": "",
                                                 "is_custom": true
                                             })
                            }

                            recalculateWidth()

                            lastIndexValue = currentIndex
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            reinitialize()
                            languageChanged.connect(reinitialize)
                            preferences.enableVsyncChanged.connect(reinitialize)
                            preferences.enableVrrChanged.connect(reinitialize)
                        }

                        model: ListModel {
                            id: fpsListModel
                        }

                        id: fpsComboBox

                        objectName: "fpsComboBox"
                        maximumWidth: parent.width / 2
                        textRole: "text"
                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated : {
                            if (model.get(currentIndex).is_custom) {
                                customFpsDialog.open()
                            }
                            else {
                                updateBitrateForSelection()
                            }
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: resVCCTitle
                    text: qsTr("Video codec")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        if (SystemProperties.hasPyroWave) {
                            codecListModel.append({
                                "text": qsTr("PyroWave (wired LAN, experimental)"),
                                "val": StreamingPreferences.VCC_FORCE_PYROWAVE
                            })
                        }

                        var saved_vcc = preferences.videoCodecConfig

                        // Default to Automatic (relevant if HDR is enabled,
                        // where we will match none of the codecs in the list)
                        currentIndex = 0

                        for(var i = 0; i < codecListModel.count; i++) {
                            var el_vcc = codecListModel.get(i).val;
                            if (saved_vcc === el_vcc) {
                                currentIndex = i
                                break
                            }
                        }

                        recalculateWidth()
                    }

                    id: codecComboBox
                    textRole: "text"
                    model: ListModel {
                        id: codecListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.VCC_AUTO
                        }
                        ListElement {
                            text: qsTr("H.264")
                            val: StreamingPreferences.VCC_FORCE_H264
                        }
                        ListElement {
                            text: qsTr("HEVC (H.265)")
                            val: StreamingPreferences.VCC_FORCE_HEVC
                        }
                        ListElement {
                            text: qsTr("AV1")
                            val: StreamingPreferences.VCC_FORCE_AV1
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        if (enabled) {
                            var wasPyroWave = slider.pyroWave
                            preferences.videoCodecConfig = codecListModel.get(currentIndex).val

                            // PyroWave's useful bitrates are an order of magnitude above
                            // the other codecs', so switching in or out resets a default.
                            if (slider.pyroWave !== wasPyroWave && preferences.autoAdjustBitrate) {
                                preferences.bitrateKbps = slider.defaultBitrate()
                                slider.value = preferences.bitrateKbps
                            }
                            else if (preferences.bitrateKbps > slider.to) {
                                preferences.bitrateKbps = slider.to
                                slider.value = preferences.bitrateKbps
                            }
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 8000
                    ToolTip.visible: hovered && slider.pyroWave
                    ToolTip.text: qsTr("PyroWave is an intra-only GPU wavelet codec. It needs a wired connection with hundreds of Mbps to spare and a host with PyroWave support; other hosts fall back to H.264. On Linux, GPU readback and upload may limit frame rate.")
                }

                CheckBox {
                    id: enableYUV444
                    width: parent.width
                    text: qsTr("Enable YUV 4:4:4")
                    font.pointSize: 12

                    checked: preferences.enableYUV444
                    onToggled: {
                        // This is called on init, so only reset to default bitrate when checked state changes.
                        if (preferences.enableYUV444 != checked) {
                            preferences.enableYUV444 = checked
                            if (preferences.autoAdjustBitrate) {
                                preferences.bitrateKbps = slider.defaultBitrate();
                                slider.value = preferences.bitrateKbps
                            }
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: enabled ?
                                      qsTr("Good for streaming desktop and text-heavy games, but not recommended for fast-paced games.")
                                    :
                                      qsTr("YUV 4:4:4 is not supported on this PC.")
                }

                Label {
                    width: parent.width
                    id: bitrateTitle
                    text: qsTr("Video bitrate:")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: bitrateDesc
                    text: qsTr("Lower the bitrate on slower connections. Raise the bitrate to increase image quality.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Row {
                    width: parent.width
                    spacing: 5

                    Slider {
                        id: slider
                        objectName: "videoBitrateSlider"

                        readonly property bool pyroWave: preferences.videoCodecConfig === StreamingPreferences.VCC_FORCE_PYROWAVE

                        // PyroWave needs several hundred Mbps, up to multi-gigabit LANs
                        function defaultBitrate() {
                            return preferences.getEffectiveDefaultBitrate()
                        }

                        value: preferences.bitrateKbps

                        stepSize: pyroWave ? 5000 : 500
                        from : pyroWave ? 5000 : 500
                        to: pyroWave ? 3000000 : (preferences.unlockBitrate ? 500000 : 150000)

                        snapMode: "SnapOnRelease"
                        width: Math.min(bitrateDesc.implicitWidth, parent.width - (resetBitrateButton.visible ? resetBitrateButton.width + parent.spacing : 0))

                        onValueChanged: {
                            bitrateTitle.text = qsTr("Video bitrate: %1 Mbps").arg(value / 1000.0)
                        }

                        onMoved: {
                            preferences.bitrateKbps = value
                            preferences.autoAdjustBitrate = false
                        }

                        Component.onCompleted: {
                            // Refresh the text after translations change
                            languageChanged.connect(valueChanged)
                        }
                    }

                    Button {
                        id: resetBitrateButton
                        text: qsTr("Use Default (%1 Mbps)").arg(slider.defaultBitrate() / 1000.0)
                        visible: !preferences.autoAdjustBitrate || preferences.bitrateKbps !== slider.defaultBitrate()
                        onClicked: {
                            var defaultBitrate = slider.defaultBitrate()
                            preferences.bitrateKbps = defaultBitrate
                            preferences.autoAdjustBitrate = true
                            slider.value = defaultBitrate
                        }
                    }
                }

                Column {
                    width: parent.width
                    spacing: 5
                    visible: !gameMode && slider.pyroWave && (Qt.platform.os === "linux" || Qt.platform.os === "windows")

                    Component.onCompleted: if (!gameMode) NetworkBuffers.refresh()

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: NetworkBuffers.needsFix
                        color: "#ffb74d"
                        text: NetworkBuffers.problemText
                    }

                    Row {
                        spacing: 8
                        visible: NetworkBuffers.needsFix

                        Button {
                            text: qsTr("Fix it")
                            visible: NetworkBuffers.canApply
                            enabled: !NetworkBuffers.busy
                            onClicked: NetworkBuffers.apply()

                            ToolTip.delay: 1000
                            ToolTip.timeout: 10000
                            ToolTip.visible: hovered
                            ToolTip.text: NetworkBuffers.fixDescription
                        }

                        Button {
                            text: qsTr("Copy command")
                            onClicked: NetworkBuffers.copyCommand()
                        }
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: NetworkBuffers.needsFix && !NetworkBuffers.canApply
                        font.pointSize: 9
                        text: NetworkBuffers.manualHint + "\n" + NetworkBuffers.manualCommand
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: NetworkBuffers.message !== ""
                        text: NetworkBuffers.message
                    }
                }

                Column {
                    width: parent.width
                    spacing: 5
                    visible: !gameMode && SystemProperties.hasPyroWave && slider.pyroWave

                    ComboBox {
                        id: calibrationHost
                        width: parent.width
                        model: calibrationHosts
                        textRole: "name"
                        enabled: !PyroWaveCalibrator.running
                    }

                    Button {
                        text: qsTr("Calibrate PyroWave")
                        enabled: !PyroWaveCalibrator.running && calibrationHost.currentIndex >= 0
                        onClicked: {
                            calibrationDialog.testFps = preferences.fps
                            calibrationDialog.open()
                            // Each test frame is drawn at this screen's size, as a stream would be
                            PyroWaveCalibrator.start(ComputerManager,
                                                     calibrationHosts.uuidAt(calibrationHost.currentIndex),
                                                     calibrationDialog.testFps,
                                                     Math.round(Screen.width * Screen.devicePixelRatio),
                                                     Math.round(Screen.height * Screen.devicePixelRatio))
                        }
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Tests bandwidth from the selected host to this PC, then finds a bitrate this device can decode and draw smoothly at your frame rate. Takes about two minutes. Click a result to use it.")
                    }
                }

                NavigableDialog {
                    id: calibrationDialog
                    property int testFps: 60
                    title: qsTr("PyroWave calibration — %1 FPS").arg(testFps)
                    width: Math.min(settingsPage.width - 24, 860)
                    height: Math.min(settingsPage.height - 24, 640)
                    standardButtons: Dialog.Close

                    // Finished results stay; the format being tested is abandoned
                    onAboutToHide: PyroWaveCalibrator.cancel()

                    readonly property var rows: (Qt.platform.os === "linux" ? [
                        { name: "4K", width: 3840, height: 2160 },
                        { name: "1440p", width: 2560, height: 1440 },
                        { name: "1080p", width: 1920, height: 1080 },
                        { name: "800p", width: 1280, height: 800 },
                        { name: "720p", width: 1280, height: 720 }
                    ] : [
                        { name: "4K", width: 3840, height: 2160 },
                        { name: "1440p", width: 2560, height: 1440 },
                        { name: "1080p", width: 1920, height: 1080 },
                        { name: "720p", width: 1280, height: 720 }
                    ])
                    readonly property var samples: PyroWaveCalibrator.results
                    readonly property var tierColors: ({
                        "any": "#66bb6a",
                        "vrr": "#ffca28",
                        "vrrLarge": "#ff9800",
                        "slow": "#ef5350",
                        "error": "#9e9e9e"
                    })

                    // Results arrive in sweep order: per resolution, 4:4:4 HDR,
                    // 4:4:4 SDR, 4:2:0 HDR, 4:2:0 SDR
                    function sample(row, mode) {
                        var offset = row * 4 + mode
                        return offset < samples.length ? samples[offset] : null
                    }

                    function hdrUnavailable(option) {
                        return option && option.hdr && !SystemProperties.supportsHdr
                    }

                    function canApply(option) {
                        return option && option.valid && !hdrUnavailable(option)
                    }

                    function headline(option, hdr) {
                        var format = hdr ? qsTr("HDR (10-bit)") : qsTr("SDR (8-bit)")
                        if (option && option.valid) return format + " · " + qsTr("%1 Mbps").arg(option.bitrateKbps / 1000)
                        return format
                    }

                    function tierText(option) {
                        if (!option) return PyroWaveCalibrator.running ? qsTr("Testing…") : "—"
                        if (option.tier === "error") return option.error
                        if (option.tier === "slow") return qsTr("Can't keep up")
                        if (hdrUnavailable(option)) return qsTr("No HDR display")
                        var text = option.tier === "any" ? qsTr("Any display") :
                                   option.tier === "vrr" ? qsTr("Needs VRR") : qsTr("Needs VRR · Smooth mode")
                        if (option.quality === "reduced") text += " · " + qsTr("Reduced quality")
                        else if (option.quality === "low") text += " · " + qsTr("Low quality")
                        return text
                    }

                    function tierColor(option) {
                        if (!option || hdrUnavailable(option)) return "#9e9e9e"
                        return tierColors[option.tier]
                    }

                    function frameCost(option) {
                        return qsTr("99% of frames take up to %1 ms to decode and draw, %2% of each frame at %3 FPS.")
                                .arg(option.frameMs.toFixed(1)).arg(option.loadPercent).arg(testFps)
                    }

                    function optionDetail(option) {
                        if (!option || !option.valid) return ""
                        if (!option.keepsUp) {
                            return frameCost(option) + " " +
                                    qsTr("A lower bitrate doesn't make this device fast enough, so the stream will stutter or fall behind. You can still use it.")
                        }
                        var details = [frameCost(option)]
                        if (option.tier === "vrr") {
                            details.push(qsTr("With VRR the occasional slow frame is shown slightly late; on a fixed-refresh display it would stutter."))
                        }
                        else if (option.tier === "vrrLarge") {
                            details.push(qsTr("Slow frames use nearly the whole frame, so only the Smooth VRR latency mode's larger buffer hides them; other modes and fixed-refresh displays would stutter."))
                        }
                        details.push(qsTr("%1 Mbps reaches %2 dB on the codec author's quality scale; he recommends %3 Mbps (35 dB) for this format.")
                                     .arg(option.bitrateKbps / 1000).arg(option.qualityDb.toFixed(1))
                                     .arg(option.guideKbps / 1000))
                        if (option.deviceLimited) details.push(qsTr("The bitrate was lowered so this device keeps up."))
                        else if (option.linkLimited) details.push(qsTr("The bitrate is capped by this device's network link."))
                        return details.join(" ")
                    }

                    function applyChoice(option) {
                        if (gameMode || !canApply(option)) return
                        preferences.videoCodecConfig = StreamingPreferences.VCC_FORCE_PYROWAVE
                        for (var codecIndex = 0; codecIndex < codecListModel.count; codecIndex++) {
                            if (codecListModel.get(codecIndex).val === StreamingPreferences.VCC_FORCE_PYROWAVE) {
                                codecComboBox.currentIndex = codecIndex
                                break
                            }
                        }
                        preferences.width = option.width
                        preferences.height = option.height
                        preferences.enableYUV444 = option.chroma444
                        preferences.enableHdr = option.hdr
                        preferences.bitrateKbps = option.bitrateKbps
                        preferences.autoAdjustBitrate = false
                        slider.value = preferences.bitrateKbps

                        var found = false
                        for (var i = 0; i < resolutionListModel.count; i++) {
                            var entry = resolutionListModel.get(i)
                            if (parseInt(entry.video_width) === option.width &&
                                    parseInt(entry.video_height) === option.height) {
                                resolutionComboBox.currentIndex = i
                                resolutionComboBox.lastIndexValue = i
                                found = true
                                break
                            }
                        }
                        if (!found) {
                            resolutionListModel.append({
                                                           "text": option.height + "p",
                                                           "video_width": "" + option.width,
                                                           "video_height": "" + option.height,
                                                           "is_custom": false
                                                       })
                            resolutionComboBox.currentIndex = resolutionListModel.count - 1
                            resolutionComboBox.lastIndexValue = resolutionComboBox.currentIndex
                        }
                        preferences.save()
                        close()
                    }

                    contentItem: Flickable {
                        clip: true
                        contentWidth: width
                        contentHeight: calibrationTable.height

                        Column {
                            id: calibrationTable
                            width: parent.width
                            spacing: 4

                            Label {
                                width: parent.width
                                wrapMode: Text.Wrap
                                text: PyroWaveCalibrator.message
                            }

                            Label {
                                width: parent.width
                                wrapMode: Text.Wrap
                                font.pointSize: 9
                                text: PyroWaveCalibrator.linkSummary + " " +
                                      qsTr("The bandwidth test is a bulk transfer; a live stream can still encounter packet loss or congestion. Clicking a format applies the bitrate shown.")
                            }

                            Repeater {
                                model: [
                                    { tier: "any", text: qsTr("Any display: 99% of frames use at most 60% of each frame, leaving room for a live stream's extra work with or without VRR.") },
                                    { tier: "vrr", text: qsTr("Needs VRR: slow frames use up to 80% of each frame. VRR hides the occasional late one; a fixed-refresh display may stutter.") },
                                    { tier: "vrrLarge", text: qsTr("Needs VRR · Smooth mode: slow frames use nearly the whole frame. Only the Smooth VRR latency mode's larger buffer hides them.") },
                                    { tier: "slow", text: qsTr("Can't keep up: at %1 FPS, more than 1 frame in 100 takes longer than a frame to decode and draw. Expect stutter; it can still be selected.").arg(calibrationDialog.testFps) }
                                ]
                                delegate: Row {
                                    spacing: 6
                                    Rectangle {
                                        width: 10
                                        height: 10
                                        radius: 5
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: calibrationDialog.tierColors[modelData.tier]
                                    }
                                    Label {
                                        width: calibrationTable.width - 16
                                        wrapMode: Text.Wrap
                                        font.pointSize: 9
                                        text: modelData.text
                                    }
                                }
                            }

                            Label {
                                width: parent.width
                                wrapMode: Text.Wrap
                                font.pointSize: 9
                                text: qsTr("Every format is tested at the codec author's recommended bitrate. \"Reduced quality\" or \"Low quality\" means the bitrate had to be lowered for this device to keep up.")
                            }

                            Row {
                                spacing: 6
                                Label { width: 72; text: qsTr("Resolution") }
                                Label { width: (calibrationTable.width - 84) / 2; text: "4:4:4"; horizontalAlignment: Text.AlignHCenter }
                                Label { width: (calibrationTable.width - 84) / 2; text: "4:2:0"; horizontalAlignment: Text.AlignHCenter }
                            }

                            Repeater {
                                model: calibrationDialog.rows
                                delegate: Row {
                                    id: calibrationRow
                                    width: calibrationTable.width
                                    spacing: 6
                                    readonly property int rowIndex: index
                                    readonly property var rowData: modelData

                                    Label {
                                        width: 72
                                        text: calibrationRow.rowData.name
                                        anchors.verticalCenter: parent.verticalCenter
                                    }

                                    Repeater {
                                        // 4:4:4 then 4:2:0, each HDR then SDR
                                        model: [[0, 1], [2, 3]]
                                        delegate: Column {
                                            id: chromaColumn
                                            readonly property var modes: modelData
                                            width: (calibrationTable.width - 84) / 2
                                            spacing: 2

                                            Repeater {
                                                model: chromaColumn.modes
                                                delegate: Button {
                                                    id: optionButton
                                                    width: parent.width
                                                    height: 54
                                                    readonly property var option: calibrationDialog.sample(calibrationRow.rowIndex, modelData)
                                                    readonly property bool hdr: modelData % 2 === 0
                                                    enabled: calibrationDialog.canApply(option)
                                                    ToolTip.delay: 400
                                                    ToolTip.visible: hovered && ToolTip.text !== ""
                                                    ToolTip.text: calibrationDialog.optionDetail(option)
                                                    onClicked: calibrationDialog.applyChoice(option)

                                                    contentItem: Column {
                                                        spacing: 2
                                                        opacity: optionButton.enabled ? 1.0 : 0.6

                                                        Label {
                                                            width: parent.width
                                                            horizontalAlignment: Text.AlignHCenter
                                                            elide: Text.ElideRight
                                                            font.pointSize: 10
                                                            text: calibrationDialog.headline(optionButton.option, optionButton.hdr)
                                                        }

                                                        Row {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            spacing: 6

                                                            Rectangle {
                                                                width: 10
                                                                height: 10
                                                                radius: 5
                                                                anchors.verticalCenter: parent.verticalCenter
                                                                visible: !!optionButton.option
                                                                color: calibrationDialog.tierColor(optionButton.option)
                                                            }

                                                            Label {
                                                                font.pointSize: 9
                                                                text: calibrationDialog.tierText(optionButton.option)

                                                                // Untested cells keep the style's text color
                                                                Binding on color {
                                                                    when: !!optionButton.option
                                                                    value: calibrationDialog.tierColor(optionButton.option)
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: windowModeTitle
                    text: qsTr("Display mode")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    function createModel() {
                        var model = Qt.createQmlObject('import QtQuick 2.0; ListModel {}', parent, '')

                        model.append({
                                         text: qsTr("Fullscreen"),
                                         val: StreamingPreferences.WM_FULLSCREEN
                                     })

                        model.append({
                                         text: qsTr("Borderless windowed"),
                                         val: StreamingPreferences.WM_FULLSCREEN_DESKTOP
                                     })

                        model.append({
                                         text: qsTr("Windowed"),
                                         val: StreamingPreferences.WM_WINDOWED
                                     })


                        // Set the recommended option based on the OS
                        for (var i = 0; i < model.count; i++) {
                            var thisWm = model.get(i).val;
                            if (thisWm === preferences.recommendedFullScreenMode) {
                                model.get(i).text += " " + qsTr("(Recommended)")
                                model.move(i, 0, 1)
                                break
                            }
                        }

                        return model
                    }


                    // This is used on initialization and upon retranslation
                    function reinitialize() {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        model = createModel()
                        currentIndex = 0

                        // VRR sessions use borderless presentation, but the
                        // saved window-mode preference is never overwritten.
                        var savedWm = vrrForced ?
                                          StreamingPreferences.WM_FULLSCREEN_DESKTOP :
                                          preferences.windowMode
                        for (var i = 0; i < model.count; i++) {
                             var thisWm = model.get(i).val;
                             if (savedWm === thisWm) {
                                 currentIndex = i
                                 break
                             }
                        }

                        // Refresh the disabled control without changing the saved mode.
                        recalculateWidth()
                    }

                    Component.onCompleted: {
                        reinitialize()
                        languageChanged.connect(reinitialize)
                    }

                    id: windowModeComboBox
                    property bool vrrForced: preferences.enableVsync && preferences.enableVrr
                    onVrrForcedChanged: reinitialize()
                    visible: SystemProperties.hasDesktopEnvironment
                    enabled: !SystemProperties.rendererAlwaysFullScreen && !vrrForced
                    hoverEnabled: true
                    textRole: "text"
                    onActivated: {
                        preferences.windowMode = model.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: vrrForced ?
                                      qsTr("Borderless windowed mode is required for active VRR streaming. Your saved display mode will be restored for non-VRR sessions.")
                                    :
                                      qsTr("Fullscreen generally provides the best performance, but borderless windowed may work better with features like macOS Spaces, Alt+Tab, screenshot tools, on-screen overlays, etc.")
                }

                Row {
                    spacing: 5
                    width: parent.width

                    CheckBox {
                        id: vsyncCheck
                        objectName: "vsyncCheck"
                        hoverEnabled: true
                        text: qsTr("V-Sync")
                        font.pointSize:  12
                        checked: preferences.enableVsync
                        onToggled: {
                            preferences.enableVsync = checked
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Disabling V-Sync allows sub-frame rendering latency, but it can display visible tearing")
                    }

                    CheckBox {
                        id: framePacingCheck
                        hoverEnabled: true
                        text: qsTr("Frame pacing")
                        font.pointSize:  12
                        enabled: preferences.enableVsync
                        checked: preferences.enableVsync && preferences.framePacing
                        onToggled: {
                            preferences.framePacing = checked
                        }
                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Frame pacing reduces micro-stutter by delaying frames that come in too early")
                    }

                    CheckBox {
                        hoverEnabled: true
                        id: vrrCheck
                        objectName: "vrrCheck"
                        text: qsTr("VRR")
                        font.pointSize: 12
                        enabled: preferences.enableVsync
                        checked: preferences.enableVrr
                        onToggled: {
                            preferences.enableVrr = checked
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: enabled ?
                                          qsTr("VRR uses adaptive presentation in borderless fullscreen. Choose your display's full refresh rate, or a lower VRR option for more headroom or lower latency.")
                                        :
                                          qsTr("VRR requires V-Sync. Enable V-Sync to change this setting.")
                    }
                }

                Column {
                    width: parent.width
                    spacing: 5
                    visible: preferences.enableVrr
                    enabled: preferences.enableVsync && preferences.enableVrr

                    Label {
                        width: parent.width
                        text: qsTr("VRR timing")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: vrrLatencyModeComboBox
                        objectName: "vrrLatencyModeComboBox"
                        textRole: "text"
                        model: ListModel {
                            id: vrrLatencyModeListModel
                            ListElement {
                                text: qsTr("Low Latency")
                                val: StreamingPreferences.VLM_LOW_LATENCY
                            }
                            ListElement {
                                text: qsTr("Balanced Target")
                                val: StreamingPreferences.VLM_BALANCED_TARGET
                            }
                            ListElement {
                                text: qsTr("Smooth")
                                val: StreamingPreferences.VLM_SMOOTH
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < vrrLatencyModeListModel.count; i++) {
                                if (vrrLatencyModeListModel.get(i).val === preferences.vrrLatencyMode) {
                                    return i
                                }
                            }
                            return 1
                        }
                        onActivated: {
                            preferences.vrrLatencyMode = vrrLatencyModeListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: preferences.vrrLatencyMode === StreamingPreferences.VLM_LOW_LATENCY ?
                                  qsTr("Minimizes added delay. Uneven delivery can cause more stutter or skipped frames.") :
                              preferences.vrrLatencyMode === StreamingPreferences.VLM_SMOOTH ?
                                  qsTr("Uses more padding and holds it longer for steadier motion, with more input delay.") :
                                  qsTr("Targets steadier motion with a moderate timing reserve and balanced input delay.")
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: preferences.vrrLatencyMode === StreamingPreferences.VLM_SMOOTH ?
                                  qsTr("Buffer allowance: up to 4 source frames, limited by queue capacity. Actual learned delay may be lower.") :
                              preferences.vrrLatencyMode === StreamingPreferences.VLM_LOW_LATENCY ?
                                  qsTr("Buffer allowance: up to 1 source frame, limited by queue capacity. Actual learned delay may be lower.") :
                                  qsTr("Buffer allowance: up to 2 source frames, limited by queue capacity. Actual learned delay may be lower.")
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Applies at all VRR frame rates. Reconnect the stream after changing this setting.")
                    }
                }

                CheckBox {
                    hoverEnabled: true
                    id: reduceJudderCheck
                    objectName: "reduceJudderCheck"
                    text: qsTr("Reduce judder")
                    font.pointSize: 12
                    visible: preferences.enableVrr
                    enabled: preferences.enableVsync && preferences.enableVrr
                    checked: preferences.smoothVrrFrameTiming
                    onToggled: preferences.smoothVrrFrameTiming = checked

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Evens out when frames are displayed, including games whose frame rate does not divide the host display's refresh rate. Adds up to a few milliseconds of delay only while uneven frames need it. Does not blend images or eliminate game stalls.") + "\n\n" +
                                  qsTr("Reconnect the stream after changing this setting.")
                }

                CheckBox {
                    id: enableHdr
                    width: parent.width
                    text: qsTr("Enable HDR")
                    font.pointSize: 12

                    enabled: SystemProperties.supportsHdr
                    checked: enabled && preferences.enableHdr
                    onToggled: {
                        if (preferences.enableHdr != checked) {
                            preferences.enableHdr = checked
                            // PyroWave's default bitrate depends on HDR
                            if (slider.pyroWave && preferences.autoAdjustBitrate) {
                                preferences.bitrateKbps = slider.defaultBitrate();
                                slider.value = preferences.bitrateKbps
                            }
                        }
                    }

                    // Updating preferences.videoCodecConfig is handled above

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: enabled ?
                                      qsTr("The stream will be HDR-capable, but some games may require an HDR monitor on your host PC to enable HDR mode.")
                                    :
                                      qsTr("HDR streaming is not supported on this PC.")
                }
            }
        }

        GroupBox {
            width: parent.width - (parent.leftPadding + parent.rightPadding)
            padding: 12
            visible: !gameMode
            title: "<font color=\"skyblue\">" + qsTr("VRR diagnostics") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 8

                CheckBox {
                    id: traceVrrFramesCheckBox
                    objectName: "traceVrrFramesCheck"
                    text: qsTr("Trace VRR frames for debugging")
                    font.pointSize: 12
                    checked: StreamingPreferences.traceVrrFrames
                    onToggled: StreamingPreferences.traceVrrFrames = checked
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Saves frame traces and session logs to the vrr-diagnostics folder on your Desktop, with a separate folder for each stream. Does not change your VRR timing settings.")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Enable VRR and reconnect the stream to start recording. Tracing can use substantial disk space and add diagnostic overhead. Uncheck this after debugging.")
                }

                Button {
                    text: StreamingPreferences.exportingDiagnostics ? qsTr("Exporting...") : qsTr("Export latest recording (ZIP)")
                    enabled: !StreamingPreferences.exportingDiagnostics
                    onClicked: StreamingPreferences.exportLatestDiagnostics()
                }

                Button {
                    text: qsTr("Open diagnostics folder")
                    onClicked: StreamingPreferences.openDiagnosticsFolder()
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Disconnect before exporting. Logs contain hardware and connection details; review them before sharing. Nothing is uploaded automatically.")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    visible: text.length > 0
                    text: StreamingPreferences.diagnosticsStatus
                }
            }
        }

        GroupBox {

            id: audioSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Audio Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resAudioTitle
                    text: qsTr("Audio configuration")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_audio = preferences.audioConfig
                        currentIndex = 0
                        for (var i = 0; i < audioListModel.count; i++) {
                            var el_audio = audioListModel.get(i).val;
                            if (saved_audio === el_audio) {
                                currentIndex = i
                                break
                            }
                        }
                        recalculateWidth()
                    }

                    id: audioComboBox
                    textRole: "text"
                    model: ListModel {
                        id: audioListModel
                        ListElement {
                            text: qsTr("Stereo")
                            val: StreamingPreferences.AC_STEREO
                        }
                        ListElement {
                            text: qsTr("5.1 surround sound")
                            val: StreamingPreferences.AC_51_SURROUND
                        }
                        ListElement {
                            text: qsTr("7.1 surround sound")
                            val: StreamingPreferences.AC_71_SURROUND
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        preferences.audioConfig = audioListModel.get(currentIndex).val
                    }
                }


                CheckBox {
                    id: audioPcCheck
                    width: parent.width
                    text: qsTr("Mute host PC speakers while streaming")
                    font.pointSize: 12
                    checked: !preferences.playAudioOnHost
                    onToggled: {
                        preferences.playAudioOnHost = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("You must restart any game currently in progress for this setting to take effect")
                }

                CheckBox {
                    id: muteOnFocusLossCheck
                    width: parent.width
                    text: qsTr("Mute audio stream when Moonlight is not the active window")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: preferences.muteOnFocusLoss
                    onToggled: {
                        preferences.muteOnFocusLoss = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Mutes Moonlight's audio when you Alt+Tab out of the stream or click on a different window.")
                }
            }
        }

        GroupBox {
            id: hostSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Host Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: optimizeGameSettingsCheck
                    width: parent.width
                    text: qsTr("Optimize game settings for streaming")
                    font.pointSize:  12
                    checked: preferences.gameOptimizations
                    onToggled: {
                        preferences.gameOptimizations = checked
                    }
                }

                CheckBox {
                    id: quitAppAfter
                    width: parent.width
                    text: qsTr("Quit app on host PC after ending stream")
                    font.pointSize: 12
                    checked: preferences.quitAppAfter
                    onToggled: {
                        preferences.quitAppAfter = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This will close the app or game you are streaming when you end your stream. You will lose any unsaved progress!")
                }
            }
        }

        GroupBox {
            id: uiSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("UI Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: languageTitle
                    visible: !gameMode
                    text: qsTr("Language")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_language = preferences.language
                        currentIndex = 0
                        for (var i = 0; i < languageListModel.count; i++) {
                            var el_language = languageListModel.get(i).val;
                            if (saved_language === el_language) {
                                currentIndex = i
                                break
                            }
                        }

                        recalculateWidth()
                    }

                    id: languageComboBox

                    objectName: "languageComboBox"
                    visible: !gameMode
                    textRole: "text"
                    model: ListModel {
                        id: languageListModel
                        ListElement {
                            text: qsTr("Automatic")
                            val: StreamingPreferences.LANG_AUTO
                        }
                        ListElement {
                            text: "Deutsch" // German
                            val: StreamingPreferences.LANG_DE
                        }
                        ListElement {
                            text: "English"
                            val: StreamingPreferences.LANG_EN
                        }
                        ListElement {
                            text: "Français" // French
                            val: StreamingPreferences.LANG_FR
                        }
                        ListElement {
                            text: "简体中文" // Simplified Chinese
                            val: StreamingPreferences.LANG_ZH_CN
                        }
                        ListElement {
                            text: "Norwegian Bokmål"
                            val: StreamingPreferences.LANG_NB_NO
                        }
                        ListElement {
                            text: "русский" // Russian
                            val: StreamingPreferences.LANG_RU
                        }
                        ListElement {
                            text: "Español" // Spanish
                            val: StreamingPreferences.LANG_ES
                        }
                        ListElement {
                            text: "日本語" // Japanese
                            val: StreamingPreferences.LANG_JA
                        }
                        ListElement {
                            text: "Tiếng Việt" // Vietnamese
                            val: StreamingPreferences.LANG_VI
                        }
                        ListElement {
                            text: "ภาษาไทย" // Thai
                            val: StreamingPreferences.LANG_TH
                        }
                        ListElement {
                            text: "한국어" // Korean
                            val: StreamingPreferences.LANG_KO
                        }
                        ListElement {
                            text: "Magyar" // Hungarian
                            val: StreamingPreferences.LANG_HU
                        }
                        ListElement {
                            text: "Nederlands" // Dutch
                            val: StreamingPreferences.LANG_NL
                        }
                        ListElement {
                            text: "Svenska" // Swedish
                            val: StreamingPreferences.LANG_SV
                        }
                        ListElement {
                            text: "Türkçe" // Turkish
                            val: StreamingPreferences.LANG_TR
                        }
                        /* ListElement {
                            text: "Українська" // Ukrainian
                            val: StreamingPreferences.LANG_UK
                        } */
                        ListElement {
                            text: "繁體中文" // Traditional Chinese
                            val: StreamingPreferences.LANG_ZH_TW
                        }
                        ListElement {
                            text: "Português" // Portuguese
                            val: StreamingPreferences.LANG_PT
                        }
                        ListElement {
                            text: "Português do Brasil" // Brazilian Portuguese
                            val: StreamingPreferences.LANG_PT_BR
                        }
                        ListElement {
                            text: "Ελληνικά" // Greek
                            val: StreamingPreferences.LANG_EL
                        }
                        ListElement {
                            text: "Italiano" // Italian
                            val: StreamingPreferences.LANG_IT
                        }
                        /* ListElement {
                            text: "हिन्दी, हिंदी" // Hindi
                            val: StreamingPreferences.LANG_HI
                        } */
                        ListElement {
                            text: "Język polski" // Polish
                            val: StreamingPreferences.LANG_PL
                        }
                        ListElement {
                            text: "Čeština" // Czech
                            val: StreamingPreferences.LANG_CS
                        }
                        /* ListElement {
                            text: "עִבְרִית" // Hebrew
                            val: StreamingPreferences.LANG_HE
                        } */
                        /* ListElement {
                            text: "کرمانجیی خواروو" // Central Kurdish
                            val: StreamingPreferences.LANG_CKB
                        } */
                        /* ListElement {
                            text: "Lietuvių kalba" // Lithuanian
                            val: StreamingPreferences.LANG_LT
                        } */
                        /* ListElement {
                            text: "Eesti" // Estonian
                            val: StreamingPreferences.LANG_ET
                        } */
                        ListElement {
                            text: "Български" // Bulgarian
                            val: StreamingPreferences.LANG_BG
                        }
                        /* ListElement {
                            text: "Esperanto"
                            val: StreamingPreferences.LANG_EO
                        } */
                        ListElement {
                            text: "தமிழ்" // Tamil
                            val: StreamingPreferences.LANG_TA
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        // Retranslating is expensive, so only do it if the language actually changed
                        var new_language = languageListModel.get(currentIndex).val
                        if (preferences.language !== new_language) {
                            preferences.language = languageListModel.get(currentIndex).val
                            if (!preferences.retranslate()) {
                                ToolTip.show(qsTr("You must restart Moonlight for this change to take effect"), 5000)
                            }
                            else {
                                // Force the back operation to pop any AppView pages that exist.
                                // The AppView stops working after retranslate() for some reason.
                                window.clearOnBack = true

                                // Signal other controls to adjust their text
                                languageChanged()
                            }
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: uiDisplayModeTitle
                    text: qsTr("GUI display mode")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: !gameMode && SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        var saved_uidisplaymode = preferences.uiDisplayMode
                        currentIndex = 0
                        for (var i = 0; i < uiDisplayModeListModel.count; i++) {
                            var el_uidisplaymode = uiDisplayModeListModel.get(i).val;
                            if (saved_uidisplaymode === el_uidisplaymode) {
                                currentIndex = i
                                break
                            }
                        }

                        recalculateWidth()
                    }

                    id: uiDisplayModeComboBox

                    objectName: "uiDisplayModeComboBox"
                    visible: !gameMode && SystemProperties.hasDesktopEnvironment
                    textRole: "text"
                    model: ListModel {
                        id: uiDisplayModeListModel
                        ListElement {
                            text: qsTr("Windowed")
                            val: StreamingPreferences.UI_WINDOWED
                        }
                        ListElement {
                            text: qsTr("Maximized")
                            val: StreamingPreferences.UI_MAXIMIZED
                        }   
                        ListElement {
                            text: qsTr("Fullscreen")
                            val: StreamingPreferences.UI_FULLSCREEN
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        preferences.uiDisplayMode = uiDisplayModeListModel.get(currentIndex).val
                    }
                }

                CheckBox {
                    id: connectionWarningsCheck
                    width: parent.width
                    text: qsTr("Show connection quality warnings")
                    font.pointSize: 12
                    checked: preferences.connectionWarnings
                    onToggled: {
                        preferences.connectionWarnings = checked
                    }
                }

                CheckBox {
                    id: configurationWarningsCheck
                    width: parent.width
                    text: qsTr("Show configuration warnings")
                    font.pointSize: 12
                    checked: preferences.configurationWarnings
                    onToggled: {
                        preferences.configurationWarnings = checked
                    }
                }

                CheckBox {
                    visible: !gameMode && SystemProperties.hasDiscordIntegration
                    id: discordPresenceCheck
                    width: parent.width
                    text: qsTr("Discord Rich Presence integration")
                    font.pointSize: 12
                    checked: preferences.richPresence
                    onToggled: {
                        preferences.richPresence = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Updates your Discord status to display the name of the game you're streaming.")
                }

                CheckBox {
                    id: keepAwakeCheck
                    width: parent.width
                    text: qsTr("Keep the display awake while streaming")
                    font.pointSize: 12
                    checked: preferences.keepAwake
                    onToggled: {
                        preferences.keepAwake = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Prevents the screensaver from starting or the display from going to sleep while streaming.")
                }
            }
        }
    }

    Column {
        padding: 10
        rightPadding: 20
        x: settingsPage.singleColumn ? 0 : settingsColumn1.width
        y: settingsPage.singleColumn ? settingsColumn1.height : 0
        id: settingsColumn2
        width: settingsPage.singleColumn ? settingsPage.width : settingsPage.width / 2
        spacing: 15

        GroupBox {
            id: inputSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Input Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: absoluteMouseCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Optimize mouse for remote desktop instead of games")
                    font.pointSize:  12
                    checked: preferences.absoluteMouseMode
                    onToggled: {
                        preferences.absoluteMouseMode = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This enables seamless mouse control without capturing the client's mouse cursor. It is ideal for remote desktop usage but will not work in most games.") + " " +
                                  qsTr("You can toggle this while streaming using Ctrl+Alt+Shift+M.") + "\n\n" +
                                  qsTr("NOTE: Due to a bug in GeForce Experience, this option may not work properly if your host PC has multiple monitors.")
                }

                Row {
                    spacing: 5
                    width: parent.width

                    CheckBox {
                        id: captureSysKeysCheck
                        objectName: "captureSysKeysCheck"
                        hoverEnabled: true
                        text: qsTr("Capture system keyboard shortcuts")
                        font.pointSize: 12
                        enabled: SystemProperties.hasDesktopEnvironment
                        checked: preferences.captureSysKeysMode !== StreamingPreferences.CSK_OFF || !SystemProperties.hasDesktopEnvironment
                        onToggled: captureSysKeysModeComboBox.updatePref()
                        width: Math.min(implicitWidth, parent.width - captureSysKeysModeComboBox.width - parent.spacing)

                        ToolTip.delay: 1000
                        ToolTip.timeout: 10000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("This enables the capture of system-wide keyboard shortcuts like Alt+Tab that would normally be handled by the client OS while streaming.") + "\n\n" +
                                      qsTr("NOTE: Certain keyboard shortcuts like Ctrl+Alt+Del on Windows cannot be intercepted by any application, including Moonlight.")
                    }

                    AutoResizingComboBox {
                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            if (!visible) {
                                // Do nothing if the control won't even be visible
                                return
                            }

                            var saved_syskeysmode = preferences.captureSysKeysMode
                            currentIndex = 0
                            for (var i = 0; i < captureSysKeysModeListModel.count; i++) {
                                var el_syskeysmode = captureSysKeysModeListModel.get(i).val;
                                if (saved_syskeysmode === el_syskeysmode) {
                                    currentIndex = i
                                    break
                                }
                            }

                            recalculateWidth()
                        }

                        id: captureSysKeysModeComboBox
                        maximumWidth: parent.width / 2
                        enabled: captureSysKeysCheck.checked && captureSysKeysCheck.enabled
                        textRole: "text"
                        model: ListModel {
                            id: captureSysKeysModeListModel
                            ListElement {
                                text: qsTr("in fullscreen")
                                val: StreamingPreferences.CSK_FULLSCREEN
                            }
                            ListElement {
                                text: qsTr("always")
                                val: StreamingPreferences.CSK_ALWAYS
                            }
                        }

                        function updatePref() {
                            if (!enabled) {
                                preferences.captureSysKeysMode = StreamingPreferences.CSK_OFF
                            }
                            else {
                                preferences.captureSysKeysMode = captureSysKeysModeListModel.get(currentIndex).val
                            }
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated: {
                            updatePref()
                        }

                    }
                }

                CheckBox {
                    id: absoluteTouchCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Use touchscreen as a virtual trackpad")
                    font.pointSize:  12
                    checked: !preferences.absoluteTouchMode
                    onToggled: {
                        preferences.absoluteTouchMode = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("When checked, the touchscreen acts like a trackpad. When unchecked, the touchscreen will directly control the mouse pointer.")
                }

                CheckBox {
                    id: swapMouseButtonsCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Swap left and right mouse buttons")
                    font.pointSize:  12
                    checked: preferences.swapMouseButtons
                    onToggled: {
                        preferences.swapMouseButtons = checked
                    }
                }

                CheckBox {
                    id: reverseScrollButtonsCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Reverse mouse scrolling direction")
                    font.pointSize: 12
                    checked: preferences.reverseScrollDirection
                    onToggled: {
                        preferences.reverseScrollDirection = checked
                    }
                }
            }
        }

        GroupBox {
            id: gamepadSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Gamepad Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: swapFaceButtonsCheck
                    width: parent.width
                    text: qsTr("Swap A/B and X/Y gamepad buttons")
                    font.pointSize: 12
                    checked: preferences.swapFaceButtons
                    onToggled: {
                        preferences.swapFaceButtons = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This switches gamepads into a Nintendo-style button layout")
                }

                CheckBox {
                    id: singleControllerCheck
                    width: parent.width
                    text: qsTr("Force gamepad #1 always connected")
                    font.pointSize:  12
                    checked: !preferences.multiController
                    onToggled: {
                        preferences.multiController = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Forces a single gamepad to always stay connected to the host, even if no gamepads are actually connected to this PC.") + " " +
                                  qsTr("Only enable this option when streaming a game that doesn't support gamepads being connected after startup.")
                }

                CheckBox {
                    id: gamepadMouseCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Enable mouse control with gamepads by holding the 'Start' button")
                    font.pointSize: 12
                    checked: preferences.gamepadMouse
                    onToggled: {
                        preferences.gamepadMouse = checked
                    }
                }

                CheckBox {
                    id: backgroundGamepadCheck
                    width: parent.width
                    text: qsTr("Process gamepad input when Moonlight is in the background")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: preferences.backgroundGamepad
                    onToggled: {
                        preferences.backgroundGamepad = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Allows Moonlight to capture gamepad inputs even if it's not the current window in focus")
                }
            }
        }

        GroupBox {
            id: advancedSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Advanced Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resVDSTitle
                    text: qsTr("Video decoder")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_vds = preferences.videoDecoderSelection
                        currentIndex = 0
                        for (var i = 0; i < decoderListModel.count; i++) {
                            var el_vds = decoderListModel.get(i).val;
                            if (saved_vds === el_vds) {
                                currentIndex = i
                                break
                            }
                        }
                        recalculateWidth()
                    }

                    id: decoderComboBox
                    textRole: "text"
                    model: ListModel {
                        id: decoderListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.VDS_AUTO
                        }
                        ListElement {
                            text: qsTr("Force software decoding")
                            val: StreamingPreferences.VDS_FORCE_SOFTWARE
                        }
                        ListElement {
                            text: qsTr("Force hardware decoding")
                            val: StreamingPreferences.VDS_FORCE_HARDWARE
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated: {
                        if (enabled) {
                            preferences.videoDecoderSelection = decoderListModel.get(currentIndex).val
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: rendererTitle
                    text: qsTr("Renderer")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.isDarwin
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_rs = preferences.rendererSelection

                        // Default to Automatic
                        currentIndex = 0

                        for(var i = 0; i < rendererListModel.count; i++) {
                            var el_rs = rendererListModel.get(i).val;
                            if (saved_rs === el_rs) {
                                currentIndex = i
                                break
                            }
                        }

                        recalculateWidth()
                    }

                    id: rendererComboBox
                    visible: SystemProperties.isDarwin
                    textRole: "text"
                    model: ListModel {
                        id: rendererListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.RS_AUTO
                        }
                        ListElement {
                            text: "Vulkan"
                            val: StreamingPreferences.RS_VULKAN
                        }
                        ListElement {
                            text: "Metal"
                            val: StreamingPreferences.RS_METAL
                        }
                        ListElement {
                            text: "AVSampleBufferDisplayLayer"
                            val: StreamingPreferences.RS_AVSBDL
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        preferences.rendererSelection = rendererListModel.get(currentIndex).val
                    }
                }

                CheckBox {
                    id: unlockBitrate
                    width: parent.width
                    text: qsTr("Unlock bitrate limit (Experimental)")
                    font.pointSize: 12

                    checked: preferences.unlockBitrate
                    onToggled: {
                        preferences.unlockBitrate = checked
                        preferences.bitrateKbps = Math.min(preferences.bitrateKbps, slider.to)
                        slider.value = preferences.bitrateKbps
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This unlocks extremely high video bitrates for use with Sunshine hosts. It should only be used when streaming over an Ethernet LAN connection.")
                }

                CheckBox {
                    id: enableMdns
                    objectName: "enableMdns"
                    visible: !gameMode
                    width: parent.width
                    text: qsTr("Automatically find PCs on the local network (Recommended)")
                    font.pointSize: 12
                    checked: preferences.enableMdns
                    onToggled: {
                        // This is called on init, so only do the work if we've
                        // actually changed the value.
                        if (preferences.enableMdns != checked) {
                            preferences.enableMdns = checked

                            // Restart polling so the mDNS change takes effect
                            if (window.pollingActive) {
                                ComputerManager.stopPollingAsync()
                                ComputerManager.startPolling()
                            }
                        }
                    }
                }

                CheckBox {
                    id: detectNetworkBlocking
                    visible: !gameMode
                    width: parent.width
                    text: qsTr("Automatically detect blocked connections (Recommended)")
                    font.pointSize: 12
                    checked: preferences.detectNetworkBlocking
                    onToggled: {
                        preferences.detectNetworkBlocking = checked
                    }
                }

                CheckBox {
                    id: showPerformanceOverlay
                    width: parent.width
                    text: qsTr("Show performance stats while streaming")
                    font.pointSize: 12
                    checked: preferences.showPerformanceOverlay
                    onToggled: {
                        preferences.showPerformanceOverlay = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Display real-time stream performance information while streaming.") + "\n\n" +
                                  qsTr("You can toggle it at any time while streaming using Ctrl+Alt+Shift+S or Select+L1+R1+X.") + "\n\n" +
                                  qsTr("The performance overlay is not supported on Steam Link or Raspberry Pi.")
                }
            }
        }
    }
}
