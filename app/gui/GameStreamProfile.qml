import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import StreamingPreferences 1.0
import SystemProperties 1.0
import "ConsoleStyle.js" as Style

NavigableDialog {
    id: profile
    objectName: "gameStreamProfile"
    title: qsTr("Game stream profile")
    width: Math.min(Style.px(650, hostWindow), hostWindow ? hostWindow.width - Style.px(32, hostWindow) : 650)
    height: Math.min(implicitHeight, hostWindow ? hostWindow.height - Style.px(24, hostWindow) : implicitHeight)
    standardButtons: Dialog.Save | Dialog.Cancel
    closePolicy: Popup.CloseOnEscape
    focus: true
    property string hostUuid
    property int appId
    property string appName
    property var resolutionChoices: []
    property var fpsChoices: []
    property var bitrateOverride
    property var hdrOverride
    readonly property int effectiveBitrate: bitrateOverride === undefined ? StreamingPreferences.bitrateKbps : bitrateOverride
    readonly property int fieldHeight: Style.px(48, hostWindow)
    readonly property int fieldPadding: Style.px(6, hostWindow)
    readonly property bool customResolution: resolution.currentIndex === resolutionChoices.length - 1
    readonly property bool customFps: frameRate.currentIndex === fpsChoices.length - 1
    readonly property bool valid: (!customResolution || (customWidth.acceptableInput && customHeight.acceptableInput)) &&
                                  (!customFps || customRate.acceptableInput)

    function openFor(host, app, name, saved) {
        hostUuid = host
        appId = app
        appName = name

        var global = StreamingPreferences
        var sizes = [null, Qt.size(1280, 720), Qt.size(1920, 1080), Qt.size(1920, 1200),
                     Qt.size(2560, 1440), Qt.size(2560, 1600), Qt.size(3840, 2160), null]
        resolutionChoices = sizes
        resolution.model = sizes.map(function(size, index) {
            if (index === 0) return qsTr("Global · %1 × %2").arg(global.width).arg(global.height)
            return size ? qsTr("%1 × %2").arg(size.width).arg(size.height) : qsTr("Custom")
        })
        var savedSize = saved.resolution
        var match = savedSize ? sizes.findIndex(function(size) {
            return size && size.width === savedSize.width && size.height === savedSize.height
        }) : 0
        resolution.currentIndex = match < 0 ? sizes.length - 1 : match
        customWidth.text = String(savedSize ? savedSize.width : global.width)
        customHeight.text = String(savedSize ? savedSize.height : global.height)

        var refreshRates = []
        for (var i = 0; ; ++i) {
            var refresh = SystemProperties.getRefreshRate(i)
            if (!refresh) break
            refreshRates.push(refresh)
        }
        var rates = [0]
        global.getFpsChoices(refreshRates).forEach(function(choice) {
            if (!choice.is_custom) rates.push(Number(choice.video_fps))
        })
        rates.push(0)
        fpsChoices = rates
        frameRate.model = rates.map(function(rate, index) {
            if (index === 0) return qsTr("Global · %1 FPS").arg(global.fps)
            return rate ? qsTr("%1 FPS").arg(rate) : qsTr("Custom")
        })
        match = saved.fps === undefined ? 0 : rates.indexOf(saved.fps)
        frameRate.currentIndex = match < 0 ? rates.length - 1 : match
        customRate.text = String(saved.fps === undefined ? global.fps : saved.fps)
        bitrateOverride = saved.bitrateKbps
        hdrOverride = saved.hdr
        viewport.contentY = 0
        open()
    }

    function refreshSaveButton() {
        var saveButton = standardButton(Dialog.Save)
        if (saveButton) {
            saveButton.objectName = "gameProfileSave"
            saveButton.enabled = valid
        }
    }
    onOpened: {
        refreshSaveButton()
        resolution.forceActiveFocus()
    }
    onValidChanged: refreshSaveButton()
    onAccepted: {
        if (!valid) return
        var overrides = {}
        if (resolution.currentIndex > 0) {
            overrides.resolution = customResolution ? Qt.size(Number(customWidth.text), Number(customHeight.text)) :
                                                      resolutionChoices[resolution.currentIndex]
        }
        if (frameRate.currentIndex > 0) {
            overrides.fps = customFps ? Number(customRate.text) : fpsChoices[frameRate.currentIndex]
        }
        if (bitrateOverride !== undefined) overrides.bitrateKbps = bitrateOverride
        if (hdrOverride !== undefined) overrides.hdr = hdrOverride
        StreamingPreferences.saveAppVideoOverrides(hostUuid, appId, overrides)
    }

    ConsoleValueEditor {
        id: bitrateEditor
        objectName: "gameProfileBitrateEditor"
        parent: profile.parent
    }

    contentItem: Flickable {
        id: viewport
        objectName: "gameProfileViewport"
        implicitHeight: form.height
        contentHeight: form.height
        clip: true
        ScrollBar.vertical: ScrollBar {
            policy: viewport.contentHeight > viewport.height + 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        }
        Window.onActiveFocusItemChanged: {
            var item = Window.activeFocusItem
            if (!item || !profile.opened) return
            var ancestor = item
            while (ancestor && ancestor !== form) ancestor = ancestor.parent
            if (!ancestor) return
            var y = item.mapToItem(contentItem, 0, 0).y
            if (y < contentY) contentY = y
            else if (y + item.height > contentY + height) contentY = y + item.height - height
        }

        GridLayout {
            id: form
            width: viewport.width - Style.px(12, profile.hostWindow)
            columns: 2
            rowSpacing: Style.px(8, profile.hostWindow)
            columnSpacing: Style.px(16, profile.hostWindow)

            ConsoleText {
                Layout.columnSpan: 2; Layout.fillWidth: true
                role: "secondary"; text: profile.appName; wrapMode: Text.Wrap
            }
            ConsoleText { text: qsTr("Resolution"); Layout.alignment: Qt.AlignTop; Layout.topMargin: Style.px(12, profile.hostWindow) }
            ColumnLayout {
                Layout.fillWidth: true
                AutoResizingComboBox {
                    id: resolution; objectName: "gameProfileResolution"; Layout.fillWidth: true
                    KeyNavigation.tab: profile.customResolution ? customWidth : frameRate
                    KeyNavigation.backtab: profile.standardButton(Dialog.Cancel)
                    Accessible.name: qsTr("Resolution")
                }
                RowLayout {
                    Layout.fillWidth: true; visible: profile.customResolution
                    ConsoleTextField {
                        id: customWidth; objectName: "gameProfileWidth"; Layout.fillWidth: true; Accessible.name: qsTr("Width")
                        KeyNavigation.tab: customHeight
                        KeyNavigation.backtab: resolution
                        Layout.preferredHeight: profile.fieldHeight
                        topPadding: profile.fieldPadding; bottomPadding: profile.fieldPadding
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { locale: "C"; bottom: 256; top: 8192 }
                    }
                    ConsoleText { text: "×" }
                    ConsoleTextField {
                        id: customHeight; Layout.fillWidth: true; Accessible.name: qsTr("Height")
                        KeyNavigation.tab: frameRate
                        KeyNavigation.backtab: customWidth
                        Layout.preferredHeight: profile.fieldHeight
                        topPadding: profile.fieldPadding; bottomPadding: profile.fieldPadding
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { locale: "C"; bottom: 256; top: 8192 }
                    }
                }
            }
            ConsoleText { text: qsTr("Frame rate"); Layout.alignment: Qt.AlignTop; Layout.topMargin: Style.px(12, profile.hostWindow) }
            ColumnLayout {
                Layout.fillWidth: true
                AutoResizingComboBox {
                    id: frameRate; objectName: "gameProfileFrameRate"; Layout.fillWidth: true
                    KeyNavigation.tab: profile.customFps ? customRate : bitrateRow
                    KeyNavigation.backtab: profile.customResolution ? customHeight : resolution
                    Accessible.name: qsTr("Frame rate")
                }
                ConsoleTextField {
                    id: customRate; objectName: "gameProfileCustomRate"; Layout.fillWidth: true
                    KeyNavigation.tab: bitrateRow
                    KeyNavigation.backtab: frameRate
                    Layout.preferredHeight: profile.fieldHeight
                    topPadding: profile.fieldPadding; bottomPadding: profile.fieldPadding
                    visible: profile.customFps; Accessible.name: qsTr("Custom frame rate")
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { locale: "C"; bottom: 10; top: 9999 }
                }
            }
            ConsoleSettingRow {
                id: bitrateRow
                objectName: "gameProfileBitrateRow"
                Layout.columnSpan: 2; Layout.fillWidth: true
                KeyNavigation.backtab: profile.customFps ? customRate : frameRate
                KeyNavigation.tab: resetBitrate.visible ? resetBitrate : hdr
                labelItem: ConsoleText { text: qsTr("Video bitrate") }
                editor: VideoBitrateSlider {
                    id: bitrate
                    objectName: "gameProfileBitrate"
                    value: profile.effectiveBitrate
                    valueText: profile.bitrateOverride === undefined ?
                        qsTr("Global · %1 Mbps").arg(profile.effectiveBitrate / 1000) :
                        qsTr("%1 Mbps").arg(profile.effectiveBitrate / 1000)
                    function openSettingsEditor(row) { bitrateEditor.openFor(row, bitrate, true) }
                    onSettingsCommitted: function(newValue) { profile.bitrateOverride = newValue }
                }
                Keys.onSpacePressed: activateSetting()
                Keys.onReturnPressed: activateSetting()
                Keys.onEnterPressed: activateSetting()
            }
            ConsoleListRow {
                id: resetBitrate
                objectName: "gameProfileResetBitrate"
                Layout.columnSpan: 2; Layout.fillWidth: true
                visible: profile.bitrateOverride !== undefined
                text: qsTr("Use global bitrate (%1 Mbps)").arg(StreamingPreferences.bitrateKbps / 1000)
                KeyNavigation.backtab: bitrateRow
                KeyNavigation.tab: hdr
                onClicked: { profile.bitrateOverride = undefined; bitrateRow.forceActiveFocus() }
            }
            ConsoleCheckBox {
                id: hdr
                objectName: "gameProfileHdr"
                Layout.columnSpan: 2; Layout.fillWidth: true
                text: qsTr("Enable HDR")
                enabled: SystemProperties.supportsHdr
                checked: enabled && (profile.hdrOverride === undefined ? StreamingPreferences.enableHdr : profile.hdrOverride)
                description: !enabled ? qsTr("HDR streaming is not supported on this PC.") :
                    profile.hdrOverride === undefined ? qsTr("Using the global setting") : ""
                onToggled: profile.hdrOverride = checked
                KeyNavigation.backtab: resetBitrate.visible ? resetBitrate : bitrateRow
                KeyNavigation.tab: resetHdr.visible ? resetHdr : profile.standardButton(profile.valid ? Dialog.Save : Dialog.Cancel)
            }
            ConsoleListRow {
                id: resetHdr
                objectName: "gameProfileResetHdr"
                Layout.columnSpan: 2; Layout.fillWidth: true
                visible: profile.hdrOverride !== undefined
                text: qsTr("Use global HDR setting")
                KeyNavigation.backtab: hdr
                KeyNavigation.tab: profile.standardButton(profile.valid ? Dialog.Save : Dialog.Cancel)
                onClicked: {
                    profile.hdrOverride = undefined
                    if (hdr.enabled) hdr.forceActiveFocus()
                    else bitrateRow.forceActiveFocus()
                }
            }
            ConsoleText {
                Layout.columnSpan: 2; Layout.fillWidth: true; role: "caption"; wrapMode: Text.Wrap
                text: qsTr("Changes apply the next time you connect.")
            }
        }
    }
}
