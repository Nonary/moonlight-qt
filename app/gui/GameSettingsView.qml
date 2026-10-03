import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

Page {
    id: gameSettingsPage
    property var editor
    property string gameName
    property string hostName
    objectName: qsTr("Streaming Settings: %1 (%2)").arg(gameName).arg(hostName)
    focus: true

    function activate() {
        if (settingsLoader.item) settingsLoader.item.activate()
    }

    function resetSetting(key) {
        // Recreate the existing controls to refresh their selected model indexes.
        // Game-mode controls never persist on destruction.
        settingsLoader.active = false
        editor.reset(key)
        settingsLoader.active = true
        Qt.callLater(function() {
            if (gameSettingsPage.StackView.status === StackView.Active) activate()
        })
    }

    StackView.onActivated: activate()
    StackView.onDeactivating: {
        if (settingsLoader.item) settingsLoader.item.deactivate()
        editor.save()
    }
    Component.onDestruction: {
        if (editor) {
            editor.save()
            editor.dispose()
        }
    }

    Loader {
        id: settingsLoader
        anchors.fill: parent
        sourceComponent: SettingsView {
            preferences: editor.preferences
            gameMode: true
            manageLifecycle: false
        }
    }

    footer: ToolBar {
        padding: 10
        GridLayout {
            width: parent.width
            columns: width < 600 ? 2 : 3
            columnSpacing: 10
            Label {
                visible: customSetting.count === 0
                text: qsTr("Profile settings")
                Layout.fillWidth: true
                Layout.columnSpan: parent.columns === 2 ? 2 : 1
                elide: Text.ElideRight
            }
            AutoResizingComboBox {
                id: customSetting
                objectName: "customSettingSelector"
                model: editor.customSettings
                textRole: "text"
                visible: count > 0
                Layout.fillWidth: true
                Layout.columnSpan: parent.columns === 2 ? 2 : 1
                Layout.minimumWidth: 80
                onCountChanged: {
                    if (currentIndex < 0 || currentIndex >= count) currentIndex = 0
                    recalculateWidth()
                }
                Component.onCompleted: recalculateWidth()
            }
            Button {
                objectName: "resetGameSetting"
                text: qsTr("Use Profile Setting")
                enabled: customSetting.count > 0
                onClicked: {
                    var setting = editor.customSettings[customSetting.currentIndex]
                    if (setting) resetSetting(setting.key)
                }
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
            }
            Button {
                objectName: "resetAllGameSettings"
                text: qsTr("Reset All")
                enabled: customSetting.count > 0
                onClicked: resetDialog.open()
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
            }
        }
    }

    NavigableMessageDialog {
        id: resetDialog
        text: qsTr("Remove all custom streaming settings for %1?").arg(gameName)
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: resetSetting("")
        onClosed: Qt.callLater(function() {
            if (gameSettingsPage.StackView.status === StackView.Active) activate()
        })
    }
}
