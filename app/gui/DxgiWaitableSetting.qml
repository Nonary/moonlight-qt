import QtQuick 2.9
import QtQuick.Controls 2.2

CheckBox {
    property bool windowsPlatform: Qt.platform.os === "windows"
    property bool vrrEnabled: false
    property bool vsyncEnabled: false
    visible: windowsPlatform && vrrEnabled
    enabled: windowsPlatform && vrrEnabled && vsyncEnabled
    hoverEnabled: true
    text: qsTr("Experimental DXGI waitable pacing")
    font.pointSize: 12
    ToolTip.delay: 1000
    ToolTip.timeout: 10000
    ToolTip.visible: hovered
    ToolTip.text: qsTr("Selects DXGI instead of Windows composition presentation for supported D3D11 VRR streams. Waits for room in the presentation queue before rendering, with a two-frame limit. Experimental: may change smoothness and latency. Keeps the existing tearing protection; the wait does not measure when a frame reaches the display. Unsupported systems keep normal DXGI pacing.") + "\n\n" + qsTr("Reconnect the stream after changing this setting.")
}
