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
    property bool customBitrate: false
    readonly property int fieldHeight: Style.px(48, hostWindow)
    readonly property int fieldPadding: Style.px(6, hostWindow)
    readonly property string globalBitrate: qsTr("Global · %1 Mbps").arg(StreamingPreferences.bitrateKbps / 1000)
    readonly property bool customResolution: resolution.currentIndex === resolutionChoices.length - 1
    readonly property bool customFps: frameRate.currentIndex === fpsChoices.length - 1
    readonly property bool bitrateValid: bitrate.acceptableInput && isFinite(Number(bitrate.text))
    readonly property bool valid: (!customResolution || (customWidth.acceptableInput && customHeight.acceptableInput)) &&
                                  (!customFps || customRate.acceptableInput) &&
                                  (!customBitrate || bitrateValid)

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
        customBitrate = saved.bitrateKbps !== undefined
        bitrate.text = String((customBitrate ? saved.bitrateKbps : global.bitrateKbps) / 1000)
        hdr.currentIndex = saved.hdr === undefined ? 0 : saved.hdr ? 1 : 2
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
        if (customBitrate) overrides.bitrateKbps = Math.round(Number(bitrate.text) * 1000)
        if (hdr.currentIndex > 0) overrides.hdr = hdr.currentIndex === 1
        StreamingPreferences.saveAppVideoOverrides(hostUuid, appId, overrides)
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
                    Accessible.name: qsTr("Resolution")
                }
                RowLayout {
                    Layout.fillWidth: true; visible: profile.customResolution
                    ConsoleTextField {
                        id: customWidth; objectName: "gameProfileWidth"; Layout.fillWidth: true; Accessible.name: qsTr("Width")
                        Layout.preferredHeight: profile.fieldHeight
                        topPadding: profile.fieldPadding; bottomPadding: profile.fieldPadding
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { locale: "C"; bottom: 256; top: 8192 }
                    }
                    ConsoleText { text: "×" }
                    ConsoleTextField {
                        id: customHeight; Layout.fillWidth: true; Accessible.name: qsTr("Height")
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
                    Accessible.name: qsTr("Frame rate")
                }
                ConsoleTextField {
                    id: customRate; objectName: "gameProfileCustomRate"; Layout.fillWidth: true
                    Layout.preferredHeight: profile.fieldHeight
                    topPadding: profile.fieldPadding; bottomPadding: profile.fieldPadding
                    visible: profile.customFps; Accessible.name: qsTr("Custom frame rate")
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { locale: "C"; bottom: 10; top: 9999 }
                }
            }
            ConsoleText { text: qsTr("Bitrate (Mbps)") }
            RowLayout {
                Layout.fillWidth: true
                spacing: Style.px(10, profile.hostWindow)
                ConsoleText {
                    Layout.fillWidth: true
                    visible: !profile.customBitrate
                    leftPadding: Style.px(16, profile.hostWindow)
                    text: profile.globalBitrate
                    wrapMode: Text.Wrap
                }
                ConsoleTextField {
                    id: bitrate; objectName: "gameProfileBitrate"; Layout.fillWidth: true
                    visible: profile.customBitrate
                    Layout.preferredHeight: profile.fieldHeight
                    topPadding: profile.fieldPadding; bottomPadding: profile.fieldPadding
                    Accessible.name: qsTr("Bitrate in Mbps")
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    validator: DoubleValidator { locale: "C"; bottom: 0.5; top: 3000; decimals: 3; notation: DoubleValidator.StandardNotation }
                }
                ConsoleButton {
                    objectName: "gameProfileBitrateMode"
                    text: profile.customBitrate ? qsTr("Use global") : qsTr("Set custom")
                    onClicked: {
                        profile.customBitrate = !profile.customBitrate
                        if (profile.customBitrate) bitrate.forceActiveFocus()
                    }
                }
            }
            ConsoleText { text: qsTr("HDR") }
            AutoResizingComboBox {
                id: hdr; objectName: "gameProfileHdr"; Layout.fillWidth: true
                Accessible.name: qsTr("HDR")
                model: [StreamingPreferences.enableHdr ? qsTr("Global · On") : qsTr("Global · Off"), qsTr("On"), qsTr("Off")]
            }
            ConsoleText {
                Layout.columnSpan: 2; Layout.fillWidth: true; role: "caption"; wrapMode: Text.Wrap
                text: profile.customBitrate && !profile.bitrateValid ? qsTr("Enter a bitrate between 0.5 and 3000 Mbps.") :
                    qsTr("Changes apply the next time you connect.")
            }
        }
    }
}
