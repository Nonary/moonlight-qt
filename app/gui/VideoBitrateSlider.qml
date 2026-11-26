import QtQuick.Controls 2.5
import StreamingPreferences 1.0

ConsoleSlider {
    readonly property bool pyroWave: StreamingPreferences.videoCodecConfig === StreamingPreferences.VCC_FORCE_PYROWAVE
    from: pyroWave ? 5000 : 500
    to: pyroWave ? 3000000 : (StreamingPreferences.unlockBitrate ? 500000 : 150000)
    stepSize: pyroWave ? 5000 : 500
    snapMode: Slider.SnapOnRelease
}
