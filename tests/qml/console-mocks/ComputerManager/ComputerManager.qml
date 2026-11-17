pragma Singleton
import QtQuick 2.9
QtObject {
    signal computerAddCompleted(bool success, bool portBlocking)
    signal quitAppCompleted(var error)
    function addNewHostManually(address) {}
 function startPolling() {} function stopPollingAsync() {} }
