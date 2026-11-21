.pragma library
.import QtQml 2.2 as Qml
var instances = []
function openFor(field, window, numeric, commit) {
    if (!window) return
    var keyboard = null
    for (var i = 0; i < instances.length; ++i) if (instances[i].window === window) keyboard = instances[i].keyboard
    if (!keyboard) {
        var component = Qt.createComponent(Qt.resolvedUrl("ConsoleKeyboard.qml"))
        if (component.status !== Qml.Component.Ready) { console.error(component.errorString()); return }
        keyboard = component.createObject(window.contentItem, {"hostWindow": window})
        instances.push({window: window, keyboard: keyboard})
        keyboard.released.connect(function() {
            for (var j = instances.length - 1; j >= 0; --j) if (instances[j].window === window) instances.splice(j,1)
        })
    }
    keyboard.commitHandler = commit || null
    keyboard.openFor(field, numeric)
}
