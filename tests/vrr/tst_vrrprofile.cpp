#include "assertions.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/profilecodec.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDateTime>
#include <QJsonObject>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    assert(dir.isValid());
    const auto path = dir.filePath("profiles.json");
    Vrr13::Reserve history;
    for (int i = 0; i < 400; ++i)
        history.observe(i % 30 ? 1000000 : 8000000, 9000000,
                        Vrr13::Reserve::Second + int64_t(i) * 16667000);
    assert(!Vrr13::saveProfile(path, "", history));
    assert(Vrr13::saveProfile(path, "workload-A", history));
    Vrr13::Reserve loaded;
    assert(!Vrr13::loadProfile(path, "workload-B", loaded));
    assert(Vrr13::loadProfile(path, "workload-A", loaded));
    assert(loaded.common() == history.common());
    assert(loaded.evidence() == 0 && loaded.successes() == 0 && !loaded.canRelease());
    std::vector<int64_t> words;
    const auto encoded = encodeVrrPlayoutProfile(loaded);
    assert(decodeVrrPlayoutProfile(encoded, words));
    Vrr13::Reserve trace;
    assert(trace.loadProfile(words) && trace.profile() == loaded.profile());
    assert(!decodeVrrPlayoutProfile(encoded + "!", words));
    assert(!decodeVrrPlayoutProfile(QByteArray(17000, 'A'), words));

    QFile file(path);
    assert(file.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    auto profiles = root["profiles"].toObject();
    auto entry = profiles["workload-A"].toObject();
    entry["updated"] = QDateTime::currentSecsSinceEpoch() - 15 * 86400;
    profiles["workload-A"] = entry;
    root["profiles"] = profiles;
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(root).toJson());
    file.close();
    assert(!Vrr13::loadProfile(path, "workload-A", loaded));
    std::cout << "Profile isolation, aging, validation and trace round-trip passed\n";
}
