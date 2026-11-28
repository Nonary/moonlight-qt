import QtQuick 2.12
import QtQuick.Controls 2.5

Item {
    id: hint
    property Item row
    readonly property Item helpButton: visible && hasMore ? more : null
    readonly property bool unavailable: !!row && (!row.enabled || !!row.editor && !row.editor.enabled)
    readonly property string fullText: row && row.settingsHelp || ""
    readonly property bool explicitOnly: !!row && row.helpDialogOnly === true
    readonly property bool hasMore: fullText.length > 0 &&
        (explicitOnly || fullText.indexOf("\n\n") >= 0 || summary.truncated)
    implicitHeight: summary.implicitHeight + (more.visible ? more.height : 0)
    Text {
        id: summary
        objectName: "inlineSettingSummary"
        anchors.left: parent.left
        anchors.right: parent.right
        text: hint.unavailable && (!hint.fullText || hint.explicitOnly) ? qsTr("Unavailable with the current settings or device.") :
              (hint.unavailable ? qsTr("Unavailable") + " · " : "") + (hint.explicitOnly ? "" : hint.fullText.split("\n\n")[0])
        font.family: "Inter"
        font.pixelSize: 14
        color: "#bac1cb"
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }
    ToolButton {
        id: more
        objectName: "inlineMoreHelp"
        visible: hint.hasMore
        anchors.left: parent.left
        anchors.top: summary.bottom
        height: 28
        leftPadding: 6
        rightPadding: 6
        topPadding: 0
        bottomPadding: 0
        text: qsTr("More help")
        font.pixelSize: 14
        activeFocusOnTab: true
        Keys.onTabPressed: function(event) { hint.row.settingsNavigation.tabInForm(more, !!(event.modifiers & Qt.ShiftModifier)) }
        Keys.onBacktabPressed: hint.row.settingsNavigation.tabInForm(more, true)
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        Accessible.name: qsTr("More help for %1").arg(hint.row ? hint.row.settingsLabel : "")
        contentItem: Text { text: more.text; font: more.font; color: "#9bcfff"; verticalAlignment: Text.AlignVCenter }
        onClicked: hint.row.settingsNavigation.showHelp(hint.row)
    }
}
