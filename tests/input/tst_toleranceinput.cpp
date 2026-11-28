#include <QFileInfo>
#include <QDir>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QtTest>
#include <memory>

class ToleranceInputTest : public QObject {
    Q_OBJECT
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QQuickItem> root;
    QQuickItem* tolerance = nullptr;
    QQuickItem* editor = nullptr;
    QObject* settings = nullptr;
    void key(QEvent::Type type, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
             bool repeat = false) {
        QKeyEvent event(type, code, modifiers, QString(), repeat);
        QCoreApplication::sendEvent(&window, &event);
    }
    int value() { return settings->property("toleranceUs").toInt(); }
private slots:
    void initTestCase() {
        const QString testPath = QFileInfo(QDir(QString::fromUtf8(QT_TESTCASE_BUILDDIR)),
                                          QString::fromUtf8(__FILE__)).absolutePath();
        engine.addImportPath(testPath + "/../qml/mocks");
        QQmlComponent component(&engine);
        component.setData(QString(R"(
            import QtQuick
            import QtQuick.Controls
            import "../../app/gui"
            Item {
                width: 640; height: 900
                property int leakedPresses: 0
                property int leakedReleases: 0
                Keys.onPressed: function(event) { leakedPresses++ }
                Keys.onReleased: function(event) { leakedReleases++ }
                VrrTimingSettings {
                    objectName: "settings"; width: 420
                    onToleranceEdited: function(value) { toleranceUs = value }
                }
            }
        )").toUtf8(), QUrl::fromLocalFile(testPath + "/fixture.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        root.reset(qobject_cast<QQuickItem*>(component.create()));
        QVERIFY2(root != nullptr, qPrintable(component.errorString()));
        root->setParentItem(window.contentItem());
        settings = root->findChild<QObject*>("settings");
        QVERIFY(settings);
        tolerance = root->findChild<QQuickItem*>("vrrTolerance");
        QVERIFY(tolerance);
        editor = qvariant_cast<QQuickItem*>(tolerance->property("contentItem"));
        QVERIFY(editor);
        window.resize(640, 900);
        window.show();
        QTest::qWait(30);
        QCOMPARE(window.rendererInterface()->graphicsApi(), QSGRendererInterface::Software);
    }
    void init() {
        settings->setProperty("toleranceUs", 1000);
        root->setProperty("leakedPresses", 0);
        root->setProperty("leakedReleases", 0);
        editor->forceActiveFocus();
        QVERIFY(editor->hasActiveFocus());
    }
    void focusedEditorConsumesWholeTap() {
        QSignalSpy edits(settings, SIGNAL(toleranceEdited(int)));
        key(QEvent::KeyPress, Qt::Key_Left);
        QCOMPARE(value(), 950);
        QCOMPARE(edits.count(), 1);
        key(QEvent::KeyRelease, Qt::Key_Left);
        QCOMPARE(edits.count(), 1);
        QCOMPARE(root->property("leakedPresses").toInt(), 0);
        QCOMPARE(root->property("leakedReleases").toInt(), 0);
    }
    void spinboxFocusAndHeldModifiers() {
        tolerance->forceActiveFocus();
        key(QEvent::KeyPress, Qt::Key_Right, Qt::ControlModifier | Qt::ShiftModifier);
        key(QEvent::KeyRelease, Qt::Key_Right, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(value(), 1050);
        QCOMPARE(root->property("leakedPresses").toInt(), 0);
        QCOMPARE(root->property("leakedReleases").toInt(), 0);
    }
    void rapidAdjustmentsAndRepeat() {
        QSignalSpy edits(settings, SIGNAL(toleranceEdited(int)));
        for (int i = 0; i < 3; ++i) {
            key(QEvent::KeyPress, Qt::Key_Left);
            key(QEvent::KeyRelease, Qt::Key_Left);
        }
        QCOMPARE(value(), 850);
        QCOMPARE(edits.count(), 3);
        // Keyboard auto-repeat is one bounded adjustment per delivered press.
        key(QEvent::KeyPress, Qt::Key_Right);
        key(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier, true);
        key(QEvent::KeyRelease, Qt::Key_Right);
        QCOMPARE(value(), 950);
        QCOMPARE(edits.count(), 5);
        QCOMPARE(root->property("leakedPresses").toInt(), 0);
        QCOMPARE(root->property("leakedReleases").toInt(), 0);
    }
    void releaseOrderAndFocusChange() {
        key(QEvent::KeyRelease, Qt::Key_Left);
        QCOMPARE(value(), 1000);
        key(QEvent::KeyPress, Qt::Key_Left);
        root->forceActiveFocus();
        key(QEvent::KeyRelease, Qt::Key_Left);
        QCOMPARE(value(), 950);
        editor->forceActiveFocus();
        key(QEvent::KeyPress, Qt::Key_Left);
        key(QEvent::KeyRelease, Qt::Key_Left);
        QCOMPARE(value(), 900);
    }
    void adjustmentsStayWithinLimits() {
        for (int i = 0; i < 40; ++i) {
            key(QEvent::KeyPress, Qt::Key_Left);
            key(QEvent::KeyRelease, Qt::Key_Left);
        }
        QCOMPARE(value(), 200);
        for (int i = 0; i < 40; ++i) {
            key(QEvent::KeyPress, Qt::Key_Right);
            key(QEvent::KeyRelease, Qt::Key_Right);
        }
        QCOMPARE(value(), 2000);
        QCOMPARE(root->property("leakedPresses").toInt(), 0);
        QCOMPARE(root->property("leakedReleases").toInt(), 0);
    }
    void ordinaryNavigationRemainsAvailable() {
        key(QEvent::KeyPress, Qt::Key_Tab);
        key(QEvent::KeyRelease, Qt::Key_Tab);
        QCOMPARE(value(), 1000);
        QVERIFY(!tolerance->hasActiveFocus());
        root->forceActiveFocus();
        key(QEvent::KeyPress, Qt::Key_Left);
        key(QEvent::KeyRelease, Qt::Key_Left);
        QCOMPARE(value(), 1000);
        QVERIFY(root->property("leakedPresses").toInt() > 0);
    }
};

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    ToleranceInputTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_toleranceinput.moc"
