import QtQuick 2.9

ListModel {
    signal pairingCompleted(string error)
    signal connectionTestCompleted(int result, string blockedPorts)
    function initialize(manager) {
        append({ uuid: "pc-1", name: "Test PC", online: true, paired: true,
                 statusUnknown: false, serverSupported: true, wakeable: true, details: "" })
        append({ uuid: "pc-2", name: "Other PC", online: true, paired: true,
                 statusUnknown: false, serverSupported: true, wakeable: true, details: "" })
    }
}
