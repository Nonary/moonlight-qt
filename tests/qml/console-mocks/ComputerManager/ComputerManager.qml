pragma Singleton
import QtQuick 2.9
QtObject {
    signal computerAddCompleted(bool success, bool portBlocking)
    signal quitAppCompleted(var error)
    signal manualHostRequested(string address)
    function addNewHostManually(address) { manualHostRequested(address) }
 function startPolling() {} function stopPollingAsync() {} }
