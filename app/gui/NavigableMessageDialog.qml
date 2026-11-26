import QtQuick 2.0
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2
import "ConsoleStyle.js" as Style

NavigableDialog {
    id: dialog
    tabNavigation: false

    property alias text: dialogLabel.dialogText
    property alias showSpinner: dialogSpinner.visible
    property alias imageSrc: dialogImage.source

    property string helpText
    property string helpUrl : "https://github.com/moonlight-stream/moonlight-docs/wiki/Troubleshooting"
    property string helpTextSeparator : " "
    property string acceptText: ""
    property string rejectText: ""
    property int initialFocusButton: Dialog.NoButton

    onAboutToShow: {
        if (acceptText.length) {
            var accept = dialogButtonBox.standardButton(Dialog.Yes) || dialogButtonBox.standardButton(Dialog.Ok)
            if (accept) accept.text = acceptText
        }
        if (rejectText.length) {
            var reject = dialogButtonBox.standardButton(Dialog.No) || dialogButtonBox.standardButton(Dialog.Cancel)
            if (reject) reject.text = rejectText
        }
    }

    onOpened: {
        // Button ordering varies by platform, so select an explicit button
        // when the dialog has a preferred action.
        var focusButton = initialFocusButton !== Dialog.NoButton ?
                    dialogButtonBox.standardButton(initialFocusButton) : null
        if (!focusButton && (standardButtons & Dialog.No)) focusButton = dialogButtonBox.standardButton(Dialog.No)
        if (!focusButton && (standardButtons & Dialog.Cancel)) focusButton = dialogButtonBox.standardButton(Dialog.Cancel)
        if (!focusButton && dialogButtonBox.count > 0) {
            focusButton = dialogButtonBox.itemAt(dialogButtonBox.count - 1)
        }
        if (focusButton) {
            focusButton.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    RowLayout {
        spacing: Style.px(24, dialog.hostWindow)

        BusyIndicator {
            id: dialogSpinner
            visible: false
            running: visible
        }

        Image {
            id: dialogImage
            source: (standardButtons & Dialog.Yes) ?
                        "qrc:/res/baseline-help_outline-24px.svg" :
                        "qrc:/res/baseline-error_outline-24px.svg"
            sourceSize {
                // The icon should be square so use the height as the width too
                width: 50
                height: 50
            }
            visible: false
        }

        ScrollView {
            id: messageScroll
            Layout.preferredWidth: Math.max(240, Math.min(Style.px(496, dialog.hostWindow), (Overlay.overlay ? Overlay.overlay.width : 800) - 2 * dialog.padding))
            Layout.preferredHeight: Math.min(dialogLabel.implicitHeight, Math.max(120, (Overlay.overlay ? Overlay.overlay.height : 720) - Style.px(240, dialog.hostWindow)))
            clip: true
            ConsoleText {
                property string dialogText
                id: dialogLabel
                width: messageScroll.availableWidth
                text: dialogText + ((helpText && (standardButtons & Dialog.Help)) ? (helpTextSeparator + helpText) : "")
                wrapMode: Text.Wrap
                activeFocusOnTab: implicitHeight > messageScroll.height
                Keys.onUpPressed: messageScroll.ScrollBar.vertical.decrease()
                Keys.onDownPressed: messageScroll.ScrollBar.vertical.increase()
            }
        }
    }

    footer: DialogButtonBox {
        id: dialogButtonBox
        standardButtons: dialog.standardButtons
        spacing: Style.px(12, dialog.hostWindow)
        padding: Style.px(32, dialog.hostWindow)
        topPadding: 0
        bottomPadding: Style.px(32, dialog.hostWindow)
        background: Item { }

        delegate: ConsoleButton {

            Keys.onRightPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
            Keys.onLeftPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
        }

        onHelpRequested: {
            Qt.openUrlExternally(helpUrl)
            close()
        }
    }
}
