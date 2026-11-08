.pragma library

// Menus may navigate before they close; only dialogs need to hold focus
// while the underlying view changes.
var visibleDialogs = []
var restoringModalFocus = false

function setDialogVisible(dialog, visible) {
    var index = visibleDialogs.indexOf(dialog)
    if (visible && index === -1) {
        visibleDialogs.push(dialog)
    }
    else if (!visible && index !== -1) {
        visibleDialogs.splice(index, 1)
    }
}

function hasVisibleModal() {
    for (var i = 0; i < visibleDialogs.length; i++) {
        if (visibleDialogs[i] && visibleDialogs[i].visible && visibleDialogs[i].modal) return true
    }
    return false
}

// Back navigation must dismiss the most recently opened dialog before it
// changes the underlying view. Reject it so dialog-specific cancellation runs.
function rejectTopModal() {
    for (var i = visibleDialogs.length - 1; i >= 0; i--) {
        var dialog = visibleDialogs[i]
        if (dialog && dialog.visible && dialog.modal) {
            dialog.reject()
            return true
        }
    }
    return false
}

function keepModalFocus(overlay) {
    if (restoringModalFocus || !overlay) return

    var topDialog = null
    for (var i = visibleDialogs.length - 1; i >= 0; i--) {
        var candidate = visibleDialogs[i]
        if (candidate && candidate.visible && candidate.opened && candidate.modal) {
            topDialog = candidate
            break
        }
    }
    if (!topDialog || topDialog.activeFocus) return

    // Do not leave focus on a dialog hidden behind the topmost modal.
    var olderDialogHasFocus = false
    for (i = 0; i < visibleDialogs.length; i++) {
        if (visibleDialogs[i] && visibleDialogs[i] !== topDialog && visibleDialogs[i].activeFocus) {
            olderDialogHasFocus = true
            break
        }
    }

    // A popup above the modal may be its own dropdown or menu. A focused
    // popup below the modal belongs to the background and must not keep focus.
    if (!olderDialogHasFocus) {
        var dialogItem = topDialog.contentItem ? topDialog.contentItem.parent : null
        var dialogIndex = -1
        for (i = 0; i < overlay.children.length; i++) {
            if (overlay.children[i] === dialogItem) {
                dialogIndex = i
                break
            }
        }
        for (i = 0; i < overlay.children.length; i++) {
            var item = overlay.children[i]
            if (item && item.activeFocus &&
                    (dialogIndex === -1 || item.z > dialogItem.z ||
                     (item.z === dialogItem.z && i >= dialogIndex))) {
                return
            }
        }
    }

    restoringModalFocus = true
    try {
        topDialog.forceActiveFocus()
    }
    finally {
        restoringModalFocus = false
    }
}

function focusCurrentView(stackView) {
    if (!stackView.currentItem) {
        stackView.focusViewAfterDialog = false
    }
    else if (hasVisibleModal()) {
        // The dialog will restore focus to the new view when it closes.
        stackView.focusViewAfterDialog = true
    }
    else {
        stackView.focusViewAfterDialog = false
        stackView.currentItem.forceActiveFocus()
    }
}

function restoreViewFocus(stackView) {
    if (hasVisibleModal()) return
    if (stackView.focusViewAfterDialog && stackView.currentItem) {
        stackView.focusViewAfterDialog = false
        stackView.currentItem.forceActiveFocus()
    }
    else {
        stackView.forceActiveFocus()
    }
}
