import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Controls.Material 2.2

import ProfileManager 1.0
import SdlGamepadKeyNavigation 1.0

FocusScope {
    id: profileRoot

    property bool suppressPolling: true
    objectName: qsTr("Profiles")
    focus: true
    activeFocusOnTab: true

    property var profileList: []

    function rebuildProfileList() {
        var list = []
        var profiles = ProfileManager.profiles
        for (var i = 0; i < profiles.length; i++) {
            list.push(profiles[i])
        }
        list.push({ isAddProfile: true })
        profileList = list
    }

    Component.onCompleted: rebuildProfileList()

    Connections {
        target: ProfileManager
        function onProfilesChanged() {
            profileRoot.rebuildProfileList()
        }
    }

    Timer {
        id: focusGridTimer
        interval: 0
        repeat: false
        onTriggered: {
            if (!editProfileDialog.visible && !deleteProfileDialog.visible && !errorDialog.visible) {
                profileRoot.focusCurrentGridItem()
            }
        }
    }

    function gridColumns() {
        return Math.max(1, Math.floor(profileGrid.itemsPerRow))
    }

    function hasGridItemBelow(itemIndex) {
        return itemIndex + gridColumns() < profileGrid.count
    }

    function footerFocusTarget() {
        return autoLoginButton.visible && autoLoginButton.enabled ? autoLoginButton : null
    }

    function focusCurrentGridItem() {
        if (profileRoot.StackView.status !== StackView.Active ||
                stackView.currentItem !== profileRoot) {
            return
        }

        if (profileGrid.currentItem) {
            profileGrid.currentItem.forceActiveFocus(Qt.TabFocusReason)
        }
        else {
            profileGrid.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    function restoreGridFocusSoon() {
        focusGridTimer.restart()
    }

    function toggleDefaultAutoLogin() {
        ProfileManager.setAutoLoginEnabled(!ProfileManager.autoLoginEnabled)
        restoreGridFocusSoon()
    }

    function activateProfile(profileId) {
        if (stackView.busy ||
                profileRoot.StackView.status !== StackView.Active ||
                stackView.currentItem !== profileRoot) {
            return
        }

        if (!window.enterProfile(profileId)) {
            errorDialog.text = qsTr("Unable to activate this profile.")
            errorDialog.open()
        }
    }

    StackView.onActivated: {
        if (profileGrid.currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            profileGrid.currentIndex = 0
        }
        focusGridTimer.restart()
    }

    StackView.onDeactivating: focusGridTimer.stop()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 18

        Label {
            text: qsTr("Choose a profile")
            font.pointSize: 30
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
        }

        CenteredGridView {
            id: profileGrid

            Layout.fillWidth: true
            Layout.fillHeight: true
            cellWidth: 260
            cellHeight: 280
            minMargin: 20
            focus: true
            activeFocusOnTab: true

            model: profileList

            Component.onCompleted: {
                currentIndex = ProfileManager.profiles.length > 0 ? 0 : -1
                focusGridTimer.restart()
            }

            delegate: NavigableItemDelegate {
                id: profileDelegate

                property var profile: modelData
                property alias profileContextMenu: profileContextMenuLoader.item
                property bool isAddCard: modelData.isAddProfile !== undefined && modelData.isAddProfile

                width: 250
                height: 260
                grid: profileGrid

                Rectangle {
                    id: avatar
                    width: 140
                    height: 140
                    radius: 70
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 16
                    color: isAddCard ? Qt.rgba(Material.accent.r, Material.accent.g, Material.accent.b, 0.3) : Material.accent
                    border.color: isAddCard ? Material.accent : "transparent"
                    border.width: isAddCard ? 3 : 0

                    Label {
                        anchors.centerIn: parent
                        text: isAddCard ? "+" : (profile.name.length > 0 ? profile.name.charAt(0).toUpperCase() : "?")
                        font.pointSize: 54
                        font.bold: true
                        color: isAddCard ? Material.accent : "white"
                    }
                }

                Label {
                    id: profileNameLabel
                    text: isAddCard ? qsTr("Add Profile") : profile.name
                    width: parent.width
                    anchors.top: avatar.bottom
                    anchors.topMargin: 16
                    horizontalAlignment: Text.AlignHCenter
                    font.pointSize: 24
                    elide: Text.ElideRight
                }

                Label {
                    text: isAddCard ? "" :
                          (profile.id === ProfileManager.activeProfileId ? qsTr("Current") :
                           profile.defaultProfile && profile.autoLogin ? qsTr("Default / Auto-login") :
                           profile.defaultProfile ? qsTr("Default") : "")
                    visible: text.length > 0
                    anchors.top: profileNameLabel.bottom
                    anchors.topMargin: 6
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    font.pointSize: 12
                }

                Loader {
                    id: profileContextMenuLoader
                    active: !isAddCard
                    sourceComponent: NavigableMenu {
                        initiator: profileContextMenuLoader.parent

                        NavigableMenuItem {
                            text: qsTr("Rename Profile")
                            onTriggered: {
                                editProfileDialog.profileId = profile.id
                                editProfileDialog.profileName = profile.name
                                editProfileDialog.open()
                            }
                        }

                        NavigableMenuItem {
                            text: profile.defaultProfile ? qsTr("Default Profile") : qsTr("Set as Default")
                            enabled: !profile.defaultProfile
                            onTriggered: {
                                if (!profile.defaultProfile) {
                                    ProfileManager.setDefaultProfile(profile.id)
                                    profileRoot.restoreGridFocusSoon()
                                }
                            }
                        }

                        NavigableMenuItem {
                            text: qsTr("Delete Profile")
                            visible: ProfileManager.profiles.length > 1 &&
                                     profile.id !== ProfileManager.activeProfileId
                            onTriggered: {
                                deleteProfileDialog.profileId = profile.id
                                deleteProfileDialog.profileName = profile.name
                                deleteProfileDialog.open()
                            }
                        }
                    }
                }

                onClicked: {
                    if (isAddCard) {
                        editProfileDialog.profileId = ""
                        editProfileDialog.profileName = ""
                        editProfileDialog.open()
                        return
                    }
                    activateProfile(profile.id)
                }

                onPressAndHold: {
                    if (isAddCard) {
                        // Right-click on Add card opens create dialog
                        editProfileDialog.profileId = ""
                        editProfileDialog.profileName = ""
                        editProfileDialog.open()
                        return
                    }
                    if (profileContextMenu.popup) {
                        profileContextMenu.popup()
                    }
                    else {
                        profileContextMenu.open()
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.RightButton
                    onClicked: {
                        parent.pressAndHold()
                    }
                }

                Keys.onMenuPressed: {
                    if (!isAddCard) {
                        profileContextMenu.open()
                    }
                }

                Keys.onDeletePressed: {
                    if (!isAddCard && ProfileManager.profiles.length > 1 &&
                            profile.id !== ProfileManager.activeProfileId) {
                        deleteProfileDialog.profileId = profile.id
                        deleteProfileDialog.profileName = profile.name
                        deleteProfileDialog.open()
                    }
                }

                Keys.onDownPressed: {
                    if (profileRoot.hasGridItemBelow(index)) {
                        grid.moveCurrentIndexDown()
                        profileRoot.focusCurrentGridItem()
                    }
                    else {
                        var target = profileRoot.footerFocusTarget()
                        if (target) {
                            target.forceActiveFocus(Qt.TabFocusReason)
                        }
                        else {
                            profileRoot.focusCurrentGridItem()
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 12

            Button {
                id: autoLoginButton
                activeFocusOnTab: true
                visible: ProfileManager.defaultProfileId.length > 0
                enabled: true
                text: ProfileManager.autoLoginEnabled ? qsTr("Disable Default Auto-login") : qsTr("Enable Default Auto-login")
                onClicked: profileRoot.toggleDefaultAutoLogin()

                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()

                Keys.onUpPressed: {
                    profileRoot.focusCurrentGridItem()
                }
            }

        }
    }

    NavigableDialog {
        id: editProfileDialog

        property string profileId
        property string profileName

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            nameField.text = profileName
            nameField.forceActiveFocus()
            nameField.selectAll()
        }

        onClosed: profileRoot.restoreGridFocusSoon()

        onAccepted: {
            var newName = nameField.text.trim()
            var ok

            if (profileId.length === 0) {
                ok = ProfileManager.createProfile(newName).length > 0
            }
            else {
                ok = ProfileManager.renameProfile(profileId, newName)
            }

            if (!ok) {
                errorDialog.text = qsTr("Profile names must be unique and cannot be empty.")
                errorDialog.open()
            }
        }

        ColumnLayout {
            Label {
                text: editProfileDialog.profileId.length === 0 ? qsTr("New profile name:") : qsTr("Profile name:")
                font.bold: true
            }

            TextField {
                id: nameField
                Layout.fillWidth: true

                Keys.onReturnPressed: editProfileDialog.accept()
                Keys.onEnterPressed: editProfileDialog.accept()
            }
        }
    }

    NavigableMessageDialog {
        id: deleteProfileDialog

        property string profileId
        property string profileName

        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Delete profile '%1'? This removes its paired hosts, settings, and identity.").arg(profileName)

        onClosed: profileRoot.restoreGridFocusSoon()

        onAccepted: {
            if (!ProfileManager.removeProfile(profileId)) {
                errorDialog.text = qsTr("Unable to delete this profile.")
                errorDialog.open()
            }
        }
    }

    ErrorMessageDialog {
        id: errorDialog
        helpText: ""
        onClosed: profileRoot.restoreGridFocusSoon()
    }
}
