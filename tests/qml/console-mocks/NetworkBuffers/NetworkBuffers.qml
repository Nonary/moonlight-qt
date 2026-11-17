pragma Singleton
import QtQuick 2.9
QtObject {
    property bool busy: false
    property bool canApply: false
    property bool needsFix: false
    property string copyCommand: ""
    property string fixDescription: ""
    property string manualCommand: ""
    property string manualHint: ""
    property string message: "Ready"
    property string problemText: ""
    function apply() {} function refresh() {}
}
