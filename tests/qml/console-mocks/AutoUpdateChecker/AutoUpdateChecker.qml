pragma Singleton
import QtQuick 2.9
QtObject {
    signal updateAvailable(string version, string url)
    function start() {}
    function dismissUpdate(version) {}
 property bool notifyPrereleases: false }
