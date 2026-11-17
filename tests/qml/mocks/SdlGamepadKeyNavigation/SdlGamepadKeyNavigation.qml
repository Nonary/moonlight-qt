pragma Singleton
import QtQuick 2.9
QtObject {
    property bool uiNavMode: true
    property string inputMethod: "keyboard"
    function currentUiNavMode() { return uiNavMode }
    function setUiNavMode(value) { uiNavMode = value }
}
