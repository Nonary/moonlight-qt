import QtQuick 2.9

ListModel {
    signal computerLost()
    function initialize(manager, computerIndex, showHidden) {
        append({ appid: 1, name: "Test Game", running: false, hidden: false,
                 directLaunch: false, isAppCollectorGame: false, customStreamingSettings: false,
                 boxart: "qrc:/res/no_app_image.png" })
    }
    function getDirectLaunchAppIndex() { return -1 }
    function indexOfApp(appId) {
        for (var i = 0; i < count; i++) if (get(i).appid === appId) return i
        return -1
    }
    function createGameSettings(appId) { return gameSettingsFactory.create(appId, this) }
    function removeGameSettings(appId) {
        var result = gameSettingsFactory.remove(appId)
        setProperty(indexOfApp(appId), "customStreamingSettings", false)
        return result
    }
}
