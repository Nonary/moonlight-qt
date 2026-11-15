pragma Singleton
import QtQml 2.2

// The input tests send the same Qt keys as the SDL navigation bridge.
QtObject {
    property bool uiNavMode: true
    property string inputMethod: "keyboard"
    function currentUiNavMode() { return uiNavMode }
    function setUiNavMode(enabled) { uiNavMode = enabled }
}
