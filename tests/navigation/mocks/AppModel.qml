import QtQuick 2.9

ListModel {
    signal computerLost()
    function initialize(manager, computerIndex, showHidden) {
        append({ appid: 1, name: "Test Game", running: false, hidden: false,
                 directLaunch: false, isAppCollectorGame: false,
                 boxart: "qrc:/res/no_app_image.png" })
    }
    function getDirectLaunchAppIndex() { return -1 }
}
