import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
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
        Component.onCompleted: initialize(ComputerManager)
    }
    objectName: qsTr("Settings")

    signal languageChanged()

    boundsBehavior: Flickable.OvershootBounds

    contentWidth: settingsColumn1.width > settingsColumn2.width ? settingsColumn1.width : settingsColumn2.width
    contentHeight: settingsColumn1.height > settingsColumn2.height ? settingsColumn1.height : settingsColumn2.height

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

    StackView.onActivated: {
        // This enables Tab and BackTab based navigation rather than arrow keys.
        // It is required to shift focus between controls on the settings page.
        SdlGamepadKeyNavigation.setUiNavMode(true)

        // Highlight the first item if a gamepad is connected
        if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            resolutionComboBox.forceActiveFocus(Qt.TabFocus)
        }
    }

    StackView.onDeactivating: {
        SdlGamepadKeyNavigation.setUiNavMode(false)

        // Save the prefs so the Session can observe the changes
        StreamingPreferences.save()
    }

    Component.onDestruction: {
        // Also save preferences on destruction, since we won't get a
        // deactivating callback if the user just closes Moonlight
        StreamingPreferences.save()
    }

    Column {
        padding: 10
        id: settingsColumn1
        width: settingsPage.width / 2
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
                            var saved_width = StreamingPreferences.width
                            var saved_height = StreamingPreferences.height
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
                                                               "text": qsTr("Custom")+" ("+StreamingPreferences.width+"x"+StreamingPreferences.height+")",
                                                               "video_width": ""+StreamingPreferences.width,
                                                               "video_height": ""+StreamingPreferences.height,
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
                            if (StreamingPreferences.width !== selectedWidth || StreamingPreferences.height !== selectedHeight) {
                                StreamingPreferences.width = selectedWidth
                                StreamingPreferences.height = selectedHeight

                                if (StreamingPreferences.autoAdjustBitrate) {
                                    StreamingPreferences.bitrateKbps = slider.defaultBitrate();
                                    slider.value = StreamingPreferences.bitrateKbps
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
                            var fpsChanged = StreamingPreferences.fps !== selectedFps
                            StreamingPreferences.fps = selectedFps

                            if (fpsChanged && StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = slider.defaultBitrate();
                                slider.value = StreamingPreferences.bitrateKbps
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
                                        placeholderText: fpsListModel.get(fpsComboBox.currentIndex).video_fps
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
                            var choices = StreamingPreferences.getFpsChoices(getRefreshRates())
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

                            var saved_fps = StreamingPreferences.fps
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
                            StreamingPreferences.enableVsyncChanged.connect(reinitialize)
                            StreamingPreferences.enableVrrChanged.connect(reinitialize)
                        }

                        model: ListModel {
                            id: fpsListModel
                        }

                        id: fpsComboBox
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

                        var saved_vcc = StreamingPreferences.videoCodecConfig

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

                        activated(currentIndex)
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
                            StreamingPreferences.videoCodecConfig = codecListModel.get(currentIndex).val

                            // PyroWave's useful bitrates are an order of magnitude above
                            // the other codecs', so switching in or out resets a default.
                            if (slider.pyroWave !== wasPyroWave && StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = slider.defaultBitrate()
                                slider.value = StreamingPreferences.bitrateKbps
                            }
                            else if (StreamingPreferences.bitrateKbps > slider.to) {
                                StreamingPreferences.bitrateKbps = slider.to
                                slider.value = StreamingPreferences.bitrateKbps
                            }
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 8000
                    ToolTip.visible: hovered && slider.pyroWave
                    ToolTip.text: qsTr("PyroWave is a GPU wavelet codec. It needs a wired connection with hundreds of Mbps to spare and a host with PyroWave support; other hosts fall back to H.264. On Linux, GPU readback and upload may limit frame rate.")
                }

                CheckBox {
                    id: enableYUV444
                    width: parent.width
                    text: qsTr("Enable YUV 4:4:4")
                    font.pointSize: 12

                    checked: StreamingPreferences.enableYUV444
                    onCheckedChanged: {
                        // This is called on init, so only reset to default bitrate when checked state changes.
                        if (StreamingPreferences.enableYUV444 != checked) {
                            StreamingPreferences.enableYUV444 = checked
                            if (StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = slider.defaultBitrate();
                                slider.value = StreamingPreferences.bitrateKbps
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

                        readonly property bool pyroWave: StreamingPreferences.videoCodecConfig === StreamingPreferences.VCC_FORCE_PYROWAVE

                        // PyroWave needs several hundred Mbps, up to multi-gigabit LANs
                        function defaultBitrate() {
                            if (pyroWave) {
                                return StreamingPreferences.getDefaultPyroWaveBitrate(StreamingPreferences.width,
                                                                                      StreamingPreferences.height,
                                                                                      StreamingPreferences.fps,
                                                                                      StreamingPreferences.enableYUV444,
                                                                                      StreamingPreferences.enableHdr)
                            }
                            return StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                          StreamingPreferences.height,
                                                                          StreamingPreferences.fps,
                                                                          StreamingPreferences.enableYUV444)
                        }

                        value: StreamingPreferences.bitrateKbps

                        stepSize: pyroWave ? 5000 : 500
                        from : pyroWave ? 5000 : 500
                        to: pyroWave ? 3000000 : (StreamingPreferences.unlockBitrate ? 500000 : 150000)

                        snapMode: "SnapOnRelease"
                        width: Math.min(bitrateDesc.implicitWidth, parent.width - (resetBitrateButton.visible ? resetBitrateButton.width + parent.spacing : 0))

                        onValueChanged: {
                            bitrateTitle.text = qsTr("Video bitrate: %1 Mbps").arg(value / 1000.0)
                            StreamingPreferences.bitrateKbps = value
                        }

                        onMoved: {
                            StreamingPreferences.autoAdjustBitrate = false
                        }

                        Component.onCompleted: {
                            // Refresh the text after translations change
                            languageChanged.connect(valueChanged)
                        }
                    }

                    Button {
                        id: resetBitrateButton
                        text: qsTr("Use Default (%1 Mbps)").arg(slider.defaultBitrate() / 1000.0)
                        visible: StreamingPreferences.bitrateKbps !== slider.defaultBitrate()
                        onClicked: {
                            var defaultBitrate = slider.defaultBitrate()
                            StreamingPreferences.bitrateKbps = defaultBitrate
                            StreamingPreferences.autoAdjustBitrate = true
                            slider.value = defaultBitrate
                        }
                    }
                }

                PyroWavePacketSpeedSettings {
                    width: parent.width
                    visible: SystemProperties.hasPyroWave && slider.pyroWave
                    paceMbps: StreamingPreferences.pyroWavePaceMbps
                    onPaceEdited: function(value) { StreamingPreferences.pyroWavePaceMbps = value }
                }

                Column {
                    width: parent.width
                    spacing: 5
                    visible: slider.pyroWave && (Qt.platform.os === "linux" || Qt.platform.os === "windows")

                    Component.onCompleted: NetworkBuffers.refresh()

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

                CalibrationButton {
                    objectName: "pyrowaveCalibrationOpen"
                    text: qsTr("Calibrate PyroWave…")
                    visible: SystemProperties.hasPyroWave && slider.pyroWave
                    enabled: !PyroWaveCalibrator.running
                    onClicked: calibrationDialog.open()
                }

                NavigableDialog {
                    id: calibrationDialog
                    objectName: "pyrowaveCalibrationDialog"
                    property int step: 0
                    property int targetIndex: 1
                    property bool decoderWasRunning: false
                    property int testFps: 60
                    property int testedTarget: -1
                    property string testedHostUuid: ""
                    property bool hasStarted: false
                    property bool stopRequested: false
                    readonly property int selectedTarget: targets[targetIndex].value
                    readonly property string selectedHostUuid: calibrationHost.currentIndex >= 0 ? calibrationHosts.uuidAt(calibrationHost.currentIndex) : ""
                    readonly property bool resultsMatch: hasStarted && testedTarget === selectedTarget && testedHostUuid === selectedHostUuid
                    readonly property var targets: [
                        { text: qsTr("Minimum"), summary: qsTr("Half recommendation"), value: PyroWaveCalibrator.Minimum,
                          description: qsTr("Half the developer's recommended image bitrate. Uses less bandwidth.") },
                        { text: qsTr("Recommended"), summary: qsTr("Developer choice"), value: PyroWaveCalibrator.Recommended,
                          description: qsTr("The developer's recommended image bitrate for each format.") },
                        { text: qsTr("Moderate"), summary: qsTr("60% of link speed"), value: PyroWaveCalibrator.Moderate,
                          description: qsTr("Aim for 60% of the link speed including overhead, never below Recommended. Calibration lowers it to what the connection sustains.") },
                        { text: qsTr("Maximum"), summary: qsTr("Full measured budget"), value: PyroWaveCalibrator.Maximum,
                          description: qsTr("Find the highest tested bitrate the connection and this device can sustain, with network headroom.") }
                    ]
                    readonly property bool canContinue: resultsMatch && PyroWaveCalibrator.bandwidthReady
                    title: step === 0 ? qsTr("1 of 2 · Connection & quality") : qsTr("2 of 2 · Decoder stress test")
                    width: Math.min(settingsPage.width - 24, step === 0 ? 850 : 1100)
                    height: Math.min(settingsPage.height - 24, step === 0 ? 480 : 760)
                    standardButtons: Dialog.NoButton
                    focus: true
                    background: Rectangle {
                        color: "#303030"
                        radius: 12
                        border.color: "#606060"
                    }
                    onAboutToShow: {
                        PyroWaveCalibrator.reset()
                        step = 0
                        if (calibrationHost.count > 0 && calibrationHost.currentIndex < 0) calibrationHost.currentIndex = 0
                        decoderWasRunning = false
                        testFps = StreamingPreferences.fps
                        testedTarget = -1
                        hasStarted = false
                        stopRequested = false
                    }
                    onOpened: { SdlGamepadKeyNavigation.setUiNavMode(true); focusTarget() }
                    onAboutToHide: PyroWaveCalibrator.reset()

                    function focusTarget() {
                        var card = calibrationTargets.itemAt(targetIndex)
                        if (card) card.forceActiveFocus(Qt.TabFocusReason)
                    }
                    function changeTarget(index) {
                        if (PyroWaveCalibrator.running) return
                        targetIndex = Math.max(0, Math.min(targets.length - 1, index))
                        focusTarget()
                    }
                    function nextStep() {
                        if (!canContinue) return
                        step = 1
                        stopRequested = false
                        PyroWaveCalibrator.startDecoderTest(testedHostUuid, testedTarget)
                        calibrationRunButton.forceActiveFocus(Qt.TabFocusReason)
                    }
                    Connections {
                        target: PyroWaveCalibrator
                        function onChanged() {
                            if (calibrationDialog.step !== 1 || !calibrationDialog.opened) return
                            if (PyroWaveCalibrator.running) calibrationDialog.decoderWasRunning = true
                            else if (calibrationDialog.decoderWasRunning) {
                                calibrationDialog.decoderWasRunning = false
                                Qt.callLater(function() {
                                    if (calibrationDialog.opened && calibrationDialog.step === 1 && !PyroWaveCalibrator.running)
                                        calibrationDialog.focusOption(0, 0, 1)
                                })
                            }
                        }
                    }

                    function testTarget() {
                        if (PyroWaveCalibrator.running || calibrationHost.currentIndex < 0) return
                        testedTarget = selectedTarget
                        testedHostUuid = selectedHostUuid
                        hasStarted = true
                        stopRequested = false
                        PyroWaveCalibrator.start(ComputerManager,
                                                 testedHostUuid,
                                                 testFps,
                                                 Math.round(Screen.width * Screen.devicePixelRatio),
                                                 Math.round(Screen.height * Screen.devicePixelRatio),
                                                 testedTarget)
                        calibrationRunButton.forceActiveFocus(Qt.TabFocusReason)
                    }

                    function focusOption(row, mode, direction) {
                        // Up/down follows one chroma column; left/right changes
                        // chroma without changing HDR/SDR. Skip unavailable formats.
                        var chroma = mode < 2 ? 0 : 2
                        var position = row * 2 + mode % 2
                        for (var attempt = 0; attempt < rows.length * 2; ++attempt) {
                            if (position < 0) { calibrationRunButton.forceActiveFocus(Qt.TabFocusReason); return }
                            if (position >= rows.length * 2) { calibrationCloseButton.forceActiveFocus(Qt.TabFocusReason); return }
                            var rowItem = calibrationRows.itemAt(Math.floor(position / 2))
                            var columnItem = rowItem ? rowItem.columns.itemAt(chroma / 2) : null
                            var button = columnItem ? columnItem.formats.itemAt(position % 2) : null
                            if (button && button.enabled) { button.forceActiveFocus(Qt.TabFocusReason); return }
                            if (!direction) return
                            position += direction
                        }
                        calibrationCloseButton.forceActiveFocus(Qt.TabFocusReason)
                    }

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
                    readonly property var samples: resultsMatch ? PyroWaveCalibrator.results : []
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
                        return step === 1 && !PyroWaveCalibrator.running && resultsMatch && option && option.valid && !hdrUnavailable(option)
                    }

                    function headline(option, hdr) {
                        var format = hdr ? qsTr("HDR (10-bit)") : qsTr("SDR (8-bit)")
                        if (option && option.valid) return format + " · " + qsTr("%1 Mbps").arg(option.bitrateKbps / 1000)
                        return format
                    }

                    function tierText(option) {
                        if (!option) return PyroWaveCalibrator.running ? qsTr("Testing…") : qsTr("Not tested")
                        if (option.tier === "error") return option.error
                        if (hdrUnavailable(option)) return qsTr("No HDR display")
                        var text = option.tier === "slow" ? qsTr("Can't keep up") : option.tier === "any" ? qsTr("Any display") :
                                   option.tier === "vrr" ? qsTr("Needs VRR") : qsTr("VRR · Smooth")
                        if (option.quality === "reduced") text += " · " + qsTr("Reduced quality")
                        else if (option.quality === "low") text += " · " + qsTr("Low quality")
                        return text
                    }

                    function tierColor(option) {
                        if (!option || hdrUnavailable(option)) return "#9e9e9e"
                        if (option.tier !== "slow" && option.tier !== "error") {
                            var minimum = Math.max(5000, Math.floor(option.guideKbps / 10000) * 5000)
                            var required = selectedTarget === PyroWaveCalibrator.Minimum ? minimum : option.guideKbps
                            if (option.imageKbps < minimum) return tierColors.slow
                            if (option.imageKbps < required && option.tier === "any") return tierColors.vrr
                        }
                        return tierColors[option.tier]
                    }

                    function frameCost(option) {
                        return qsTr("99% of frames take up to %1 ms to decode and draw, %2% of each frame at %3 FPS.")
                                .arg(option.frameMs.toFixed(1)).arg(option.loadPercent).arg(testFps)
                    }

                    function optionDetail(option) {
                        if (!option || !option.valid) return ""
                        var details = [frameCost(option)]
                        if (!option.keepsUp) {
                            details.push(qsTr("A lower bitrate doesn't make this device fast enough, so the stream will stutter or fall behind. You can still use it."))
                        }
                        if (option.tier === "vrr") {
                            details.push(qsTr("With VRR the occasional slow frame is shown slightly late; on a fixed-refresh display it would stutter."))
                        }
                        else if (option.tier === "vrrLarge") {
                            details.push(qsTr("Slow frames use nearly the whole frame, so only the Smooth VRR latency mode's larger buffer hides them; other modes and fixed-refresh displays would stutter."))
                        }
                        details.push(qsTr("%1 Mbps of image data reaches %2 dB on the codec author's quality scale; he recommends %3 Mbps of image data (35 dB) for this format. The applied rate includes FEC and packet overhead.")
                                     .arg(option.imageKbps / 1000).arg(option.qualityDb.toFixed(1))
                                     .arg(option.guideKbps / 1000))
                        if (option.deviceLimited) details.push(qsTr("The bitrate was lowered so this device keeps up."))
                        if (option.linkLimited) details.push(qsTr("The tested network budget limits the image bitrate for this target."))
                        return details.join(" ")
                    }

                    function applyChoice(option) {
                        if (!canApply(option)) return
                        StreamingPreferences.videoCodecConfig = StreamingPreferences.VCC_FORCE_PYROWAVE
                        for (var codecIndex = 0; codecIndex < codecListModel.count; codecIndex++) {
                            if (codecListModel.get(codecIndex).val === StreamingPreferences.VCC_FORCE_PYROWAVE) {
                                codecComboBox.currentIndex = codecIndex
                                break
                            }
                        }
                        StreamingPreferences.width = option.width
                        StreamingPreferences.height = option.height
                        StreamingPreferences.enableYUV444 = option.chroma444
                        StreamingPreferences.enableHdr = option.hdr
                        StreamingPreferences.bitrateKbps = option.bitrateKbps
                        // The tested frame pace; 0 lets the host use its default.
                        StreamingPreferences.pyroWavePaceMbps = PyroWaveCalibrator.paceMbps
                        StreamingPreferences.autoAdjustBitrate = false
                        slider.value = StreamingPreferences.bitrateKbps

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
                        StreamingPreferences.save()
                        close()
                    }

                    footer: Pane {
                        padding: 12
                        contentItem: RowLayout {
                            spacing: 8
                            CalibrationButton {
                                id: calibrationRunButton
                                objectName: "pyrowaveCalibrationRun"
                                text: PyroWaveCalibrator.running ? (calibrationDialog.stopRequested ? qsTr("Stopping…") : qsTr("Stop test")) :
                                      calibrationDialog.step === 0 ? qsTr("Test bandwidth") : qsTr("Test decoder again")
                                enabled: PyroWaveCalibrator.running ? !calibrationDialog.stopRequested :
                                         calibrationDialog.step === 0 ? calibrationHost.currentIndex >= 0 : calibrationDialog.canContinue
                                implicitHeight: 44
                                highlighted: calibrationDialog.step === 0 && !calibrationDialog.canContinue
                                onClicked: {
                                    if (PyroWaveCalibrator.running) {
                                        calibrationDialog.stopRequested = true
                                        PyroWaveCalibrator.cancel()
                                    }
                                    else if (calibrationDialog.step === 0) calibrationDialog.testTarget()
                                    else {
                                        calibrationDialog.stopRequested = false
                                        PyroWaveCalibrator.startDecoderTest(calibrationDialog.testedHostUuid, calibrationDialog.testedTarget)
                                    }
                                }
                                Keys.onPressed: function(event) {
                                    if (event.key === Qt.Key_Backtab || event.key === Qt.Key_Up ||
                                            (event.key === Qt.Key_Tab && (event.modifiers & Qt.ShiftModifier))) {
                                        if (calibrationDialog.step === 0 && !PyroWaveCalibrator.running) calibrationDialog.focusTarget()
                                        else calibrationCloseButton.forceActiveFocus(Qt.TabFocusReason)
                                    }
                                    else if (event.key === Qt.Key_Right && calibrationDialog.step === 1) {
                                        calibrationDetails.forceActiveFocus(Qt.TabFocusReason)
                                    }
                                    else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Down || event.key === Qt.Key_Right) {
                                        if (calibrationDialog.step === 1) calibrationDialog.focusOption(0, 0, 1)
                                        else if (calibrationNextButton.enabled) calibrationNextButton.forceActiveFocus(Qt.TabFocusReason)
                                        else calibrationCloseButton.forceActiveFocus(Qt.TabFocusReason)
                                    }
                                    else return
                                    event.accepted = true
                                }
                            }
                            CalibrationButton {
                                id: calibrationDetails
                                objectName: "pyrowaveCalibrationDetails"
                                visible: calibrationDialog.step === 1
                                text: qsTr("About results")
                                checkable: true
                                implicitHeight: 44
                                KeyNavigation.left: calibrationRunButton
                                KeyNavigation.right: calibrationCloseButton.enabled ? calibrationCloseButton : calibrationRunButton
                                KeyNavigation.backtab: calibrationRunButton
                                Keys.onTabPressed: calibrationDialog.focusOption(0, 0, 1)
                                Keys.onDownPressed: calibrationDialog.focusOption(0, 0, 1)
                            }
                            Item { Layout.fillWidth: true }
                            CalibrationButton {
                                id: calibrationCloseButton
                                objectName: "pyrowaveCalibrationBack"
                                text: calibrationDialog.step === 0 ? qsTr("Cancel") : qsTr("Back")
                                enabled: calibrationDialog.step === 0 || !PyroWaveCalibrator.running
                                implicitHeight: 44
                                onClicked: {
                                    if (calibrationDialog.step === 0) calibrationDialog.close()
                                    else { calibrationDialog.step = 0; calibrationDialog.focusTarget() }
                                }
                                Keys.onTabPressed: {
                                    if (calibrationDialog.step === 0 && !PyroWaveCalibrator.running) calibrationHost.forceActiveFocus(Qt.TabFocusReason)
                                    else calibrationRunButton.forceActiveFocus(Qt.TabFocusReason)
                                }
                            }
                            CalibrationButton {
                                id: calibrationNextButton
                                objectName: "pyrowaveCalibrationNext"
                                text: qsTr("Next →")
                                visible: calibrationDialog.step === 0
                                enabled: calibrationDialog.canContinue
                                implicitHeight: 44
                                highlighted: true
                                onClicked: calibrationDialog.nextStep()
                                KeyNavigation.backtab: calibrationRunButton
                                KeyNavigation.tab: calibrationCloseButton
                                KeyNavigation.left: calibrationRunButton
                            }
                        }
                    }

                    contentItem: StackLayout {
                        currentIndex: calibrationDialog.step
                        ScrollView {
                            id: calibrationConnectionScroll
                            objectName: "pyrowaveCalibrationConnectionScroll"
                            clip: true
                            contentWidth: availableWidth
                            ColumnLayout {
                                width: calibrationConnectionScroll.availableWidth
                                height: Math.max(implicitHeight, calibrationConnectionScroll.availableHeight)
                                spacing: 12
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("Host") }
                                    AutoResizingComboBox {
                                        id: calibrationHost
                                        objectName: "pyrowaveCalibrationHost"
                                        Layout.fillWidth: true
                                        model: calibrationHosts
                                        textRole: "name"
                                        onCountChanged: { if (count > 0 && currentIndex < 0) currentIndex = 0 }
                                        displayText: count > 0 ? currentText : qsTr("No hosts available")
                                        enabled: !PyroWaveCalibrator.running
                                        // Closed selectors use Tab for controller focus traversal.
                                        // An open popup owns arrow keys for selecting a host.
                                        KeyNavigation.backtab: calibrationCloseButton
                                        Keys.onTabPressed: function(event) {
                                            if (popup.visible) {
                                                event.accepted = false
                                                return
                                            }
                                            if (event.modifiers & Qt.ShiftModifier)
                                                calibrationCloseButton.forceActiveFocus(Qt.TabFocusReason)
                                            else calibrationDialog.focusTarget()
                                        }
                                    }
                                    Label { text: qsTr("%1 FPS").arg(calibrationDialog.testFps) }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    font.pointSize: 16
                                    font.bold: true
                                    text: qsTr("Choose your quality target")
                                    wrapMode: Text.Wrap
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Repeater {
                                        id: calibrationTargets
                                        model: calibrationDialog.targets
                                        delegate: CalibrationButton {
                                            id: targetCard
                                            objectName: "pyrowaveCalibrationTarget" + index
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: 1
                                            Layout.preferredHeight: 82
                                            text: modelData.text
                                            checked: calibrationDialog.targetIndex === index
                                            enabled: !PyroWaveCalibrator.running
                                            activeFocusOnTab: true
                                            Accessible.name: modelData.text
                                            Accessible.description: modelData.description
                                            contentItem: Column {
                                                spacing: 6
                                                Label {
                                                    width: parent.width
                                                    text: (targetCard.checked ? "\u2713 " : "") + targetCard.text
                                                    font.bold: targetCard.checked
                                                    font.pointSize: 11
                                                    horizontalAlignment: Text.AlignHCenter
                                                    elide: Text.ElideRight
                                                    color: "#ffffff"
                                                }
                                                Label {
                                                    width: parent.width
                                                    text: modelData.summary
                                                    font.pointSize: 9
                                                    horizontalAlignment: Text.AlignHCenter
                                                    wrapMode: Text.Wrap
                                                    color: "#c7c7c7"
                                                }
                                            }
                                            opacity: targetCard.enabled ? 1 : 0.6
                                            onClicked: calibrationDialog.changeTarget(index)
                                            Keys.priority: Keys.BeforeItem
                                            Keys.onPressed: function(event) {
                                                if (event.key === Qt.Key_Left) calibrationDialog.changeTarget(index - 1)
                                                else if (event.key === Qt.Key_Right) calibrationDialog.changeTarget(index + 1)
                                                else if (event.key === Qt.Key_Backtab || event.key === Qt.Key_Up ||
                                                         (event.key === Qt.Key_Tab && (event.modifiers & Qt.ShiftModifier)))
                                                    calibrationHost.forceActiveFocus(Qt.TabFocusReason)
                                                else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Down)
                                                    (calibrationRunButton.enabled ? calibrationRunButton : calibrationCloseButton).forceActiveFocus(Qt.TabFocusReason)
                                                else return
                                                event.accepted = true
                                            }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.minimumHeight: 32
                                    wrapMode: Text.Wrap
                                    text: calibrationDialog.targets[calibrationDialog.targetIndex].description
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Test your connection first, then continue to test this device's decoder.")
                                    wrapMode: Text.Wrap
                                    opacity: 0.7
                                }
                                Item { Layout.fillHeight: true }
                                Pane {
                                    Layout.fillWidth: true
                                    padding: 12
                                    readonly property string grade: calibrationDialog.canContinue ? PyroWaveCalibrator.bandwidthQuality : ""
                                    readonly property bool warning: grade === "reduced"
                                    background: Rectangle {
                                        color: parent.grade === "low" ? "#452b2b" : calibrationDialog.canContinue ? (parent.warning ? "#413824" : "#283b2e") : "#252525"
                                        radius: 8
                                        border.color: parent.grade === "low" ? "#b45c5c" : calibrationDialog.canContinue ? (parent.warning ? "#aa8542" : "#568260") : "#505050"
                                    }
                                    contentItem: RowLayout {
                                        spacing: 12
                                        // Keep the text width stable when the spinner appears.
                                        Item {
                                            Layout.preferredWidth: 28
                                            Layout.preferredHeight: 28
                                            BusyIndicator {
                                                anchors.fill: parent
                                                running: PyroWaveCalibrator.running
                                                visible: running
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 4
                                            Label {
                                                id: calibrationStatusText
                                                objectName: "pyrowaveCalibrationStatus"
                                                Layout.fillWidth: true
                                                Layout.minimumHeight: fontMetrics.height * 3
                                                verticalAlignment: Text.AlignTop
                                                wrapMode: Text.Wrap
                                                text: !calibrationDialog.hasStarted ? qsTr("Ready to test your connection.") :
                                                      !calibrationDialog.resultsMatch ? qsTr("Selection changed. Test bandwidth again to continue.") : PyroWaveCalibrator.message
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                Layout.minimumHeight: fontMetrics.height * 2
                                                opacity: calibrationDialog.canContinue ? 1 : 0
                                                verticalAlignment: Text.AlignTop
                                                wrapMode: Text.Wrap
                                                color: PyroWaveCalibrator.bandwidthQuality === "low" ? "#ef9a9a" :
                                                       PyroWaveCalibrator.bandwidthQuality === "reduced" ? "#ffd180" : "#a5d6a7"
                                                text: qsTr("Measured budget: %1 Mbps · Target needs: %2 Mbps including overhead")
                                                    .arg(PyroWaveCalibrator.bandwidthKbps / 1000).arg(PyroWaveCalibrator.bandwidthTargetKbps / 1000)
                                            }
                                            FontMetrics { id: fontMetrics; font: calibrationStatusText.font }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    font.pointSize: 9
                                    color: "#bdbdbd"
                                    wrapMode: Text.Wrap
                                    text: qsTr("↑↓ Navigate  ·  ←→ Choose target  ·  %1 Select  ·  %2 Back")
                                        .arg(StreamingPreferences.swapFaceButtons ? "B" : "A")
                                        .arg(StreamingPreferences.swapFaceButtons ? "A" : "B")
                                }
                            }
                        }
                        ColumnLayout {
                            spacing: 8
                            Label {
                                Layout.fillWidth: true
                                font.pointSize: 14
                                font.bold: true
                                text: calibrationDialog.targets[calibrationDialog.targetIndex].text + " · " + qsTr("%1 FPS").arg(calibrationDialog.testFps)
                            }
                            Label {
                                objectName: "pyrowaveDecoderStatus"
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: PyroWaveCalibrator.message
                            }
                            ProgressBar {
                                Layout.fillWidth: true
                                from: 0
                                to: calibrationDialog.rows.length * 4
                                value: calibrationDialog.samples.length
                                visible: PyroWaveCalibrator.running
                            }
                            Row {
                                id: calibrationHeaders
                                Layout.fillWidth: true
                                spacing: 6
                                Label { width: 72; text: qsTr("Resolution") }
                                Label { width: (calibrationHeaders.width - 84) / 2; text: "4:4:4"; horizontalAlignment: Text.AlignHCenter }
                                Label { width: (calibrationHeaders.width - 84) / 2; text: "4:2:0"; horizontalAlignment: Text.AlignHCenter }
                            }
                            Flickable {
                                id: calibrationResults
                                objectName: "pyrowaveCalibrationResults"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                contentWidth: width
                                contentHeight: calibrationTable.height
                                boundsBehavior: Flickable.StopAtBounds
                                ScrollBar.vertical: ScrollBar {}
                                function ensureFocusedVisible() {
                                    var item = Window.activeFocusItem
                                    if (!item) return
                                    var ancestor = item
                                    while (ancestor && ancestor !== contentItem) ancestor = ancestor.parent
                                    if (!ancestor) return
                                    var position = item.mapToItem(contentItem, 0, 0)
                                    if (position.y < contentY) contentY = position.y
                                    else if (position.y + item.height > contentY + height)
                                        contentY = Math.min(Math.max(0, contentHeight - height), position.y + item.height - height)
                                }
                                Window.onActiveFocusItemChanged: ensureFocusedVisible()
                                onHeightChanged: ensureFocusedVisible()
                                Column {
                                    id: calibrationTable
                                    width: parent.width
                                    spacing: 6
                                    Label {
                                        width: parent.width
                                        visible: calibrationDetails.checked
                                        wrapMode: Text.Wrap
                                        font.pointSize: 9
                                        text: PyroWaveCalibrator.linkSummary + " " +
                                              qsTr("Rates include FEC and packet overhead. Quality is relative to the developer's recommended image bitrate. Every selected target is tested afresh.")
                                    }
                                    Flow {
                                        width: parent.width
                                        spacing: 12
                                        Repeater {
                                            model: [
                                                { tier: "any", text: qsTr("Any display") },
                                                { tier: "vrr", text: qsTr("Needs VRR") },
                                                { tier: "vrrLarge", text: qsTr("VRR · Smooth") },
                                                { tier: "slow", text: qsTr("Can't keep up") }
                                            ]
                                            delegate: Row {
                                                spacing: 5
                                                Rectangle {
                                                    width: 8; height: 8; radius: 4
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    color: calibrationDialog.tierColors[modelData.tier]
                                                }
                                                Label { text: modelData.text; font.pointSize: 9 }
                                            }
                                        }
                                    }
                                    Repeater {
                                        model: calibrationDetails.checked ? [
                                            { tier: "any", text: qsTr("Any display: 99% of frames use at most 60% of each frame, leaving room for a live stream's extra work with or without VRR.") },
                                            { tier: "vrr", text: qsTr("Needs VRR: slow frames use up to 80% of each frame. VRR hides the occasional late one; a fixed-refresh display may stutter.") },
                                            { tier: "vrrLarge", text: qsTr("Needs VRR · Smooth mode: slow frames use nearly the whole frame. Only the Smooth VRR latency mode's larger buffer hides them.") },
                                            { tier: "slow", text: qsTr("Can't keep up: at %1 FPS, more than 1 frame in 100 takes longer than a frame to decode and draw. Expect stutter; it can still be selected.").arg(calibrationDialog.testFps) }
                                        ] : []
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
                                        visible: calibrationDetails.checked
                                        text: qsTr("Reduced or low quality means the image bitrate is below the developer's recommendation. This can follow your chosen target, network limits, or device limits.")
                                    }



                                    Repeater {
                                        id: calibrationRows
                                        model: calibrationDialog.rows
                                        delegate: Row {
                                            id: calibrationRow
                                            width: calibrationTable.width
                                            spacing: 6
                                            readonly property int rowIndex: index
                                            readonly property var rowData: modelData
                                            property alias columns: calibrationChroma

                                            Label {
                                                width: 72
                                                text: calibrationRow.rowData.name
                                                anchors.verticalCenter: parent.verticalCenter
                                            }

                                            Repeater {
                                                id: calibrationChroma
                                                // 4:4:4 then 4:2:0, each HDR then SDR
                                                model: [[0, 1], [2, 3]]
                                                delegate: Column {
                                                    id: chromaColumn
                                                    readonly property var modes: modelData
                                                    property alias formats: calibrationFormats
                                                    width: (calibrationTable.width - 84) / 2
                                                    spacing: 2

                                                    Repeater {
                                                        id: calibrationFormats
                                                        model: chromaColumn.modes
                                                        delegate: CalibrationButton {
                                                            id: optionButton
                                                            objectName: "pyrowaveCalibrationOption" + (calibrationRow.rowIndex * 4 + modelData)
                                                            width: parent.width
                                                            height: 44
                                                            padding: 6
                                                            topPadding: 4
                                                            bottomPadding: 4
                                                            topInset: 0
                                                            bottomInset: 0
                                                            leftInset: 0
                                                            rightInset: 0
                                                            readonly property var option: calibrationDialog.sample(calibrationRow.rowIndex, modelData)
                                                            readonly property bool hdr: modelData % 2 === 0
                                                            enabled: calibrationDialog.canApply(option)
                                                            Accessible.name: calibrationDialog.headline(option, hdr)
                                                            Accessible.description: option && option.valid ? calibrationDialog.frameCost(option) : ""
                                                            ToolTip.delay: 400
                                                            ToolTip.visible: hovered && ToolTip.text !== ""
                                                            ToolTip.text: calibrationDialog.optionDetail(option)
                                                            onClicked: calibrationDialog.applyChoice(option)
                                                            Keys.onPressed: function(event) {
                                                                var mode = modelData
                                                                var row = calibrationRow.rowIndex
                                                                if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                                                                    calibrationDialog.focusOption(row, event.key === Qt.Key_Left ? mode % 2 : 2 + mode % 2, 0)
                                                                }
                                                                else if (event.key === Qt.Key_Up || event.key === Qt.Key_Backtab ||
                                                                         (event.key === Qt.Key_Tab && (event.modifiers & Qt.ShiftModifier))) {
                                                                    calibrationDialog.focusOption(mode % 2 ? row : row - 1,
                                                                        mode % 2 ? mode - 1 : mode + 1, -1)
                                                                }
                                                                else if (event.key === Qt.Key_Down || event.key === Qt.Key_Tab) {
                                                                    calibrationDialog.focusOption(mode % 2 ? row + 1 : row,
                                                                        mode % 2 ? mode - 1 : mode + 1, 1)
                                                                }
                                                                else return
                                                                event.accepted = true
                                                            }

                                                            contentItem: Column {
                                                                spacing: 2
                                                                opacity: !optionButton.enabled ? 0.6 : optionButton.activeFocus || optionButton.hovered ? 1.0 : optionButton.option && !optionButton.option.keepsUp ? 0.7 : 1.0

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
                                                                        width: Math.min(implicitWidth, optionButton.width - 30)
                                                                        elide: Text.ElideRight
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
                            if (thisWm === StreamingPreferences.recommendedFullScreenMode) {
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
                                          StreamingPreferences.windowMode
                        for (var i = 0; i < model.count; i++) {
                             var thisWm = model.get(i).val;
                             if (savedWm === thisWm) {
                                 currentIndex = i
                                 break
                             }
                        }

                        if (!vrrForced) {
                            activated(currentIndex)
                        }

                        // VRR skips activation to preserve the saved mode, but
                        // the disabled control still needs its text measured.
                        recalculateWidth()
                    }

                    Component.onCompleted: {
                        reinitialize()
                        languageChanged.connect(reinitialize)
                    }

                    id: windowModeComboBox
                    property bool vrrForced: StreamingPreferences.enableVsync && StreamingPreferences.enableVrr
                    onVrrForcedChanged: reinitialize()
                    visible: SystemProperties.hasDesktopEnvironment
                    enabled: !SystemProperties.rendererAlwaysFullScreen && !vrrForced
                    hoverEnabled: true
                    textRole: "text"
                    onActivated: {
                        StreamingPreferences.windowMode = model.get(currentIndex).val
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
                        hoverEnabled: true
                        text: qsTr("V-Sync")
                        font.pointSize:  12
                        checked: StreamingPreferences.enableVsync
                        onCheckedChanged: {
                            StreamingPreferences.enableVsync = checked
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
                        enabled: StreamingPreferences.enableVsync
                        checked: StreamingPreferences.enableVsync && StreamingPreferences.framePacing
                        onCheckedChanged: {
                            StreamingPreferences.framePacing = checked
                        }
                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Frame pacing reduces micro-stutter by delaying frames that come in too early")
                    }

                    CheckBox {
                        hoverEnabled: true
                        text: qsTr("VRR")
                        font.pointSize: 12
                        enabled: StreamingPreferences.enableVsync
                        checked: StreamingPreferences.enableVrr
                        onCheckedChanged: {
                            StreamingPreferences.enableVrr = checked
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

                VrrTimingSettings {
                    width: parent.width
                    visible: StreamingPreferences.enableVrr
                    enabled: StreamingPreferences.enableVsync && StreamingPreferences.enableVrr
                    bufferPerMille: StreamingPreferences.vrrBufferPerMille
                    targetHundredths: StreamingPreferences.vrrTargetHundredths
                    historySeconds: StreamingPreferences.vrrHistorySeconds
                    toleranceUs: StreamingPreferences.vrrToleranceUs
                    onPresetPicked: function(mode) { StreamingPreferences.applyVrrPreset(mode) }
                    onBufferEdited: function(value) { StreamingPreferences.vrrBufferPerMille = value }
                    onTargetEdited: function(value) { StreamingPreferences.vrrTargetHundredths = value }
                    onHistoryEdited: function(value) { StreamingPreferences.vrrHistorySeconds = value }
                    onToleranceEdited: function(value) { StreamingPreferences.vrrToleranceUs = value }
                }

                CheckBox {
                    hoverEnabled: true
                    text: qsTr("Reduce judder")
                    font.pointSize: 12
                    visible: StreamingPreferences.enableVrr
                    enabled: StreamingPreferences.enableVsync && StreamingPreferences.enableVrr
                    checked: StreamingPreferences.smoothVrrFrameTiming
                    onCheckedChanged: StreamingPreferences.smoothVrrFrameTiming = checked

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Evens out when frames are displayed, including games whose frame rate does not divide the host display's refresh rate. Adds up to a few milliseconds of delay only while uneven frames need it. Does not blend images or eliminate game stalls.") + "\n\n" +
                                  qsTr("Reconnect the stream after changing this setting.")
                }

                CheckBox {
                    hoverEnabled: true
                    text: qsTr("High-performance GPU power while streaming")
                    font.pointSize: 12
                    visible: Qt.platform.os === "linux"
                    checked: StreamingPreferences.highPerformanceGpuPower
                    onCheckedChanged: StreamingPreferences.highPerformanceGpuPower = checked

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Holds an AMD GPU at its high-performance clocks for the whole stream so it cannot drop its clocks between frames, which can delay when the display shows a frame that was ready on time. Uses more power and reduces battery life. Has no effect if GPU clocks are already set manually.") + "\n\n" +
                                  qsTr("Takes effect at the start of the next stream.")
                }

                CheckBox {
                    id: enableHdr
                    width: parent.width
                    text: qsTr("Enable HDR")
                    font.pointSize: 12

                    enabled: SystemProperties.supportsHdr
                    checked: enabled && StreamingPreferences.enableHdr
                    onCheckedChanged: {
                        if (StreamingPreferences.enableHdr != checked) {
                            StreamingPreferences.enableHdr = checked
                            // PyroWave's default bitrate depends on HDR
                            if (slider.pyroWave && StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = slider.defaultBitrate();
                                slider.value = StreamingPreferences.bitrateKbps
                            }
                        }
                    }

                    // Updating StreamingPreferences.videoCodecConfig is handled above

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
            title: "<font color=\"skyblue\">" + qsTr("VRR diagnostics") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 8

                CheckBox {
                    id: traceVrrFramesCheckBox
                    text: qsTr("Trace VRR frames for debugging")
                    font.pointSize: 12
                    checked: StreamingPreferences.traceVrrFrames
                    onCheckedChanged: StreamingPreferences.traceVrrFrames = checked
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
                        var saved_audio = StreamingPreferences.audioConfig
                        currentIndex = 0
                        for (var i = 0; i < audioListModel.count; i++) {
                            var el_audio = audioListModel.get(i).val;
                            if (saved_audio === el_audio) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
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
                        StreamingPreferences.audioConfig = audioListModel.get(currentIndex).val
                    }
                }


                CheckBox {
                    id: audioPcCheck
                    width: parent.width
                    text: qsTr("Mute host PC speakers while streaming")
                    font.pointSize: 12
                    checked: !StreamingPreferences.playAudioOnHost
                    onCheckedChanged: {
                        StreamingPreferences.playAudioOnHost = !checked
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
                    checked: StreamingPreferences.muteOnFocusLoss
                    onCheckedChanged: {
                        StreamingPreferences.muteOnFocusLoss = checked
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
                    checked: StreamingPreferences.gameOptimizations
                    onCheckedChanged: {
                        StreamingPreferences.gameOptimizations = checked
                    }
                }

                CheckBox {
                    id: quitAppAfter
                    width: parent.width
                    text: qsTr("Quit app on host PC after ending stream")
                    font.pointSize: 12
                    checked: StreamingPreferences.quitAppAfter
                    onCheckedChanged: {
                        StreamingPreferences.quitAppAfter = checked
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
                    text: qsTr("Language")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_language = StreamingPreferences.language
                        currentIndex = 0
                        for (var i = 0; i < languageListModel.count; i++) {
                            var el_language = languageListModel.get(i).val;
                            if (saved_language === el_language) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: languageComboBox
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
                        if (StreamingPreferences.language !== new_language) {
                            StreamingPreferences.language = languageListModel.get(currentIndex).val
                            if (!StreamingPreferences.retranslate()) {
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
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        var saved_uidisplaymode = StreamingPreferences.uiDisplayMode
                        currentIndex = 0
                        for (var i = 0; i < uiDisplayModeListModel.count; i++) {
                            var el_uidisplaymode = uiDisplayModeListModel.get(i).val;
                            if (saved_uidisplaymode === el_uidisplaymode) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: uiDisplayModeComboBox
                    visible: SystemProperties.hasDesktopEnvironment
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
                        StreamingPreferences.uiDisplayMode = uiDisplayModeListModel.get(currentIndex).val
                    }
                }

                CheckBox {
                    id: connectionWarningsCheck
                    width: parent.width
                    text: qsTr("Show connection quality warnings")
                    font.pointSize: 12
                    checked: StreamingPreferences.connectionWarnings
                    onCheckedChanged: {
                        StreamingPreferences.connectionWarnings = checked
                    }
                }

                CheckBox {
                    id: configurationWarningsCheck
                    width: parent.width
                    text: qsTr("Show configuration warnings")
                    font.pointSize: 12
                    checked: StreamingPreferences.configurationWarnings
                    onCheckedChanged: {
                        StreamingPreferences.configurationWarnings = checked
                    }
                }

                CheckBox {
                    visible: SystemProperties.hasDiscordIntegration
                    id: discordPresenceCheck
                    width: parent.width
                    text: qsTr("Discord Rich Presence integration")
                    font.pointSize: 12
                    checked: StreamingPreferences.richPresence
                    onCheckedChanged: {
                        StreamingPreferences.richPresence = checked
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
                    checked: StreamingPreferences.keepAwake
                    onCheckedChanged: {
                        StreamingPreferences.keepAwake = checked
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
        anchors.left: settingsColumn1.right
        id: settingsColumn2
        width: settingsPage.width / 2
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
                    checked: StreamingPreferences.absoluteMouseMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteMouseMode = checked
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
                        hoverEnabled: true
                        text: qsTr("Capture system keyboard shortcuts")
                        font.pointSize: 12
                        enabled: SystemProperties.hasDesktopEnvironment
                        checked: StreamingPreferences.captureSysKeysMode !== StreamingPreferences.CSK_OFF || !SystemProperties.hasDesktopEnvironment

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

                            var saved_syskeysmode = StreamingPreferences.captureSysKeysMode
                            currentIndex = 0
                            for (var i = 0; i < captureSysKeysModeListModel.count; i++) {
                                var el_syskeysmode = captureSysKeysModeListModel.get(i).val;
                                if (saved_syskeysmode === el_syskeysmode) {
                                    currentIndex = i
                                    break
                                }
                            }

                            activated(currentIndex)
                        }

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
                                StreamingPreferences.captureSysKeysMode = StreamingPreferences.CSK_OFF
                            }
                            else {
                                StreamingPreferences.captureSysKeysMode = captureSysKeysModeListModel.get(currentIndex).val
                            }
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated: {
                            updatePref()
                        }

                        // This handles transition of the checkbox state
                        onEnabledChanged: {
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
                    checked: !StreamingPreferences.absoluteTouchMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteTouchMode = !checked
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
                    checked: StreamingPreferences.swapMouseButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapMouseButtons = checked
                    }
                }

                CheckBox {
                    id: reverseScrollButtonsCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Reverse mouse scrolling direction")
                    font.pointSize: 12
                    checked: StreamingPreferences.reverseScrollDirection
                    onCheckedChanged: {
                        StreamingPreferences.reverseScrollDirection = checked
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
                    checked: StreamingPreferences.swapFaceButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapFaceButtons = checked
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
                    checked: !StreamingPreferences.multiController
                    onCheckedChanged: {
                        StreamingPreferences.multiController = !checked
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
                    checked: StreamingPreferences.gamepadMouse
                    onCheckedChanged: {
                        StreamingPreferences.gamepadMouse = checked
                    }
                }

                CheckBox {
                    id: backgroundGamepadCheck
                    width: parent.width
                    text: qsTr("Process gamepad input when Moonlight is in the background")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.backgroundGamepad
                    onCheckedChanged: {
                        StreamingPreferences.backgroundGamepad = checked
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
                        var saved_vds = StreamingPreferences.videoDecoderSelection
                        currentIndex = 0
                        for (var i = 0; i < decoderListModel.count; i++) {
                            var el_vds = decoderListModel.get(i).val;
                            if (saved_vds === el_vds) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
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
                            StreamingPreferences.videoDecoderSelection = decoderListModel.get(currentIndex).val
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
                        var saved_rs = StreamingPreferences.rendererSelection

                        // Default to Automatic
                        currentIndex = 0

                        for(var i = 0; i < rendererListModel.count; i++) {
                            var el_rs = rendererListModel.get(i).val;
                            if (saved_rs === el_rs) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
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
                        StreamingPreferences.rendererSelection = rendererListModel.get(currentIndex).val
                    }
                }

                CheckBox {
                    id: unlockBitrate
                    width: parent.width
                    text: qsTr("Unlock bitrate limit (Experimental)")
                    font.pointSize: 12

                    checked: StreamingPreferences.unlockBitrate
                    onCheckedChanged: {
                        StreamingPreferences.unlockBitrate = checked
                        StreamingPreferences.bitrateKbps = Math.min(StreamingPreferences.bitrateKbps, slider.to)
                        slider.value = StreamingPreferences.bitrateKbps
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This unlocks extremely high video bitrates for use with Sunshine hosts. It should only be used when streaming over an Ethernet LAN connection.")
                }

                CheckBox {
                    id: enableMdns
                    width: parent.width
                    text: qsTr("Automatically find PCs on the local network (Recommended)")
                    font.pointSize: 12
                    checked: StreamingPreferences.enableMdns
                    onCheckedChanged: {
                        // This is called on init, so only do the work if we've
                        // actually changed the value.
                        if (StreamingPreferences.enableMdns != checked) {
                            StreamingPreferences.enableMdns = checked

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
                    width: parent.width
                    text: qsTr("Automatically detect blocked connections (Recommended)")
                    font.pointSize: 12
                    checked: StreamingPreferences.detectNetworkBlocking
                    onCheckedChanged: {
                        StreamingPreferences.detectNetworkBlocking = checked
                    }
                }

                CheckBox {
                    id: showPerformanceOverlay
                    width: parent.width
                    text: qsTr("Show performance stats while streaming")
                    font.pointSize: 12
                    checked: StreamingPreferences.showPerformanceOverlay
                    onCheckedChanged: {
                        StreamingPreferences.showPerformanceOverlay = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Display real-time stream performance information while streaming.") + "\n\n" +
                                  qsTr("You can toggle it at any time while streaming using Ctrl+Alt+Shift+S or Select+L1+R1+X.") + "\n\n" +
                                  qsTr("The performance overlay is not supported on Steam Link or Raspberry Pi.")
                }

                CheckBox {
                    id: showFrametimeGraph
                    width: parent.width
                    text: qsTr("Show frametime graph while streaming")
                    font.pointSize: 12
                    checked: StreamingPreferences.showFrametimeGraph
                    onCheckedChanged: {
                        StreamingPreferences.showFrametimeGraph = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Shows planned, submitted and displayed frame intervals in separate lanes during VRR streaming, so you can see where a stutter starts. Variation under 1 ms is drawn flat.") + "\n\n" +
                                  qsTr("You can toggle it at any time while streaming using Ctrl+Alt+Shift+F or Select+L1+R1+Y (Triangle).")
                }
            }
        }
    }
}
