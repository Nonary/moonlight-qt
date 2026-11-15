pragma Singleton
import QtQuick 2.9
QtObject {
    enum Quality { Minimum, Moderate, Recommended, Maximum }
    property bool running: false
    property bool bandwidthReady: false
    property int bandwidthKbps: 0
    property int bandwidthQuality: 0
    property int bandwidthTargetKbps: 0
    property int paceMbps: 0
    property string linkSummary: ""
    property string message: ""
    property var results: []
    signal changed()
    function cancel() {} function reset() {} function start() {} function startDecoderTest() {}
}
