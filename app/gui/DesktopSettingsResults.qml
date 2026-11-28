import QtQuick 2.12
import QtQuick.Controls 2.5

ListView {
    id: list
    property var catalog: []
    property string query
    signal picked(var entry)
    function matches() {
        var terms = query.trim().toLocaleLowerCase().split(/\s+/)
        if (!terms[0]) return []
        var names = [], descriptions = []
        for (var i = 0; i < catalog.length; ++i) {
            var entry = catalog[i]
            var title = entry.label.toLocaleLowerCase()
            var text = (title + " " + entry.group + " " + entry.help).toLocaleLowerCase()
            if (terms.every(function(term) { return text.indexOf(term) >= 0 }))
                (terms.every(function(term) { return title.indexOf(term) >= 0 }) ? names : descriptions).push(entry)
        }
        return names.concat(descriptions)
    }
    model: matches()
    onModelChanged: currentIndex = count ? 0 : -1
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}
    function step(delta) {
        currentIndex = Math.max(0, Math.min(count - 1, currentIndex + delta))
        positionViewAtIndex(currentIndex, ListView.Contain)
    }
    function choose() { if (model && currentIndex >= 0 && currentIndex < count) picked(model[currentIndex]) }
    header: Label {
        width: list.width
        height: 40
        text: qsTr("Search results")
        font.pixelSize: 22; font.weight: Font.DemiBold
    }
    delegate: ItemDelegate {
        width: list.width
        height: 58
        text: modelData.label
        highlighted: ListView.isCurrentItem
        Accessible.name: modelData.group + ", " + text
        contentItem: Column {
            spacing: 2
            Text { width: parent.width; text: modelData.label; font.pixelSize: 16; color: "#f2f4f7"; elide: Text.ElideRight }
            Text { width: parent.width; text: modelData.group; font.pixelSize: 13; color: "#b8c1cb"; elide: Text.ElideRight }
        }
        onClicked: list.picked(modelData)
    }
    Label {
        y: 48
        visible: !list.count
        width: parent.width
        wrapMode: Text.Wrap
        text: qsTr("No settings match “%1”.").arg(list.query)
    }
}
