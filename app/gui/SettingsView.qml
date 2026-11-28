import QtQuick 2.12
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2

import StreamingPreferences 1.0
import AutoUpdateChecker 1.0
import ComputerManager 1.0
import ComputerModel 1.0
import SdlGamepadKeyNavigation 1.0
import SystemProperties 1.0
import PyroWaveCalibrator 1.0
import NetworkBuffers 1.0
import "ConsoleStyle.js" as Style
import QtQuick.Controls.Material 2.2

FocusScope {
    id: settingsShell
    objectName: qsTr("Settings")
    property int category: 0
    readonly property real uiScale: Style.windowScale(Window.window)
    readonly property bool categoryMenuFocused: categoryList.activeFocus
    readonly property string primaryVerb: categoryMenuFocused ? qsTr("Open") : activeRow && activeRow.disabledSettingProxy === true ? qsTr("Help") : activeRow && activeRow.settingsAction === true ? qsTr("Select") : qsTr("Change")
    readonly property bool optionsAvailable: !categoryMenuFocused && !!activeRow && !!activeRow.settingsHelp
    readonly property bool helpDialogOnly: optionsAvailable && activeRow.helpDialogOnly === true
    property var activeRow: null
    property var rememberedRows: ({})
    property var categoryRows: []
    // Select an input presentation on entry. Opening a dropdown or moving a
    // pointer across a controller session must not rearrange the focused form.
    property bool desktopPreferences: false
    readonly property int formMargin: width < 1000 ? 12 : 24
    readonly property bool searching: desktopPreferences && searchField.text.trim().length > 0
    property bool indexing: false
    property var catalog: []
    property var categoryNames: [qsTr("Video"), qsTr("Audio"), qsTr("Input & Controllers"), qsTr("Host"), qsTr("Interface"), qsTr("Diagnostics"), qsTr("Advanced"), qsTr("About")]
    property Item hoverRow
    function choosePresentation() {
        desktopPreferences = SystemProperties.hasDesktopEnvironment &&
                (SdlGamepadKeyNavigation.connectedGamepads === 0 ||
                 (SdlGamepadKeyNavigation.inputMethod !== "controller" && SdlGamepadKeyNavigation.inputMethod !== "touch"))
    }
    function buildSearchCatalog() {
        // Sample the live controls in one pass before painting. Category is
        // unchanged; conditional sections still obey device and codec availability.
        indexing = true
        var rows = settingsFocusItems()
        var result = []
        for (var i = 0; i < rows.length; ++i) {
            var row = rows[i].disabledSettingProxy === true ? rows[i].target : rows[i]
            var group = row
            while (group && group.settingsCategory === undefined) group = group.parent
            if (group) result.push({ row: row, label: row.settingsLabel, help: row.settingsHelp,
                                     category: group.settingsCategory, group: categoryNames[group.settingsCategory] })
        }
        indexing = false
        catalog = result
    }
    function jumpTo(entry) {
        searchField.clear()
        category = entry.category
        Qt.callLater(function() {
            var rows = settingsFocusItems()
            var target = rows.indexOf(entry.row) >= 0 ? entry.row : entry.row.disabledSettingsProxy
            if (rows.indexOf(target) >= 0) {
                focusRow(target)
                Qt.callLater(reflowAndReveal)
            } else focusContent()
        })
    }
    function searchFocus() { searchField.forceActiveFocus(Qt.ShortcutFocusReason); searchField.selectAll() }
    function showHelp(row) {
        if (!row || !row.settingsHelp) return
        helpDialog.subject = row
        helpDialog.open()
    }
    Material.theme: Material.Dark
    Material.accent: Style.accent
    Material.background: Style.surface
    Material.foreground: Style.text
    signal languageChanged()
    focus: true
    function focusContent() {
        var items = settingsFocusItems()
        var remembered = rememberedRows[category]
        if (items.length) focusRow(items.indexOf(remembered) >= 0 ? remembered : items[0])
    }
    function prepareRows(item) {
        // Configure every row, including hidden and disabled rows, before
        // filtering navigation. Late-appearing settings must have the same UI.
        if (item.disabledSettingProxy === true) return
        if (item.settingsEntry === true) {
            if (item.settingsNavigation !== settingsShell) {
                item.settingsNavigation = settingsShell
                item.disabledSettingsProxy = unavailableRow.createObject(settingsPage.contentItem, { target: item })
                if (!item.editor) item.width = Qt.binding(function() { return item.parent.width })
            }
            return
        }
        for (var i = 0; i < item.children.length; ++i) prepareRows(item.children[i])
    }
    // Rows own focus and are visited in declaration order. Editors and modal
    // content are excluded, so layout and reparenting cannot reverse navigation.
    function settingsFocusItems() {
        prepareRows(settingsPage.contentItem)
        settingsColumn1.forceLayout()
        settingsColumn2.forceLayout()
        var items = []
        function collect(item) {
            if (item.disabledSettingProxy === true || !item.visible) return
            if (item.settingsEntry === true) {
                if (item.editor && !item.editor.visible) return
                if (!item.enabled || item.editor && !item.editor.enabled) {
                    item.disabledSettingsProxy.syncGeometry()
                    items.push(item.disabledSettingsProxy)
                } else items.push(item)
                return
            }
            if (!item.enabled) return
            for (var i = 0; i < item.children.length; ++i) collect(item.children[i])
        }
        collect(settingsPage.contentItem)
        // All proxies are direct siblings of the Columns. Keep even those
        // excluded from navigation aligned so they cannot intercept stale areas.
        var siblings = settingsPage.contentItem.children
        for (var i = 0; i < siblings.length; ++i) {
            if (siblings[i].disabledSettingProxy === true && siblings[i].visible) siblings[i].syncGeometry()
        }
        categoryRows = items
        return items
    }
    // A proxy is outside the settings Columns, so resynchronize it after
    // wrapping, conditional rows, and viewport changes settle.
    function reflowRows() { if (visible) settingsFocusItems() }
    function reflowAndReveal() {
        reflowRows()
        if (activeRow && (activeRow.activeFocus || activeRow.rowFocused === true)) ensureRowVisible(activeRow)
    }
    function rowFocused(row) {
        hoverRow = null
        activeRow = row
        rememberedRows[category] = row
        ensureRowVisible(row)
    }
    function ensureRowVisible(row) {
        if (!row || !row.visible) return
        var pos = row.mapToItem(settingsPage.contentItem, 0, 0)
        var margin = Style.px(8, settingsShell.Window.window)
        var next = settingsPage.contentY
        if (categoryRows.length && categoryRows[0] === row) next = 0
        else if (pos.y < next + margin) next = pos.y - margin
        else if (pos.y + row.height > next + settingsPage.height - margin) next = pos.y + row.height - settingsPage.height + margin
        settingsPage.contentY = Math.max(0, Math.min(next, Math.max(0, settingsPage.contentHeight - settingsPage.height)))
    }
    function focusRow(row) { row.forceActiveFocus(); rowFocused(row) }
    function focusCategories() { categoryList.forceActiveFocus(); SdlGamepadKeyNavigation.setUiNavMode(false) }
    function primaryAction() { if (categoryMenuFocused) focusContent(); else if (activeRow) activateRow(activeRow) }
    function backAction() { focusCategories() }
    function activateRow(row) {
        if (!row) return
        var items = settingsFocusItems()
        var index = items.indexOf(row)
        // Availability can change while a popup or asynchronous action owns
        // focus. Recover to its live row instead of invoking a stale editor.
        if (index < 0) {
            var live = row.disabledSettingProxy === true ? row.target : row.disabledSettingsProxy
            if (live && items.indexOf(live) >= 0) focusRow(live)
            else if (items.length) focusRow(items[0])
            else focusCategories()
            return
        }
        rowFocused(row)
        if (row.disabledSettingProxy === true) { optionsAction(); return }
        row.activateSetting()
        // Actions such as Use Default can remove their own row. Keep focus
        // within the list when that happens instead of falling into the shell.
        Qt.callLater(function() {
            if (!settingsShell.visible || row.visible && row.enabled && (!row.editor || row.editor.enabled)) return
            if (typeof window !== "undefined" && typeof window.popupHasFocus === "function" && window.popupHasFocus()) return
            var items = settingsFocusItems()
            if (items.length) focusRow(items[Math.max(0, Math.min(index, items.length - 1))])
            else focusCategories()
        })
    }
    function openValueEditor(row, control, range) { valueEditor.openFor(row, control, range) }
    function optionsAction() { if (optionsAvailable) showHelp(activeRow) }
    function tabInForm(current, backwards) {
        var stops = []
        var rows = settingsFocusItems()
        for (var i = 0; i < rows.length; ++i) {
            stops.push(rows[i])
            if (rows[i].desktopHelpLink) stops.push(rows[i].desktopHelpLink)
        }
        var next = stops.indexOf(current) + (backwards ? -1 : 1)
        if (next < 0) focusCategories()
        else if (next >= stops.length) doneButton.forceActiveFocus(Qt.TabFocusReason)
        else stops[next].forceActiveFocus(backwards ? Qt.BacktabFocusReason : Qt.TabFocusReason)
    }
    function handleRowKey(row, event) {
        if (desktopPreferences) {
            // Inline spin editors own text and arrow keys while editing.
            if (row.editor && row.editor.desktopEditing === true && event.key !== Qt.Key_Tab && event.key !== Qt.Key_Backtab)
                return false
            if (event.key === Qt.Key_F1) { showHelp(row); return true }
            if (event.key === Qt.Key_Backtab || event.key === Qt.Key_Tab) {
                tabInForm(row, event.key === Qt.Key_Backtab || !!(event.modifiers & Qt.ShiftModifier))
                return true
            } else if (event.key === Qt.Key_Escape && typeof window !== "undefined" && window.showHome) {
                window.showHome(); return true
            }
        }
        switch (event.key) {
        case Qt.Key_Up: case Qt.Key_Backtab: return navigateSettings(-1)
        case Qt.Key_Down: return navigateSettings(1)
        case Qt.Key_Tab: return navigateSettings(event.modifiers & Qt.ShiftModifier ? -1 : 1)
        case Qt.Key_Left: case Qt.Key_Escape: case Qt.Key_Back: focusCategories(); return true
        case Qt.Key_Right: return true
        case Qt.Key_Return: case Qt.Key_Enter: case Qt.Key_Space: activateRow(row); return true
        case Qt.Key_Menu: optionsAction(); return true
        }
        return false
    }
    function navigateSettings(delta) {
        var items = settingsFocusItems()
        var current = Window.activeFocusItem
        while (current && items.indexOf(current) < 0) current = current.parent
        var index = items.indexOf(current)
        if (index < 0) {
            // A busy/hidden action may have relinquished focus since the last
            // frame. Recover inside the list; popup keys are contained by editors.
            var live = activeRow && (activeRow.disabledSettingProxy === true ? activeRow.target : activeRow.disabledSettingsProxy)
            var remembered = Math.max(items.indexOf(activeRow), items.indexOf(live))
            if (items.length) focusRow(items[Math.max(0, remembered)])
            else focusCategories()
            return true
        }
        focusRow(items[Math.max(0, Math.min(items.length - 1, index + delta))])
        return true
    }
    StackView.onActivated: {
        choosePresentation()
        focusCategories()
    }
    StackView.onDeactivating: {
        SdlGamepadKeyNavigation.setUiNavMode(false)
        StreamingPreferences.save()
    }
    onCategoryChanged: { settingsPage.contentY = 0; categoryList.currentIndex = category; hoverRow = null; activeRow = null; Qt.callLater(settingsFocusItems) }
    Component.onCompleted: Qt.callLater(settingsFocusItems)
    // A sibling focus target keeps disabled controls inert while making their
    // explanation reachable. It sits outside Columns so it adds no layout gap.
    Component {
        id: unavailableRow
        Item {
            id: unavailableEntry
            property Item target
            readonly property bool disabledSettingProxy: true
            readonly property bool settingsEntry: true
            readonly property string settingsLabel: target ? target.settingsLabel : ""
            readonly property bool helpDialogOnly: !!target && target.helpDialogOnly === true
            readonly property string settingsHelp: qsTr("Unavailable with the current settings or device.") +
                (target && target.settingsHelp ? "\n" + target.settingsHelp : "")
            visible: !!target && target.visible && (!target.editor || target.editor.visible) && (!target.enabled || !!target.editor && !target.editor.enabled)
            activeFocusOnTab: true
            onVisibleChanged: if (visible && target) Qt.callLater(syncGeometry)
            z: 2
            Accessible.role: Accessible.Button
            Accessible.name: settingsLabel
            Accessible.description: settingsHelp
            Accessible.onPressAction: settingsShell.showHelp(unavailableEntry)
            function syncGeometry() {
                if (!target) return
                var point = target.mapToItem(parent, 0, 0)
                x = point.x; y = point.y; width = target.width; height = target.height
            }
            onActiveFocusChanged: if (activeFocus) { syncGeometry(); settingsShell.rowFocused(this) }
            Keys.onPressed: function(event) { event.accepted = settingsShell.handleRowKey(this, event) }
            ConsoleFocus { focused: parent.activeFocus; radius: Style.px(6, settingsShell.Window.window) }
            MouseArea { anchors.fill: parent; onClicked: { parent.forceActiveFocus(); settingsShell.optionsAction() } }
        }
    }
    ConsoleValueEditor { id: valueEditor }
    NavigableMessageDialog {
        id: helpDialog
        objectName: "settingsHelpDialog"
        property Item subject
        title: subject ? subject.settingsLabel : ""
        text: subject ? subject.settingsHelp : ""
        standardButtons: Dialog.Close
        width: Math.min(Style.px(560, hostWindow), hostWindow ? hostWindow.width - Style.px(48, hostWindow) : 560)
        height: hostWindow ? Math.min(implicitHeight, Math.max(0, hostWindow.height - Style.px(48, hostWindow))) : implicitHeight
        onClosed: Qt.callLater(function() { var rows = settingsShell.settingsFocusItems(); if (rows.indexOf(subject) >= 0) settingsShell.focusRow(subject); else settingsShell.focusContent() })
    }
    ConsoleBackdrop { anchors.fill: parent }
    Rectangle {
        id: form
        visible: settingsShell.desktopPreferences
        x: (parent.width - width) / 2
        y: parent.height >= 650 ? 16 : 4
        width: Math.min(1320, parent.width - 2 * settingsShell.formMargin)
        height: parent.height - 2 * y
        radius: 10
        color: "#191c22"
        border.color: "#383d47"
    }
    Item {
        id: formHeader
        visible: settingsShell.desktopPreferences
        x: form.x + 1; y: form.y + 1
        width: form.width - 2; height: settingsShell.height >= 650 ? 74 : 58
        Label {
            anchors.left: parent.left; anchors.leftMargin: 24; anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Settings"); font.pixelSize: 25; font.weight: Font.DemiBold
        }
        TextField {
            id: searchField
            objectName: "desktopSettingsSearch"
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: doneButton.left; anchors.rightMargin: 14
            width: Math.min(320, parent.width * 0.35); height: 38
            leftPadding: 12; rightPadding: 40
            font.pixelSize: 15; verticalAlignment: Text.AlignVCenter
            Label {
                visible: !searchField.text
                anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Find a setting…"); color: "#b4becb"; font.pixelSize: 15
            }
            Accessible.name: qsTr("Find a setting")
            background: Rectangle { radius: 5; color: "#101319"; border.color: searchField.activeFocus ? "#9bcfff" : "#4a515c" }
            Label {
                anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter
                visible: !searchField.text && !searchField.activeFocus
                text: Qt.platform.os === "osx" ? "⌘F" : "Ctrl+F"; color: "#9ba4af"; font.pixelSize: 12
            }
            onTextChanged: {
                if (text.trim() && !settingsShell.catalog.length) settingsShell.buildSearchCatalog()
                else if (!text.trim()) settingsShell.catalog = []
            }
            Keys.onDownPressed: if (settingsShell.searching) results.step(1); else settingsShell.focusCategories()
            Keys.onUpPressed: if (settingsShell.searching) results.step(-1)
            Keys.onReturnPressed: if (settingsShell.searching) results.choose(); else settingsShell.focusContent()
            Keys.onEnterPressed: if (settingsShell.searching) results.choose(); else settingsShell.focusContent()
            Keys.onEscapePressed: { if (text.length) clear(); else settingsShell.focusCategories() }
            Keys.onTabPressed: { if (settingsShell.searching) results.forceActiveFocus(Qt.TabFocusReason); else settingsShell.focusCategories() }
        }
        Button {
            id: doneButton
            objectName: "desktopSettingsDone"
            anchors.right: parent.right; anchors.rightMargin: 20; anchors.verticalCenter: parent.verticalCenter
            height: 38; width: Math.max(76, implicitWidth); text: qsTr("Done"); font.pixelSize: 15
            background: Rectangle {
                radius: 5; color: doneButton.down ? "#364253" : doneButton.hovered ? "#34404e" : "#292f38"
                border.color: doneButton.visualFocus ? "#9bcfff" : "#4a515c"
            }
            onClicked: if (settingsShell.Window.window && settingsShell.Window.window.showHome) settingsShell.Window.window.showHome()
            Keys.onTabPressed: settingsShell.searchFocus()
            Keys.onBacktabPressed: if (settingsShell.searching) results.forceActiveFocus(Qt.BacktabFocusReason); else if (settingsShell.activeRow) settingsShell.focusRow(settingsShell.activeRow); else settingsShell.focusContent()
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#383d47" }
    }
    Shortcut {
        sequences: [StandardKey.Find]
        enabled: settingsShell.visible && settingsShell.desktopPreferences && (!settingsShell.Window.window.popupHasFocus || !settingsShell.Window.window.popupHasFocus())
        onActivated: settingsShell.searchFocus()
    }
    Rectangle {
        visible: settingsShell.desktopPreferences
        x: form.x + 1; y: formHeader.y + formHeader.height
        width: categoryList.width + 20; height: form.y + form.height - y - 1
        color: "#20242b"
        Rectangle { anchors.right: parent.right; height: parent.height; width: 1; color: "#343942" }
    }
    ListView {
        id: categoryList
        objectName: "settingsCategories"
        anchors.left: parent.left; anchors.leftMargin: settingsShell.desktopPreferences ? form.x + 10 : Style.px(64, settingsShell.Window.window)
        anchors.top: parent.top; anchors.topMargin: settingsShell.desktopPreferences ? formHeader.y + formHeader.height + 14 : Style.px(24, settingsShell.Window.window)
        anchors.bottom: parent.bottom; anchors.bottomMargin: settingsShell.desktopPreferences ? form.y + 10 : 16
        width: settingsShell.desktopPreferences ? (settingsShell.width < 960 ? 164 : 202) : Style.px(260, settingsShell.Window.window)
        spacing: Style.px(4, settingsShell.Window.window); clip: true; focus: true
        model: settingsShell.categoryNames
        currentIndex: settingsShell.category
        delegate: ConsoleListRow {
            width: categoryList.width
            height: settingsShell.desktopPreferences ? 40 : Style.px(48, settingsShell.Window.window)
            font.pixelSize: settingsShell.desktopPreferences ? 15 : Style.typeSize("body", settingsShell.Window.window)
            text: modelData; selected: !settingsShell.searching && settingsShell.category === index; textOnly: true; rail: true; textAlignment: Text.AlignLeft
            Keys.onTabPressed: function(event) { if (settingsShell.desktopPreferences) settingsShell.focusContent(); else categoryList.moveCategory(event.modifiers & Qt.ShiftModifier ? -1 : 1) }
            Keys.onBacktabPressed: if (settingsShell.desktopPreferences) settingsShell.searchFocus(); else categoryList.moveCategory(-1)
            Keys.onUpPressed: categoryList.moveCategory(-1)
            Keys.onDownPressed: categoryList.moveCategory(1)
            Keys.onRightPressed: settingsShell.focusContent()
            Keys.onReturnPressed: settingsShell.focusContent()
            Keys.onEnterPressed: settingsShell.focusContent()
            Keys.onSpacePressed: settingsShell.focusContent()
            onClicked: { searchField.clear(); settingsShell.category = index; categoryList.currentIndex = index; settingsShell.focusCategories() }
        }
        function moveCategory(delta) {
            settingsShell.category = Math.max(0, Math.min(count - 1, settingsShell.category + delta))
            currentIndex = settingsShell.category
            positionViewAtIndex(currentIndex, ListView.Contain)
        }
        Keys.onUpPressed: moveCategory(-1)
        Keys.onDownPressed: moveCategory(1)
        Keys.onBacktabPressed: if (settingsShell.desktopPreferences) settingsShell.searchFocus(); else moveCategory(-1)
        Keys.onTabPressed: function(event) { if (settingsShell.desktopPreferences) settingsShell.focusContent(); else moveCategory(event.modifiers & Qt.ShiftModifier ? -1 : 1) }
        Keys.onRightPressed: settingsShell.focusContent()
        Keys.onReturnPressed: settingsShell.focusContent()
        Keys.onEnterPressed: settingsShell.focusContent()
        Keys.onSpacePressed: settingsShell.focusContent()
    }
    Flickable {
    id: settingsPage
    visible: !settingsShell.searching || settingsShell.indexing
    HoverHandler {
        onPointChanged: {
            if (!hovered || helpDialog.visible) return
            // A Flickable attaches declarative pointer handlers to its content item.
            var viewportPoint = parent.mapToItem(settingsPage, point.position.x, point.position.y)
            if (!settingsPage.contains(viewportPoint)) {
                settingsShell.hoverRow = null
                return
            }
            for (var i = 0; i < settingsShell.categoryRows.length; ++i) {
                var row = settingsShell.categoryRows[i]
                if (row.visible && row.contains(settingsPage.mapToItem(row, viewportPoint.x, viewportPoint.y))) {
                    settingsShell.hoverRow = row
                    return
                }
            }
            settingsShell.hoverRow = null
        }
        onHoveredChanged: if (!hovered) settingsShell.hoverRow = null
    }
    onWidthChanged: Qt.callLater(settingsShell.reflowAndReveal)
    onHeightChanged: Qt.callLater(settingsShell.reflowAndReveal)
    onContentHeightChanged: Qt.callLater(settingsShell.desktopPreferences ? settingsShell.reflowAndReveal : settingsShell.reflowRows)
    anchors.left: categoryList.right; anchors.leftMargin: settingsShell.desktopPreferences ? 28 : Style.px(40, settingsShell.Window.window)
    width: settingsShell.desktopPreferences ? form.x + form.width - x - 20 : Math.min(Style.px(760, settingsShell.Window.window), settingsShell.width - x - Style.px(64, settingsShell.Window.window))
    anchors.top: parent.top; anchors.topMargin: settingsShell.desktopPreferences ? formHeader.y + formHeader.height + 18 : Style.px(24, settingsShell.Window.window)
    anchors.bottom: settingsShell.desktopPreferences ? parent.bottom : helpPanel.top
    anchors.bottomMargin: settingsShell.desktopPreferences ? form.y + 12 : 0
    clip: true
    Keys.onTabPressed: function(event) { event.accepted = settingsShell.navigateSettings(event.modifiers & Qt.ShiftModifier ? -1 : 1) }
    Keys.onBacktabPressed: function(event) { event.accepted = settingsShell.navigateSettings(-1) }
    Keys.onUpPressed: function(event) { event.accepted = settingsShell.navigateSettings(-1) }
    Keys.onDownPressed: function(event) { event.accepted = settingsShell.navigateSettings(1) }
    Keys.onLeftPressed: settingsShell.focusCategories()
    Keys.onMenuPressed: settingsShell.optionsAction()
    Keys.onEscapePressed: function(event) {
        if (typeof window !== "undefined" && typeof window.popupHasFocus === "function" && window.popupHasFocus()) { event.accepted = false; return }
        categoryList.forceActiveFocus()
    }
    Keys.onBackPressed: categoryList.forceActiveFocus()
    ComputerModel {
        id: calibrationHosts
        Component.onCompleted: initialize(ComputerManager)
    }
    CalibrationHostSelection {
        id: calibrationSelection
        hosts: calibrationHosts.onlineHosts
        testedUuid: calibrationDialog.testedHostUuid
        onTestedHostUnavailable: {
            calibrationDialog.hasStarted = false
            if (PyroWaveCalibrator.running) {
                calibrationDialog.stopRequested = true
                PyroWaveCalibrator.cancel()
            }
        }
    }
    objectName: "settingsContent"

    signal languageChanged()
    onLanguageChanged: settingsShell.languageChanged()

    boundsBehavior: Flickable.StopAtBounds

    contentWidth: settingsColumn1.width > settingsColumn2.width ? settingsColumn1.width : settingsColumn2.width
    contentHeight: (settingsShell.category === 7 ? aboutColumn.height : settingsColumn1.height + settingsColumn2.height) + Style.px(24, settingsShell.Window.window)

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

    Window.onActiveFocusItemChanged: {
        var item = Window.activeFocusItem
        if (item && isChildOfFlickable(item)) {
            while (item && item.settingsEntry !== true) item = item.parent
            if (item) settingsShell.rowFocused(item)
        }
    }

    Component.onDestruction: {
        // Also save preferences on destruction, since we won't get a
        // deactivating callback if the user just closes Moonlight
        StreamingPreferences.save()
    }

    Column {
        padding: Style.px(0, settingsShell.Window.window)
        id: settingsColumn1
        width: settingsPage.width
        spacing: Style.px(32, settingsShell.Window.window)

        ConsoleSettingsSection {
            id: basicSettingsGroupBox
            readonly property int settingsCategory: 0
            visible: settingsShell.indexing || settingsShell.category === settingsCategory
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: Style.px(0, settingsShell.Window.window)
            title: qsTr("Video")
            font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

            Column {
                anchors.fill: parent
                spacing: 0



                Column {
                    width: parent.width
                    spacing: 0
                    ConsoleSettingRow {
                        width: parent.width
                        objectName: "resolutionRow"
                        description: (settingsShell.desktopPreferences ? qsTr("Choose the image size requested from the host.") + "\n\n" : "") +
                                     qsTr("Setting values too high for your PC or network connection may cause lag, stuttering, or errors.")
                        labelItem: ConsoleText { text: qsTr("Resolution") }
                        editor: AutoResizingComboBox {
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
                        maximumWidth: parent.width
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
                                ConsoleText {
                    role: "secondary"
                                    text: qsTr("Custom resolutions are not officially supported by GeForce Experience, so it will not set your host display resolution. You will need to set it manually while in game.") + "\n\n" +
                                          qsTr("Resolutions that are not supported by your client or host PC may cause streaming errors.") + "\n"
                                    wrapMode: Label.WordWrap
                                    Layout.maximumWidth: Style.px(300, settingsShell.Window.window)
                                }

                                ConsoleText {
                                    text: qsTr("Enter a custom resolution:")
                                    font.bold: true
                                }

                                RowLayout {
                                    ConsoleTextField {
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
                                            if (controllerEditing) openKeyboard()
                                            else customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            if (controllerEditing) openKeyboard()
                                            else customResolutionDialog.accept()
                                        }
                                    }

                                    ConsoleText {
                                        text: "x"
                                        font.bold: true
                                    }

                                    ConsoleTextField {
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
                                            if (controllerEditing) openKeyboard()
                                            else customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            if (controllerEditing) openKeyboard()
                                            else customResolutionDialog.accept()
                                        }
                                    }
                                }
                            }
                        }
                    }
                    }
                    ConsoleSettingRow {
                        width: parent.width
                        objectName: "frameRateRow"
                        description: settingsShell.desktopPreferences ? qsTr("Choose how many frames per second the host should send.") : ""
                        labelItem: ConsoleText { text: qsTr("Frame rate") }
                        editor: AutoResizingComboBox {
                        property int lastIndexValue
                        property int fpsLimit: 0

                        function updateBitrateForSelection() {
                            var selectedFps = parseInt(model.get(fpsComboBox.currentIndex).video_fps)
                            var fpsChanged = StreamingPreferences.fps !== selectedFps
                            StreamingPreferences.fps = selectedFps

                            if (fpsChanged && StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = slider.defaultBitrate();

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
                                ConsoleText {
                                    text: qsTr("Enter a custom frame rate:")
                                    font.bold: true
                                }

                                RowLayout {
                                    ConsoleTextField {
                                        id: fpsField
                                        maximumLength: 4
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: fpsComboBox.currentIndex >= 0 && fpsComboBox.currentIndex < fpsListModel.count ?
                                                             fpsListModel.get(fpsComboBox.currentIndex).video_fps : ""
                                        validator: IntValidator{bottom:10; top:fpsComboBox.fpsLimit >= 10 ? fpsComboBox.fpsLimit : 9999}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customFpsDialog.standardButton) {
                                                customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            if (controllerEditing) openKeyboard()
                                            else customFpsDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            if (controllerEditing) openKeyboard()
                                            else customFpsDialog.accept()
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
                            case "low-latency-vrr":
                                return qsTr("%1 FPS (Low-latency VRR)").arg(choice.video_fps)
                            case "custom":
                                return qsTr("Custom (%1 FPS)").arg(choice.video_fps)
                            default:
                                return qsTr("%1 FPS").arg(choice.video_fps)
                            }
                        }

                        function swapVrrRate(enabled) {
                            var fastestRefresh = Math.max.apply(null, getRefreshRates())
                            if (!isFinite(fastestRefresh)) return

                            var targetFps = StreamingPreferences.fpsAfterVrrToggle(fastestRefresh, enabled)
                            if (targetFps === StreamingPreferences.fps) return

                            StreamingPreferences.fps = targetFps
                            if (StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = slider.defaultBitrate()
                            }
                            reinitialize()
                        }

                        function reinitialize() {
                            fpsLimit = StreamingPreferences.enableVrr && StreamingPreferences.enableVsync &&
                                       StreamingPreferences.ultraLowLatencyVrr ? SystemProperties.getUltraLowLatencyVrrFpsLimit() : 0
                            var choices = StreamingPreferences.getFpsChoices(getRefreshRates(), fpsLimit)
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
                            if (fpsLimit > 0) saved_fps = Math.min(saved_fps, fpsLimit)
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
                            StreamingPreferences.ultraLowLatencyVrrChanged.connect(reinitialize)
                        }

                        model: ListModel {
                            id: fpsListModel
                        }

                        id: fpsComboBox
                        maximumWidth: parent.width
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
                }

                ConsoleSettingRow {
                    width: parent.width
                    description: settingsShell.desktopPreferences ?
                        qsTr("Automatic selects a compatible codec for the host and client.") + "\n\n" + codecComboBox.ToolTip.text : ""
                    labelItem: ConsoleText {
                    width: parent.width
                    id: resVCCTitle
                    text: qsTr("Video codec")
                    wrapMode: Text.Wrap
                }
                    editor: AutoResizingComboBox {
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

                            }
                            else if (StreamingPreferences.bitrateKbps > slider.to) {
                                StreamingPreferences.bitrateKbps = slider.to

                            }
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 8000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered && slider.pyroWave
                    ToolTip.text: qsTr("PyroWave is a GPU wavelet codec. It needs a wired connection with hundreds of Mbps to spare and a host with PyroWave support; other hosts fall back to H.264. On Linux, GPU readback and upload may limit frame rate.")
                }
                }

                ConsoleCheckBox {
                    id: enableYUV444
                    width: parent.width
                    text: qsTr("Enable YUV 4:4:4")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

                    checked: StreamingPreferences.enableYUV444
                    onCheckedChanged: {
                        // This is called on init, so only reset to default bitrate when checked state changes.
                        if (StreamingPreferences.enableYUV444 != checked) {
                            StreamingPreferences.enableYUV444 = checked
                            if (StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = slider.defaultBitrate();

                            }
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: enabled ?
                                      qsTr("Good for streaming desktop and text-heavy games, but not recommended for fast-paced games.")
                                    :
                                      qsTr("YUV 4:4:4 is not supported on this PC.")
                }

                ConsoleSettingRow {
                    width: parent.width
                    objectName: "bitrateRow"
                    labelItem: ConsoleText { text: qsTr("Video bitrate") }
                    description: qsTr("Lower the bitrate on slower connections. Raise it to increase image quality. Changes apply to the next stream.")
                    editor: VideoBitrateSlider {
                        id: slider

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

                        onValueChanged: {
                            StreamingPreferences.bitrateKbps = value
                        }

                        onMoved: {
                            StreamingPreferences.autoAdjustBitrate = false
                        }
                        onSettingsCommitted: function(newValue) {
                            StreamingPreferences.bitrateKbps = newValue
                            StreamingPreferences.autoAdjustBitrate = false
                        }

                        Component.onCompleted: {
                            // Refresh the text after translations change
                            languageChanged.connect(valueChanged)
                        }
                    }
                }
                ConsoleListRow {
                        id: resetBitrateButton
                        objectName: "resetBitrateRow"
                        text: qsTr("Use Default (%1 Mbps)").arg(slider.defaultBitrate() / 1000.0)
                        visible: StreamingPreferences.bitrateKbps !== slider.defaultBitrate()
                        onClicked: {
                            var defaultBitrate = slider.defaultBitrate()
                            StreamingPreferences.bitrateKbps = defaultBitrate
                            StreamingPreferences.autoAdjustBitrate = true

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
                    spacing: Style.px(12, settingsShell.Window.window)
                    visible: slider.pyroWave && (Qt.platform.os === "linux" || Qt.platform.os === "windows")

                    Component.onCompleted: NetworkBuffers.refresh()

                    ConsoleText {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: NetworkBuffers.needsFix
                        color: "#ffb74d"
                        text: NetworkBuffers.problemText
                    }

                    Column {
                        width: parent.width
                        spacing: 0
                        visible: NetworkBuffers.needsFix

                        ConsoleListRow {
                            text: qsTr("Fix it")
                            visible: NetworkBuffers.canApply
                            enabled: !NetworkBuffers.busy
                            onClicked: NetworkBuffers.apply()

                            ToolTip.delay: 1000
                            ToolTip.timeout: 10000
                            ToolTip.visible: !settingsShell.desktopPreferences && hovered
                            ToolTip.text: NetworkBuffers.fixDescription
                        }

                        ConsoleListRow {
                            text: qsTr("Copy command")
                            onClicked: NetworkBuffers.copyCommand()
                        }
                    }

                    ConsoleText {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: NetworkBuffers.needsFix && !NetworkBuffers.canApply
                        text: NetworkBuffers.manualHint + "\n" + NetworkBuffers.manualCommand
                    }

                    ConsoleText {
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
                    readonly property string selectedHostUuid: calibrationSelection.selectedUuid
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
                        color: Style.raised
                        radius: Style.px(12, settingsShell.Window.window)
                        border.color: Style.outline
                    }
                    onAboutToShow: {
                        PyroWaveCalibrator.reset()
                        step = 0
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
                        if (PyroWaveCalibrator.running || calibrationSelection.currentIndex < 0) return
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
                        padding: Style.px(0, settingsShell.Window.window)
                        contentItem: RowLayout {
                            spacing: Style.px(8, settingsShell.Window.window)
                            CalibrationButton {
                                id: calibrationRunButton
                                objectName: "pyrowaveCalibrationRun"
                                text: PyroWaveCalibrator.running ? (calibrationDialog.stopRequested ? qsTr("Stopping…") : qsTr("Stop test")) :
                                      calibrationDialog.step === 0 ? qsTr("Test bandwidth") : qsTr("Test decoder again")
                                enabled: PyroWaveCalibrator.running ? !calibrationDialog.stopRequested :
                                         calibrationDialog.step === 0 ? calibrationSelection.currentIndex >= 0 : calibrationDialog.canContinue
                                implicitHeight: Style.px(44, settingsShell.Window.window)
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
                                implicitHeight: Style.px(44, settingsShell.Window.window)
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
                                implicitHeight: Style.px(44, settingsShell.Window.window)
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
                                implicitHeight: Style.px(44, settingsShell.Window.window)
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
                                spacing: Style.px(12, settingsShell.Window.window)
                                RowLayout {
                                    Layout.fillWidth: true
                                    ConsoleText { text: qsTr("Host") }
                                    AutoResizingComboBox {
                                        id: calibrationHost
                                        objectName: "pyrowaveCalibrationHost"
                                        Layout.fillWidth: true
                                        model: calibrationSelection.hosts
                                        textRole: "name"
                                        currentIndex: calibrationSelection.currentIndex
                                        onActivated: calibrationSelection.select(index)
                                        displayText: count > 0 ? currentText : qsTr("No online hosts available")
                                        enabled: count > 0 && !PyroWaveCalibrator.running
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
                                    ConsoleText { text: qsTr("%1 FPS").arg(calibrationDialog.testFps) }
                                }
                                ConsoleText {
                                    Layout.fillWidth: true
                                    font.bold: true
                                    text: qsTr("Choose your quality target")
                                    wrapMode: Text.Wrap
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Style.px(8, settingsShell.Window.window)
                                    Repeater {
                                        id: calibrationTargets
                                        model: calibrationDialog.targets
                                        delegate: CalibrationButton {
                                            id: targetCard
                                            objectName: "pyrowaveCalibrationTarget" + index
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: Style.px(1, settingsShell.Window.window)
                                            Layout.preferredHeight: Style.px(82, settingsShell.Window.window)
                                            text: modelData.text
                                            checked: calibrationDialog.targetIndex === index
                                            enabled: !PyroWaveCalibrator.running
                                            activeFocusOnTab: true
                                            Accessible.name: modelData.text
                                            Accessible.description: modelData.description
                                            contentItem: Column {
                                                spacing: Style.px(6, settingsShell.Window.window)
                                                ConsoleText {
                                                    width: parent.width
                                                    text: (targetCard.checked ? "\u2713 " : "") + targetCard.text
                                                    font.bold: targetCard.checked
                                                    horizontalAlignment: Text.AlignHCenter
                                                    elide: Text.ElideRight
                                                    color: "#ffffff"
                                                }
                                                ConsoleText {
                                                    width: parent.width
                                                    text: modelData.summary
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
                                ConsoleText {
                                    Layout.fillWidth: true
                                    Layout.minimumHeight: Style.px(32, settingsShell.Window.window)
                                    wrapMode: Text.Wrap
                                    text: calibrationDialog.targets[calibrationDialog.targetIndex].description
                                }
                                ConsoleText {
                                    Layout.fillWidth: true
                                    text: qsTr("Test your connection first, then continue to test this device's decoder.")
                                    wrapMode: Text.Wrap
                                    opacity: 0.7
                                }
                                Item { Layout.fillHeight: true }
                                Pane {
                                    Layout.fillWidth: true
                                    padding: Style.px(0, settingsShell.Window.window)
                                    readonly property string grade: calibrationDialog.canContinue ? PyroWaveCalibrator.bandwidthQuality : ""
                                    readonly property bool warning: grade === "reduced"
                                    background: Rectangle {
                                        color: parent.grade === "low" ? "#452b2b" : calibrationDialog.canContinue ? (parent.warning ? "#413824" : "#283b2e") : "#252525"
                                        radius: Style.px(8, settingsShell.Window.window)
                                        border.color: parent.grade === "low" ? "#b45c5c" : calibrationDialog.canContinue ? (parent.warning ? "#aa8542" : "#568260") : "#505050"
                                    }
                                    contentItem: RowLayout {
                                        spacing: Style.px(12, settingsShell.Window.window)
                                        // Keep the text width stable when the spinner appears.
                                        Item {
                                            Layout.preferredWidth: Style.px(28, settingsShell.Window.window)
                                            Layout.preferredHeight: Style.px(28, settingsShell.Window.window)
                                            BusyIndicator {
                                                anchors.fill: parent
                                                running: PyroWaveCalibrator.running
                                                visible: running
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: Style.px(4, settingsShell.Window.window)
                                            ConsoleText {
                                                id: calibrationStatusText
                                                objectName: "pyrowaveCalibrationStatus"
                                                Layout.fillWidth: true
                                                Layout.minimumHeight: fontMetrics.height * 3
                                                verticalAlignment: Text.AlignTop
                                                wrapMode: Text.Wrap
                                                text: PyroWaveCalibrator.running && !calibrationSelection.testedHostAvailable ? qsTr("Host went offline. Stopping test…") :
                                                      calibrationSelection.currentIndex < 0 ? qsTr("No online hosts available. Waiting for discovery.") :
                                                      !calibrationDialog.hasStarted ? qsTr("Ready to test your connection.") :
                                                      !calibrationDialog.resultsMatch ? qsTr("Selection changed. Test bandwidth again to continue.") : PyroWaveCalibrator.message
                                            }
                                            ConsoleText {
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
                                ConsoleText {
                                    Layout.fillWidth: true
                                    color: "#bdbdbd"
                                    wrapMode: Text.Wrap
                                    text: qsTr("↑↓ Navigate  ·  ←→ Choose target  ·  %1 Select  ·  %2 Back")
                                        .arg(StreamingPreferences.swapFaceButtons ? "B" : "A")
                                        .arg(StreamingPreferences.swapFaceButtons ? "A" : "B")
                                }
                            }
                        }
                        ColumnLayout {
                            spacing: Style.px(8, settingsShell.Window.window)
                            ConsoleText {
                                Layout.fillWidth: true
                                font.bold: true
                                text: calibrationDialog.targets[calibrationDialog.targetIndex].text + " · " + qsTr("%1 FPS").arg(calibrationDialog.testFps)
                            }
                            ConsoleText {
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
                                spacing: Style.px(6, settingsShell.Window.window)
                                ConsoleText { width: Style.px(72, settingsShell.Window.window); text: qsTr("Resolution") }
                                ConsoleText { width: (calibrationHeaders.width - 84) / 2; text: "4:4:4"; horizontalAlignment: Text.AlignHCenter }
                                ConsoleText { width: (calibrationHeaders.width - 84) / 2; text: "4:2:0"; horizontalAlignment: Text.AlignHCenter }
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
                                    spacing: Style.px(6, settingsShell.Window.window)
                                    ConsoleText {
                                        width: parent.width
                                        visible: calibrationDetails.checked
                                        wrapMode: Text.Wrap
                                        text: PyroWaveCalibrator.linkSummary + " " +
                                              qsTr("Rates include FEC and packet overhead. Quality is relative to the developer's recommended image bitrate. Every selected target is tested afresh.")
                                    }
                                    Flow {
                                        width: parent.width
                                        spacing: Style.px(12, settingsShell.Window.window)
                                        Repeater {
                                            model: [
                                                { tier: "any", text: qsTr("Any display") },
                                                { tier: "vrr", text: qsTr("Needs VRR") },
                                                { tier: "vrrLarge", text: qsTr("VRR · Smooth") },
                                                { tier: "slow", text: qsTr("Can't keep up") }
                                            ]
                                            delegate: Row {
                                                spacing: Style.px(12, settingsShell.Window.window)
                                                Rectangle {
                                                    width: Style.px(8, settingsShell.Window.window); height: Style.px(8, settingsShell.Window.window); radius: Style.px(4, settingsShell.Window.window)
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    color: calibrationDialog.tierColors[modelData.tier]
                                                }
                                                ConsoleText { text: modelData.text; font.pixelSize: Style.typeSize("body", settingsShell.Window.window) }
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
                                            spacing: Style.px(6, settingsShell.Window.window)
                                            Rectangle {
                                                width: Style.px(10, settingsShell.Window.window)
                                                height: Style.px(10, settingsShell.Window.window)
                                                radius: Style.px(5, settingsShell.Window.window)
                                                anchors.verticalCenter: parent.verticalCenter
                                                color: calibrationDialog.tierColors[modelData.tier]
                                            }
                                            ConsoleText {
                                                width: calibrationTable.width - 16
                                                wrapMode: Text.Wrap
                                                text: modelData.text
                                            }
                                        }
                                    }

                                    ConsoleText {
                    role: "secondary"
                                        width: parent.width
                                        wrapMode: Text.Wrap
                                        visible: calibrationDetails.checked
                                        text: qsTr("Reduced or low quality means the image bitrate is below the developer's recommendation. This can follow your chosen target, network limits, or device limits.")
                                    }



                                    Repeater {
                                        id: calibrationRows
                                        model: calibrationDialog.rows
                                        delegate: Row {
                                            id: calibrationRow
                                            width: calibrationTable.width
                                            spacing: Style.px(6, settingsShell.Window.window)
                                            readonly property int rowIndex: index
                                            readonly property var rowData: modelData
                                            property alias columns: calibrationChroma

                                            ConsoleText {
                                                width: Style.px(72, settingsShell.Window.window)
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
                                                    spacing: Style.px(2, settingsShell.Window.window)

                                                    Repeater {
                                                        id: calibrationFormats
                                                        model: chromaColumn.modes
                                                        delegate: CalibrationButton {
                                                            id: optionButton
                                                            objectName: "pyrowaveCalibrationOption" + (calibrationRow.rowIndex * 4 + modelData)
                                                            width: parent.width
                                                            height: Style.px(44, settingsShell.Window.window)
                                                            padding: Style.px(6, settingsShell.Window.window)
                                                            topPadding: Style.px(4, settingsShell.Window.window)
                                                            bottomPadding: Style.px(4, settingsShell.Window.window)
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
                                                                spacing: Style.px(2, settingsShell.Window.window)
                                                                opacity: !optionButton.enabled ? 0.6 : optionButton.activeFocus || optionButton.hovered ? 1.0 : optionButton.option && !optionButton.option.keepsUp ? 0.7 : 1.0

                                                                ConsoleText {
                                                                    width: parent.width
                                                                    horizontalAlignment: Text.AlignHCenter
                                                                    elide: Text.ElideRight
                                                                    text: calibrationDialog.headline(optionButton.option, optionButton.hdr)
                                                                }

                                                                Row {
                                                                    anchors.horizontalCenter: parent.horizontalCenter
                                                                    spacing: Style.px(6, settingsShell.Window.window)

                                                                    Rectangle {
                                                                        width: Style.px(10, settingsShell.Window.window)
                                                                        height: Style.px(10, settingsShell.Window.window)
                                                                        radius: Style.px(5, settingsShell.Window.window)
                                                                        anchors.verticalCenter: parent.verticalCenter
                                                                        visible: !!optionButton.option
                                                                        color: calibrationDialog.tierColor(optionButton.option)
                                                                    }

                                                                    ConsoleText {
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
                    visible: settingsShell.desktopPreferences
                    text: qsTr("Display & pacing")
                    font.pixelSize: 19; font.weight: Font.DemiBold
                    leftPadding: 12; topPadding: 22; bottomPadding: 10
                }
                ConsoleSettingRow {
                    visible: SystemProperties.hasDesktopEnvironment
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    id: windowModeTitle
                    text: qsTr("Display mode")
                    wrapMode: Text.Wrap
                    visible: SystemProperties.hasDesktopEnvironment
                }
                    editor: AutoResizingComboBox {
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
                        if (!SystemProperties.hasDesktopEnvironment) {
                            // Category visibility must not suppress saved-value initialization.
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
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: vrrForced ?
                                      qsTr("Borderless windowed mode is required for active VRR streaming. Your saved display mode will be restored for non-VRR sessions.")
                                    :
                                      qsTr("Fullscreen generally provides the best performance, but borderless windowed may work better with features like macOS Spaces, Alt+Tab, screenshot tools, on-screen overlays, etc.")
                }
                }

                Column {
                    spacing: 0
                    width: parent.width

                    ConsoleCheckBox {
                        width: parent.width
                        id: vsyncCheck
                        hoverEnabled: true
                        text: qsTr("V-Sync")
                        font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                        checked: StreamingPreferences.enableVsync
                        onCheckedChanged: {
                            StreamingPreferences.enableVsync = checked
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: !settingsShell.desktopPreferences && hovered
                        ToolTip.text: qsTr("Disabling V-Sync allows sub-frame rendering latency, but it can display visible tearing")
                    }

                    ConsoleCheckBox {
                        width: parent.width
                        id: framePacingCheck
                        objectName: "framePacingRow"
                        hoverEnabled: true
                        text: qsTr("Frame pacing")
                        font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                        enabled: StreamingPreferences.enableVsync
                        checked: StreamingPreferences.enableVsync && StreamingPreferences.framePacing
                        onCheckedChanged: {
                            StreamingPreferences.framePacing = checked
                        }
                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: !settingsShell.desktopPreferences && hovered
                        ToolTip.text: !enabled && settingsShell.desktopPreferences ? qsTr("Enable V-Sync to change frame pacing.") : qsTr("Frame pacing reduces micro-stutter by delaying frames that come in too early")
                    }

                    ConsoleCheckBox {
                        width: parent.width
                        hoverEnabled: true
                        text: qsTr("VRR")
                        font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                        enabled: StreamingPreferences.enableVsync
                        checked: StreamingPreferences.enableVrr
                        onToggled: {
                            StreamingPreferences.enableVrr = checked
                            if (Qt.platform.os === "windows") {
                                fpsComboBox.swapVrrRate(checked)
                            }
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: !settingsShell.desktopPreferences && hovered
                        ToolTip.text: enabled ?
                                          qsTr("VRR uses adaptive presentation in borderless fullscreen.") :
                                          qsTr("VRR requires V-Sync. Enable V-Sync to change this setting.")
                    }

                    Timer {
                        id: gr10Detector
                        property int dockState: SystemProperties.Gr10Absent
                        interval: 2000
                        repeat: true
                        triggeredOnStart: true
                        running: Qt.platform.os === "linux" && parent.visible
                        onTriggered: dockState = SystemProperties.benqGr10State()
                    }

                    ConsoleCheckBox {
                        objectName: "benqGr10VrrRestart"
                        readonly property bool helpDialogOnly: true
                        width: parent.width
                        visible: gr10Detector.dockState !== SystemProperties.Gr10Absent
                        enabled: StreamingPreferences.enableVrr && StreamingPreferences.enableVsync
                        text: qsTr("Restart BenQ dock to restore VRR")
                        font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                        checked: StreamingPreferences.benqGr10VrrRestart
                        onToggled: StreamingPreferences.benqGr10VrrRestart = checked
                        description: qsTr("The BenQ GR10 can lose VRR after a display mode switch. If this happens, Moonlight restarts the dock before streaming. USB and Ethernet briefly disconnect.") +
                                     (gr10Detector.dockState === SystemProperties.Gr10NeedsHelper ?
                                          "\n\n" + qsTr("Install the SteamOS GR10 recovery helper to use this setting.") : "")
                    }

                    ConsoleCheckBox {
                        width: parent.width
                        objectName: "ultraLowLatencyVrr"
                        hoverEnabled: true
                        text: qsTr("Ultra Low Latency VRR")
                        visible: StreamingPreferences.enableVrr
                        enabled: StreamingPreferences.enableVsync && StreamingPreferences.enableVrr
                        checked: StreamingPreferences.ultraLowLatencyVrr
                        onToggled: StreamingPreferences.ultraLowLatencyVrr = checked

                        ToolTip.delay: 1000
                        ToolTip.timeout: 10000
                        ToolTip.visible: !settingsShell.desktopPreferences && hovered
                        ToolTip.text: qsTr("On supported systems, saves about 2 frames of presentation latency on average compared with standard VRR.") + "\n\n" +
                                      qsTr("Uses low-latency presentation on supported Windows 11 systems, SteamOS 3.10 or later in Gaming Mode, and Linux Wayland displays that support scheduled Vulkan presentation. Your VRR timing profile still controls Moonlight's adaptive buffer.") + "\n\n" +
                                      qsTr("Caps a 120 Hz display at 116 FPS when supported. Other setups use five-sixths of the refresh rate and keep their configured buffer.") +
                                      (fpsComboBox.fpsLimit > 0 ? "\n\n" + qsTr("Current frame rate limit: %1 FPS").arg(fpsComboBox.fpsLimit) : "")
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

                ConsoleCheckBox {
                    hoverEnabled: true
                    text: qsTr("Reduce judder")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    visible: StreamingPreferences.enableVrr
                    enabled: StreamingPreferences.enableVsync && StreamingPreferences.enableVrr
                    checked: StreamingPreferences.smoothVrrFrameTiming
                    onCheckedChanged: StreamingPreferences.smoothVrrFrameTiming = checked

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Adjusts when frames are shown to smooth small variations in frame timing. This can add a small amount of delay.") + "\n\n" +
                                  qsTr("Reconnect the stream after changing this setting.")
                }

                ConsoleCheckBox {
                    hoverEnabled: true
                    text: qsTr("High-performance GPU power while streaming")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    visible: Qt.platform.os === "linux"
                    checked: StreamingPreferences.highPerformanceGpuPower
                    onCheckedChanged: StreamingPreferences.highPerformanceGpuPower = checked

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Holds an AMD GPU at its high-performance clocks for the whole stream so it cannot drop its clocks between frames, which can delay when the display shows a frame that was ready on time. Uses more power and reduces battery life. Has no effect if GPU clocks are already set manually.") + "\n\n" +
                                  qsTr("Takes effect at the start of the next stream.")
                }

                ConsoleCheckBox {
                    id: enableHdr
                    width: parent.width
                    text: qsTr("Enable HDR")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

                    enabled: SystemProperties.supportsHdr
                    checked: enabled && StreamingPreferences.enableHdr
                    onCheckedChanged: {
                        if (StreamingPreferences.enableHdr != checked) {
                            StreamingPreferences.enableHdr = checked
                            // PyroWave's default bitrate depends on HDR
                            if (slider.pyroWave && StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = slider.defaultBitrate();

                            }
                        }
                    }

                    // Updating StreamingPreferences.videoCodecConfig is handled above

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: enabled ?
                                      qsTr("The stream will be HDR-capable, but some games may require an HDR monitor on your host PC to enable HDR mode.")
                                    :
                                      qsTr("HDR streaming is not supported on this PC.")
                }
            }
        }

        ConsoleSettingsSection {
            width: parent.width - (parent.leftPadding + parent.rightPadding)
            padding: Style.px(0, settingsShell.Window.window)
            readonly property int settingsCategory: 5
            visible: settingsShell.indexing || settingsShell.category === settingsCategory
            title: qsTr("VRR diagnostics")
            font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

            Column {
                anchors.fill: parent
                spacing: Style.px(8, settingsShell.Window.window)

                ConsoleCheckBox {
                    id: traceVrrFramesCheckBox
                    text: qsTr("Trace VRR frames for debugging")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.traceVrrFrames
                    onCheckedChanged: StreamingPreferences.traceVrrFrames = checked
                }

                ConsoleText {
                    leftPadding: Style.px(16, settingsShell.Window.window)
                    rightPadding: Style.px(16, settingsShell.Window.window)
                    role: "secondary"
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Saves frame traces and session logs to the vrr-diagnostics folder on your Desktop, with a separate folder for each stream. Does not change your VRR timing settings.")
                }

                ConsoleText {
                    leftPadding: Style.px(16, settingsShell.Window.window)
                    rightPadding: Style.px(16, settingsShell.Window.window)
                    role: "secondary"
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Enable VRR and reconnect the stream to start recording. Tracing can use substantial disk space and add diagnostic overhead. Uncheck this after debugging.")
                }

                ConsoleListRow {
                    text: StreamingPreferences.exportingDiagnostics ? qsTr("Exporting...") : qsTr("Export latest recording (ZIP)")
                    enabled: !StreamingPreferences.exportingDiagnostics
                    onClicked: StreamingPreferences.exportLatestDiagnostics()
                }

                ConsoleListRow {
                    text: qsTr("Open diagnostics folder")
                    onClicked: StreamingPreferences.openDiagnosticsFolder()
                }

                ConsoleText {
                    leftPadding: Style.px(16, settingsShell.Window.window)
                    rightPadding: Style.px(16, settingsShell.Window.window)
                    role: "secondary"
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Disconnect before exporting. Logs contain hardware and connection details; review them before sharing. Nothing is uploaded automatically.")
                }

                ConsoleText {
                    leftPadding: Style.px(16, settingsShell.Window.window)
                    rightPadding: Style.px(16, settingsShell.Window.window)
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    visible: text.length > 0
                    text: StreamingPreferences.diagnosticsStatus
                }
            }
        }

        ConsoleSettingsSection {

            id: audioSettingsGroupBox
            readonly property int settingsCategory: 1
            visible: settingsShell.indexing || settingsShell.category === settingsCategory
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: Style.px(0, settingsShell.Window.window)
            title: qsTr("Audio")
            font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

            Column {
                anchors.fill: parent
                spacing: 0

                ConsoleSettingRow {
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    id: resAudioTitle
                    text: qsTr("Audio configuration")
                    wrapMode: Text.Wrap
                }
                    editor: AutoResizingComboBox {
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
                }


                ConsoleCheckBox {
                    id: audioPcCheck
                    width: parent.width
                    text: qsTr("Mute host PC speakers while streaming")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: !StreamingPreferences.playAudioOnHost
                    onCheckedChanged: {
                        StreamingPreferences.playAudioOnHost = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("You must restart any game currently in progress for this setting to take effect")
                }

                ConsoleCheckBox {
                    id: muteOnFocusLossCheck
                    width: parent.width
                    text: qsTr("Mute audio stream when Moonlight is not the active window")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.muteOnFocusLoss
                    onCheckedChanged: {
                        StreamingPreferences.muteOnFocusLoss = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Mutes Moonlight's audio when you Alt+Tab out of the stream or click on a different window.")
                }
            }
        }

        ConsoleSettingsSection {
            id: hostSettingsGroupBox
            readonly property int settingsCategory: 3
            visible: settingsShell.indexing || settingsShell.category === settingsCategory
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: Style.px(0, settingsShell.Window.window)
            title: qsTr("Host")
            font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

            Column {
                anchors.fill: parent
                spacing: 0

                ConsoleCheckBox {
                    id: optimizeGameSettingsCheck
                    width: parent.width
                    text: qsTr("Optimize game settings for streaming")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.gameOptimizations
                    onCheckedChanged: {
                        StreamingPreferences.gameOptimizations = checked
                    }
                }

                ConsoleCheckBox {
                    id: quitAppAfter
                    width: parent.width
                    text: qsTr("Quit app on host PC after ending stream")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.quitAppAfter
                    onCheckedChanged: {
                        StreamingPreferences.quitAppAfter = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("This will close the app or game you are streaming when you end your stream. You will lose any unsaved progress!")
                }
            }
        }

        ConsoleSettingsSection {
            id: clipboardSettingsGroupBox
            visible: settingsShell.category === 3
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: Style.px(0, settingsShell.Window.window)
            title: qsTr("Clipboard Sync")
            font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

            Column {
                anchors.fill: parent
                spacing: 0

                ConsoleCheckBox {
                    id: clipboardSyncToHostCheck
                    width: parent.width
                    text: qsTr("Send copied text to the host")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.clipboardSyncToHost
                    onCheckedChanged: {
                        StreamingPreferences.clipboardSyncToHost = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Sends your local clipboard text to the host when the stream starts and whenever you return to the stream window. Requires a host that supports clipboard sync, such as Apollo or Vibepollo, with clipboard permissions granted to this client.") + "\n\n" +
                                  qsTr("You can also send it at any time while streaming using Ctrl+Alt+Shift+U.")
                }

                ConsoleCheckBox {
                    id: clipboardSyncFromHostCheck
                    width: parent.width
                    text: qsTr("Copy the host's clipboard text when leaving the stream window")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.clipboardSyncFromHost
                    onCheckedChanged: {
                        StreamingPreferences.clipboardSyncFromHost = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Replaces your local clipboard with the host's clipboard text when you Alt+Tab out of the stream or click on a different window. Requires a host that supports clipboard sync, such as Apollo or Vibepollo, with clipboard permissions granted to this client.") + "\n\n" +
                                  qsTr("You can also fetch it at any time while streaming using Ctrl+Alt+Shift+G.")
                }

                ConsoleSettingRow {
                    width: parent.width
                    labelItem: ConsoleText { text: qsTr("Max clipboard size (MB)") }
                    description: qsTr("Clipboard text larger than this is not sent to or accepted from the host. Changes apply to the next stream.")
                    editor: ConsoleSpinBox {
                        id: clipboardSyncMaxSizeSpinBox
                        objectName: "clipboardSyncMaxSize"
                        from: 1
                        to: 100
                        stepSize: 1
                        editable: true
                        value: StreamingPreferences.clipboardSyncMaxSizeMB
                        function commitSize(newValue) {
                            StreamingPreferences.clipboardSyncMaxSizeMB = Math.max(from, Math.min(to, Math.round(newValue)))
                        }
                        onValueModified: commitSize(value)
                        onSettingsCommitted: function(newValue) { commitSize(newValue) }
                        Keys.onLeftPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) commitSize(value - stepSize) }
                        Keys.onRightPressed: function(event) { if (!settingsRowHost || !settingsRowHost.settingsNavigation || !settingsRowHost.settingsNavigation.handleRowKey(settingsRowHost, event)) commitSize(value + stepSize) }
                        Accessible.name: qsTr("Maximum clipboard size in megabytes")
                    }
                }
            }
        }

        ConsoleSettingsSection {
            id: uiSettingsGroupBox
            readonly property int settingsCategory: 4
            visible: settingsShell.indexing || settingsShell.category === settingsCategory
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: Style.px(0, settingsShell.Window.window)
            title: qsTr("Interface")
            font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

            Column {
                anchors.fill: parent
                spacing: 0

                ConsoleButton {
                    objectName: "uiFullscreenButton"
                    visible: SystemProperties.hasDesktopEnvironment
                    text: settingsShell.Window.window && settingsShell.Window.window.visibility === Window.FullScreen ? qsTr("Exit fullscreen") : qsTr("Enter fullscreen")
                    textAlignment: Text.AlignLeft
                    onClicked: settingsShell.Window.window.toggleUiFullscreen()
                }

                ConsoleCheckBox {
                    width: parent.width
                    text: qsTr("Reduce interface motion")
                    checked: StreamingPreferences.uiReducedMotion
                    objectName: "reduceMotionRow"
                    onCheckedChanged: StreamingPreferences.uiReducedMotion = checked
                }

                ConsoleSettingRow {
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    id: languageTitle
                    text: qsTr("Language")
                    wrapMode: Text.Wrap
                }
                    editor: AutoResizingComboBox {
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
                                window.libraryNeedsReload = true

                                // Signal other controls to adjust their text
                                languageChanged()
                            }
                        }
                    }
                }
                }

                ConsoleSettingRow {
                    visible: SystemProperties.hasDesktopEnvironment
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    id: uiDisplayModeTitle
                    text: qsTr("GUI display mode")
                    wrapMode: Text.Wrap
                    visible: SystemProperties.hasDesktopEnvironment
                }
                    editor: AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        if (!SystemProperties.hasDesktopEnvironment) {
                            // Category visibility must not suppress saved-value initialization.
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
                    objectName: "uiDisplayModeChoice"
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
                }

                ConsoleCheckBox {
                    objectName: "controllerFullscreenCheck"
                    visible: SystemProperties.hasDesktopEnvironment
                    width: parent.width
                    text: qsTr("Fullscreen with a controller")
                    checked: StreamingPreferences.uiControllerFullscreen
                    onCheckedChanged: StreamingPreferences.uiControllerFullscreen = checked
                    ToolTip.text: qsTr("Enter fullscreen when a controller is connected, and stay there until you choose Exit fullscreen or press F11. Mouse and keyboard use do not change the window mode.")
                }

                ConsoleCheckBox {
                    id: connectionWarningsCheck
                    width: parent.width
                    text: qsTr("Show connection quality warnings")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.connectionWarnings
                    onCheckedChanged: {
                        StreamingPreferences.connectionWarnings = checked
                    }
                }

                ConsoleCheckBox {
                    id: hideVrrBufferWarningsCheck
                    width: parent.width
                    text: qsTr("Hide VRR buffer limit warnings")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.hideVrrBufferWarnings
                    onCheckedChanged: {
                        StreamingPreferences.hideVrrBufferWarnings = checked
                    }
                }

                ConsoleCheckBox {
                    id: configurationWarningsCheck
                    width: parent.width
                    text: qsTr("Show configuration warnings")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.configurationWarnings
                    onCheckedChanged: {
                        StreamingPreferences.configurationWarnings = checked
                    }
                }

                ConsoleCheckBox {
                    visible: SystemProperties.hasDiscordIntegration
                    id: discordPresenceCheck
                    width: parent.width
                    text: qsTr("Discord Rich Presence integration")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.richPresence
                    onCheckedChanged: {
                        StreamingPreferences.richPresence = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Updates your Discord status to display the name of the game you're streaming.")
                }

                ConsoleCheckBox {
                    id: keepAwakeCheck
                    width: parent.width
                    text: qsTr("Keep the display awake while streaming")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.keepAwake
                    onCheckedChanged: {
                        StreamingPreferences.keepAwake = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Prevents the screensaver from starting or the display from going to sleep while streaming.")
                }
            }
        }
    }

    Column {
        padding: Style.px(0, settingsShell.Window.window)
        rightPadding: Style.px(0, settingsShell.Window.window)
        y: settingsColumn1.height
        id: settingsColumn2
        width: settingsPage.width
        spacing: Style.px(32, settingsShell.Window.window)

        ConsoleSettingsSection {
            id: inputSettingsGroupBox
            readonly property int settingsCategory: 2
            visible: settingsShell.indexing || settingsShell.category === settingsCategory
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: Style.px(0, settingsShell.Window.window)
            title: qsTr("Mouse & keyboard")
            font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

            Column {
                anchors.fill: parent
                spacing: 0

                ConsoleCheckBox {
                    id: absoluteMouseCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Optimize mouse for remote desktop instead of games")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.absoluteMouseMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteMouseMode = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("This enables seamless mouse control without capturing the client's mouse cursor. It is ideal for remote desktop usage but will not work in most games.") + " " +
                                  qsTr("You can toggle this while streaming using Ctrl+Alt+Shift+M.") + "\n\n" +
                                  qsTr("NOTE: Due to a bug in GeForce Experience, this option may not work properly if your host PC has multiple monitors.")
                }

                ConsoleSettingRow {
                    width: parent.width
                    objectName: "captureSystemKeysRow"
                    labelItem: ConsoleText { text: qsTr("Capture system keyboard shortcuts") }
                    description: qsTr("Capture shortcuts such as Alt+Tab while streaming. Ctrl+Alt+Del is handled by Windows and cannot be captured.")
                    editor: AutoResizingComboBox {
                        id: captureKeysMode
                        enabled: SystemProperties.hasDesktopEnvironment
                        textRole: "text"
                        model: ListModel {
                            ListElement { text: qsTr("Off"); val: StreamingPreferences.CSK_OFF }
                            ListElement { text: qsTr("In fullscreen"); val: StreamingPreferences.CSK_FULLSCREEN }
                            ListElement { text: qsTr("Always"); val: StreamingPreferences.CSK_ALWAYS }
                        }
                        Component.onCompleted: {
                            for (var i = 0; i < count; ++i) {
                                if (model.get(i).val === StreamingPreferences.captureSysKeysMode) { currentIndex = i; break }
                            }
                            recalculateWidth()
                        }
                        onActivated: StreamingPreferences.captureSysKeysMode = model.get(currentIndex).val
                    }
                }

                ConsoleCheckBox {
                    id: absoluteTouchCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Use touchscreen as a virtual trackpad")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: !StreamingPreferences.absoluteTouchMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteTouchMode = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("When checked, the touchscreen acts like a trackpad. When unchecked, the touchscreen will directly control the mouse pointer.")
                }

                ConsoleCheckBox {
                    id: swapMouseButtonsCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Swap left and right mouse buttons")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.swapMouseButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapMouseButtons = checked
                    }
                }

                ConsoleCheckBox {
                    id: reverseScrollButtonsCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Reverse mouse scrolling direction")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.reverseScrollDirection
                    onCheckedChanged: {
                        StreamingPreferences.reverseScrollDirection = checked
                    }
                }
            }
        }

        ConsoleSettingsSection {
            id: gamepadSettingsGroupBox
            readonly property int settingsCategory: 2
            visible: settingsShell.indexing || settingsShell.category === settingsCategory
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: Style.px(0, settingsShell.Window.window)
            title: qsTr("Controllers")
            font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

            Column {
                anchors.fill: parent
                spacing: 0

                ConsoleCheckBox {
                    id: swapFaceButtonsCheck
                    width: parent.width
                    text: qsTr("Swap A/B and X/Y gamepad buttons")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.swapFaceButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapFaceButtons = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("This switches gamepads into a Nintendo-style button layout")
                }

                ConsoleCheckBox {
                    id: singleControllerCheck
                    width: parent.width
                    text: qsTr("Force gamepad #1 always connected")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: !StreamingPreferences.multiController
                    onCheckedChanged: {
                        StreamingPreferences.multiController = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Forces a single gamepad to always stay connected to the host, even if no gamepads are actually connected to this PC.") + " " +
                                  qsTr("Only enable this option when streaming a game that doesn't support gamepads being connected after startup.")
                }

                ConsoleCheckBox {
                    id: gamepadMouseCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Enable mouse control with gamepads by holding the 'Start' button")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.gamepadMouse
                    onCheckedChanged: {
                        StreamingPreferences.gamepadMouse = checked
                    }
                }

                ConsoleCheckBox {
                    id: backgroundGamepadCheck
                    width: parent.width
                    text: qsTr("Process gamepad input when Moonlight is in the background")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.backgroundGamepad
                    onCheckedChanged: {
                        StreamingPreferences.backgroundGamepad = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Allows Moonlight to capture gamepad inputs even if it's not the current window in focus")
                }

                ConsoleCheckBox {
                    id: steamControllerCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Drive a USB or puck-connected Steam Controller (2026) directly")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.steamControllerDriver
                    onCheckedChanged: {
                        StreamingPreferences.steamControllerDriver = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Announces a Steam Controller (2026) on a USB cable or the wireless puck to the host as a Steam Controller with both trackpads, gyro, grip sensors, battery and Steam haptics, and relays its firmware attributes.") + " " +
                                  qsTr("A client PC running Steam owns the controller first; close Steam or stop Steam Input for it.") + "\n\n" +
                                  qsTr("NEVER start a controller firmware update from Steam while streaming. The update would target the virtual controller on the host, not the real controller; it cannot succeed and may leave Steam confused. Update firmware with the controller connected directly to a PC running Steam.")
                }

                ConsoleCheckBox {
                    id: steamControllerMotionCheck
                    hoverEnabled: true
                    width: parent.width
                    enabled: steamControllerCheck.checked
                    text: qsTr("Steam Controller: gyro and accelerometer")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.steamControllerMotion
                    onCheckedChanged: {
                        StreamingPreferences.steamControllerMotion = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Asks the firmware for the raw IMU and advertises motion to the host; the host decides when it wants the reports.")
                }

                ConsoleCheckBox {
                    id: steamControllerSplitPadsCheck
                    hoverEnabled: true
                    width: parent.width
                    enabled: steamControllerCheck.checked
                    text: qsTr("Steam Controller: trackpads as DualShock touchpad halves")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.steamControllerSplitPads
                    onCheckedChanged: {
                        StreamingPreferences.steamControllerSplitPads = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Sends the left pad as finger 0 in the left half and the right pad as finger 1 in the right half of one touchpad, which a DualShock 4 emulation splits back.") + " " +
                                  qsTr("Turn it off only for a host that understands two touchpads.")
                }

                ConsoleSettingRow {
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    text: qsTr("Steam Controller: grip buttons")
                    wrapMode: Text.Wrap
                    enabled: steamControllerCheck.checked
                }
                    editor: AutoResizingComboBox {
                    id: steamControllerGripsComboBox
                    enabled: steamControllerCheck.checked
                    textRole: "text"

                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_grips = StreamingPreferences.steamControllerGrips
                        currentIndex = 0
                        for (var i = 0; i < steamControllerGripsListModel.count; i++) {
                            var el_grips = steamControllerGripsListModel.get(i).val;
                            if (saved_grips === el_grips) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    model: ListModel {
                        id: steamControllerGripsListModel
                        ListElement {
                            text: qsTr("Paddles 1-4 (Xbox Elite style)")
                            val: StreamingPreferences.SCG_PADDLES
                        }
                        ListElement {
                            text: qsTr("DualShock extras (Share, touchpad click, L3, R3)")
                            val: StreamingPreferences.SCG_DS4
                        }
                        ListElement {
                            text: qsTr("Off")
                            val: StreamingPreferences.SCG_OFF
                        }
                    }

                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated: {
                        StreamingPreferences.steamControllerGrips = steamControllerGripsListModel.get(currentIndex).val
                    }
                }
                }

                ConsoleSettingRow {
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    text: qsTr("Steam Controller: hold host rumble for")
                    wrapMode: Text.Wrap
                    enabled: steamControllerCheck.checked
                }
                    editor: AutoResizingComboBox {
                    id: steamControllerRumbleHoldComboBox
                    enabled: steamControllerCheck.checked
                    textRole: "text"

                    Component.onCompleted: {
                        var saved_hold = StreamingPreferences.steamControllerRumbleHold
                        currentIndex = 1
                        for (var i = 0; i < steamControllerRumbleHoldListModel.count; i++) {
                            if (saved_hold === steamControllerRumbleHoldListModel.get(i).val) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    model: ListModel {
                        id: steamControllerRumbleHoldListModel
                        ListElement {
                            text: qsTr("250 ms")
                            val: 250
                        }
                        ListElement {
                            text: qsTr("500 ms (default)")
                            val: 500
                        }
                        ListElement {
                            text: qsTr("1 second")
                            val: 1000
                        }
                        ListElement {
                            text: qsTr("2 seconds")
                            val: 2000
                        }
                        ListElement {
                            text: qsTr("Unlimited (the host sends the stop)")
                            val: 0
                        }
                    }

                    onActivated: {
                        StreamingPreferences.steamControllerRumbleHold = steamControllerRumbleHoldListModel.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("A rumble from the host is refreshed on the controller every 40 ms until this time passes, then the motors stop; choose Unlimited only when the host reliably sends a stop.")
                }
                }

                ConsoleCheckBox {
                    id: steamControllerStickStretchCheck
                    hoverEnabled: true
                    width: parent.width
                    enabled: steamControllerCheck.checked
                    text: qsTr("Steam Controller: stretch the sticks to the rim")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.steamControllerStickStretch
                    onCheckedChanged: {
                        StreamingPreferences.steamControllerStickStretch = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Divides each stick axis by the largest deflection seen this session and clamps to the unit circle. Off by default: Steam's own calibration expects the controller's native range.")
                }

                ConsoleSettingRow {
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    text: qsTr("Steam Controller over Bluetooth LE: pad click boost")
                    wrapMode: Text.Wrap
                    enabled: steamControllerCheck.checked
                }
                    editor: AutoResizingComboBox {
                    id: steamControllerHapticBoostComboBox
                    enabled: steamControllerCheck.checked
                    textRole: "text"

                    Component.onCompleted: {
                        var saved_boost = StreamingPreferences.steamControllerHapticBoost
                        currentIndex = 0
                        for (var i = 0; i < steamControllerHapticBoostListModel.count; i++) {
                            if (saved_boost === steamControllerHapticBoostListModel.get(i).val) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    model: ListModel {
                        id: steamControllerHapticBoostListModel
                        ListElement {
                            text: qsTr("Off (as Steam sends it)")
                            val: 0
                        }
                        ListElement {
                            text: qsTr("+6 dB")
                            val: 6
                        }
                        ListElement {
                            text: qsTr("+12 dB")
                            val: 12
                        }
                        ListElement {
                            text: qsTr("+20 dB (everything at the ceiling)")
                            val: 20
                        }
                    }

                    onActivated: {
                        StreamingPreferences.steamControllerHapticBoost = steamControllerHapticBoostListModel.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Bluetooth LE only: added to the gain of Steam's trackpad tick and click commands, clamped to the firmware's +6 dB ceiling. USB and puck links are not affected.")
                }
                }

                ConsoleSettingRow {
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    text: qsTr("Steam Controller over Bluetooth LE: pad slide pulse length")
                    wrapMode: Text.Wrap
                    enabled: steamControllerCheck.checked
                }
                    editor: AutoResizingComboBox {
                    id: steamControllerHapticPulseScaleComboBox
                    enabled: steamControllerCheck.checked
                    textRole: "text"

                    Component.onCompleted: {
                        var saved_scale = StreamingPreferences.steamControllerHapticPulseScale
                        currentIndex = 0
                        for (var i = 0; i < steamControllerHapticPulseScaleListModel.count; i++) {
                            if (saved_scale === steamControllerHapticPulseScaleListModel.get(i).val) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    model: ListModel {
                        id: steamControllerHapticPulseScaleListModel
                        ListElement {
                            text: qsTr("100% (as Steam sends it)")
                            val: 100
                        }
                        ListElement {
                            text: qsTr("150%")
                            val: 150
                        }
                        ListElement {
                            text: qsTr("200%")
                            val: 200
                        }
                        ListElement {
                            text: qsTr("300%")
                            val: 300
                        }
                    }

                    onActivated: {
                        StreamingPreferences.steamControllerHapticPulseScale = steamControllerHapticPulseScaleListModel.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Bluetooth LE only: Steam's slide texture is 400 us pulses with no gain field, so this lengthens their on-time instead. Stops are untouched.")
                }
                }

                ConsoleSettingRow {
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    text: qsTr("Steam Controller over Bluetooth LE: haptic master gain")
                    wrapMode: Text.Wrap
                    enabled: steamControllerCheck.checked
                }
                    editor: AutoResizingComboBox {
                    id: steamControllerHapticMasterGainComboBox
                    enabled: steamControllerCheck.checked
                    textRole: "text"

                    Component.onCompleted: {
                        var saved_gain = StreamingPreferences.steamControllerHapticMasterGain
                        currentIndex = 0
                        for (var i = 0; i < steamControllerHapticMasterGainListModel.count; i++) {
                            if (saved_gain === steamControllerHapticMasterGainListModel.get(i).val) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    model: ListModel {
                        id: steamControllerHapticMasterGainListModel
                        ListElement {
                            text: qsTr("Off (not written)")
                            val: 0
                        }
                        ListElement {
                            text: qsTr("+6 dB")
                            val: 6
                        }
                        ListElement {
                            text: qsTr("+12 dB")
                            val: 12
                        }
                    }

                    onActivated: {
                        StreamingPreferences.steamControllerHapticMasterGain = steamControllerHapticMasterGainListModel.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Bluetooth LE only: written as the firmware's haptic master gain setting (76) with the link's own settings and cleared at stream end. Steam never writes it; if the firmware honours it, everything gets stronger and the two knobs above are unnecessary.")
                }
                }
            }
        }

        ConsoleSettingsSection {
            id: advancedSettingsGroupBox
            readonly property int settingsCategory: 6
            visible: settingsShell.indexing || settingsShell.category === settingsCategory
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: Style.px(0, settingsShell.Window.window)
            title: qsTr("Advanced")
            font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

            Column {
                anchors.fill: parent
                spacing: 0

                ConsoleSettingRow {
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    id: resVDSTitle
                    text: qsTr("Video decoder")
                    wrapMode: Text.Wrap
                }
                    editor: AutoResizingComboBox {
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
                }

                ConsoleSettingRow {
                    visible: SystemProperties.isDarwin
                    width: parent.width
                    labelItem: ConsoleText {
                    width: parent.width
                    id: rendererTitle
                    text: qsTr("Renderer")
                    wrapMode: Text.Wrap
                    visible: SystemProperties.isDarwin
                }
                    editor: AutoResizingComboBox {
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
                }

                ConsoleCheckBox {
                    id: unlockBitrate
                    width: parent.width
                    text: qsTr("Unlock bitrate limit (Experimental)")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)

                    checked: StreamingPreferences.unlockBitrate
                    onCheckedChanged: {
                        StreamingPreferences.unlockBitrate = checked
                        StreamingPreferences.bitrateKbps = Math.min(StreamingPreferences.bitrateKbps, slider.to)

                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("This unlocks extremely high video bitrates for use with Sunshine hosts. It should only be used when streaming over an Ethernet LAN connection.")
                }

                ConsoleCheckBox {
                    width: parent.width
                    text: qsTr("Notify me about prerelease and nightly updates")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: AutoUpdateChecker.notifyPrereleases
                    onClicked: AutoUpdateChecker.notifyPrereleases = checked
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Enabled by default. Turn off to receive only stable update notifications. Updates are never installed automatically.")
                }

                ConsoleCheckBox {
                    id: enableMdns
                    width: parent.width
                    text: qsTr("Automatically find PCs on the local network (Recommended)")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
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

                ConsoleCheckBox {
                    id: detectNetworkBlocking
                    width: parent.width
                    text: qsTr("Automatically detect blocked connections (Recommended)")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.detectNetworkBlocking
                    onCheckedChanged: {
                        StreamingPreferences.detectNetworkBlocking = checked
                    }
                }

                ConsoleCheckBox {
                    id: showPerformanceOverlay
                    width: parent.width
                    text: qsTr("Show performance stats while streaming")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.showPerformanceOverlay
                    onCheckedChanged: {
                        StreamingPreferences.showPerformanceOverlay = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Display real-time stream performance information while streaming.") + "\n\n" +
                                  qsTr("You can toggle it at any time while streaming using Ctrl+Alt+Shift+S or Select+L1+R1+X.") + "\n\n" +
                                  qsTr("The performance overlay is not supported on Steam Link or Raspberry Pi.")
                }

                ConsoleCheckBox {
                    id: showFrametimeGraph
                    width: parent.width
                    text: qsTr("Show frametime graph while streaming")
                    font.pixelSize: Style.typeSize("body", settingsShell.Window.window)
                    checked: StreamingPreferences.showFrametimeGraph
                    onCheckedChanged: {
                        StreamingPreferences.showFrametimeGraph = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: !settingsShell.desktopPreferences && hovered
                    ToolTip.text: qsTr("Shows planned, submitted and displayed frame intervals in separate lanes during VRR streaming, so you can see where a stutter starts. Variation under 1 ms is drawn flat.") + "\n\n" +
                                  qsTr("You can toggle it at any time while streaming using Ctrl+Alt+Shift+F or Select+L1+R1+Y (Triangle).")
                }
            }
        }
    Column {
        id: aboutColumn
        readonly property int settingsCategory: 7
        visible: settingsShell.indexing || settingsShell.category === settingsCategory
        width: settingsPage.width; spacing: Style.px(16, settingsShell.Window.window)
        ConsoleText { role: "heading"; text: qsTr("About Moonlight") }
        ConsoleText { role: "secondary"; text: qsTr("Version %1").arg(SystemProperties.versionString) }
        ConsoleListRow { text: qsTr("Setup guide"); onClicked: Qt.openUrlExternally("https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide"); visible: SystemProperties.hasBrowser }
        ConsoleListRow { text: qsTr("Community"); onClicked: Qt.openUrlExternally("https://moonlight-stream.org/discord"); visible: SystemProperties.hasBrowser }
        ConsoleListRow {
            text: typeof updateButton !== "undefined" && updateButton.available ? qsTr("Download update %1").arg(updateButton.updateVersion) : qsTr("Check for updates")
            onClicked: { if (typeof updateButton !== "undefined" && updateButton.available) Qt.openUrlExternally(updateButton.browserUrl); else AutoUpdateChecker.start() }
            visible: SystemProperties.hasBrowser
        }
        ConsoleListRow {
            text: qsTr("Dismiss update")
            visible: typeof updateButton !== "undefined" && updateButton.available
            onClicked: { AutoUpdateChecker.dismissUpdate(updateButton.updateVersion); updateButton.available = false }
        }
        ConsoleListRow { text: qsTr("Quit Moonlight"); onClicked: { if (typeof window !== "undefined" && typeof window.requestQuit === "function") window.requestQuit() } }
    }
    }
}
    DesktopSettingsResults {
        id: results
        objectName: "desktopSettingsResults"
        x: settingsPage.x; y: settingsPage.y; width: settingsPage.width; height: settingsPage.height
        visible: settingsShell.searching
        catalog: settingsShell.catalog
        query: searchField.text
        onPicked: function(entry) { settingsShell.jumpTo(entry) }
        Keys.onUpPressed: step(-1)
        Keys.onDownPressed: step(1)
        Keys.onReturnPressed: choose()
        Keys.onEnterPressed: choose()
        Keys.onEscapePressed: { searchField.clear(); settingsShell.searchFocus() }
        Keys.onBacktabPressed: settingsShell.searchFocus()
        Keys.onTabPressed: doneButton.forceActiveFocus(Qt.TabFocusReason)
    }
    Binding {
        target: helpText
        property: "text"
        when: !settingsShell.desktopPreferences && !!settingsShell.hoverRow && !!settingsShell.hoverRow.settingsHelp &&
              settingsShell.hoverRow.helpDialogOnly !== true && (!settingsShell.hoverRow.target || settingsShell.hoverRow.target.helpDialogOnly !== true)
        value: settingsShell.hoverRow ? settingsShell.hoverRow.settingsHelp : ""
    }
    Rectangle {
        id: helpPanel
        visible: !settingsShell.desktopPreferences
        x: settingsPage.x; width: settingsPage.width
        height: Style.px(112, settingsShell.Window.window)
        anchors.bottom: parent.bottom
        color: Style.background
        Rectangle { width: parent.width; height: 1; color: Style.outline }
        Flickable {
            id: helpViewport
            anchors.fill: parent; anchors.margins: Style.px(16, settingsShell.Window.window)
            clip: true; contentHeight: helpText.implicitHeight; boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ConsoleText {
                id: helpText; width: parent.width - Style.px(16, settingsShell.Window.window); role: "secondary"; wrapMode: Text.Wrap
                text: settingsShell.categoryMenuFocused ? qsTr("Choose a category. Press Right or Confirm to open its settings.") :
                      settingsShell.helpDialogOnly ? "" :
                      settingsShell.activeRow && settingsShell.activeRow.settingsHelp ? settingsShell.activeRow.settingsHelp : settingsShell.activeRow && settingsShell.activeRow.settingsAction === true ? qsTr("Confirm to select this action. Press Left to return to categories.") : qsTr("Confirm to change this setting. Press Left to return to categories.")
            }
        }
        Rectangle {
            anchors.right: parent.right; anchors.bottom: parent.bottom
            width: overflowLabel.implicitWidth + Style.px(16, settingsShell.Window.window)
            height: overflowLabel.implicitHeight + Style.px(8, settingsShell.Window.window)
            color: Style.control; radius: Style.px(4, settingsShell.Window.window)
            visible: settingsShell.optionsAvailable && (settingsShell.helpDialogOnly || helpText.implicitHeight > helpViewport.height)
            ConsoleText { id: overflowLabel; anchors.centerIn: parent; role: "caption"; text: qsTr("More help…") }
            MouseArea { anchors.fill: parent; onClicked: settingsShell.optionsAction() }
        }
    }
}
