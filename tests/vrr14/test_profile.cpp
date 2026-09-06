#include "profile.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QDateTime>
#include <cassert>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    assert(temp.isValid());
    const auto path = temp.filePath("profiles.json");
    Vrr::Reserve learned, restored;
    assert(!Vrr::saveProfile(path, "short-session", learned) && !QFile::exists(path));
    for (int i = 0; i < 50000; ++i) learned.observe(1000000, 1500000);
    assert(Vrr::saveProfile(path, "profile-a", learned, Vrr::PresentationValidation::Passed));
    assert(!Vrr::saveProfile(path, "profile-a", learned)); // Rate-limit repeated renderer restarts.
    assert(Vrr::loadProfile(path, "profile-a", restored));
    assert(!restored.observations() && restored.successes() == 1);
    const auto loadedWords = restored.profile(), learnedWords = learned.profile();
    assert(std::equal(loadedWords.begin() + 4, loadedWords.end(), learnedWords.begin() + 4));
    assert(!Vrr::loadProfile(path, "wrong-output", restored));
    assert(restored.cachedEvidence() > 35000 && !restored.reliable());
    const auto cachedEvidence = restored.cachedEvidence();
    for (unsigned i = 0; i < 36100; ++i) restored.observe(1000000, 17000000);
    assert(restored.reliable() && restored.target(100000000, 16666667) == 0);
    assert(restored.evidence() > 35000 && restored.cachedEvidence() == cachedEvidence); // Live window replaces the cache.

    {
        QLockFile lock(path + ".lock");
        assert(lock.tryLock(0));
        assert(!Vrr::saveProfile(path, "locked", learned));
    }
    for (int i = 0; i < 20; ++i) assert(Vrr::saveProfile(path, QString::number(i), learned));
    QFile file(path);
    assert(file.open(QIODevice::ReadOnly) && file.size() < 262144);
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    assert(root["profiles"].toObject().size() == 16);
    file.close();
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QByteArray(262145, 'x')); file.close();
    assert(!Vrr::loadProfile(path, "profile-a", restored));
    assert(Vrr::saveProfile(path, "recovered", learned));
    assert(Vrr::loadProfile(path, "recovered", restored));
    assert(file.open(QIODevice::ReadOnly));
    root = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    auto profiles = root["profiles"].toObject();
    auto expired = profiles["recovered"].toObject();
    expired["updated"] = QDateTime::currentSecsSinceEpoch() - 15 * 86400;
    profiles["recovered"] = expired; root["profiles"] = profiles;
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(root).toJson()); file.close();
    assert(!Vrr::loadProfile(path, "recovered", restored));
    expired["updated"] = QDateTime::currentSecsSinceEpoch() - 2 * 86400;
    profiles["recovered"] = expired; root["profiles"] = profiles;
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(root).toJson()); file.close();
    assert(Vrr::loadProfile(path, "recovered", restored));
    assert(restored.cachedEvidence() <= learned.evidence() / 4);
    assert(restored.common() == learned.common() && !restored.reliable());
    assert(restored.target(100000000, 16666667) == 16666667);

    // Distinct successful sessions establish a repeatable startup reserve.
    // Advance stored wall-clock metadata rather than sleeping through rate limits.
    const auto provenPath = temp.filePath("proven.json");
    auto allowNextSave = [&] {
        QFile f(provenPath); assert(f.open(QIODevice::ReadOnly));
        auto doc = QJsonDocument::fromJson(f.readAll()).object(); f.close();
        auto entries = doc["profiles"].toObject(); auto entry = entries["same-route"].toObject();
        entry["updated"] = QDateTime::currentSecsSinceEpoch() - 61;
        entries["same-route"] = entry; doc["profiles"] = entries;
        assert(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(doc).toJson());
    };
    for (int session = 0; session < 3; ++session) {
        Vrr::Reserve run;
        if (session) assert(Vrr::loadProfile(provenPath, "same-route", run));
        for (int i = 0; i < 37000; ++i)
            run.observe(1000000, 3500000, (1 + session * 1000LL) * Vrr::Reserve::Second + int64_t(i) * 8333333);
        if (!session) {
            assert(Vrr::saveProfile(provenPath, "without-native-evidence", run));
            Vrr::Reserve unproven;
            assert(Vrr::loadProfile(provenPath, "without-native-evidence", unproven));
            assert(unproven.cachedEvidence() > 35000 && unproven.successes() == 0);
        }
        assert(Vrr::saveProfile(provenPath, "same-route", run, Vrr::PresentationValidation::Passed));
        Vrr::Reserve check; assert(Vrr::loadProfile(provenPath, "same-route", check));
        assert(check.successes() == unsigned(session + 1));
        assert(Vrr::saveProfile(provenPath, QString("failed-presentation-%1").arg(session), run,
            Vrr::PresentationValidation::Failed));
        Vrr::Reserve failed;
        assert(Vrr::loadProfile(provenPath, QString("failed-presentation-%1").arg(session), failed));
        assert(failed.successes() == 0);
        allowNextSave();
        // Saving the identical session again must not fabricate another success.
        assert(Vrr::saveProfile(provenPath, "same-route", run, Vrr::PresentationValidation::Passed));
        assert(Vrr::loadProfile(provenPath, "same-route", check));
        assert(check.successes() == unsigned(session + 1));
        allowNextSave();
    }
    Vrr::Reserve proven;
    assert(Vrr::loadProfile(provenPath, "same-route", proven));
    assert(proven.cachedEvidence() > 35000);
    assert(proven.target(100000000, 16666667) == 3500000);
    proven.observe(12000000, 1500000, 4000LL * Vrr::Reserve::Second);
    assert(proven.target(100000000, 16666667) >= 16666667);
    Vrr::Reserve replay;
    assert(replay.restore(proven.checkpoint()) && replay.checkpoint() == proven.checkpoint());
    for (int i = 1; i < 240; ++i) proven.observe(12000000, 1500000, 4000LL * Vrr::Reserve::Second + int64_t(i) * 8333333);
    assert(Vrr::saveProfile(provenPath, "same-route", proven));
    assert(Vrr::loadProfile(provenPath, "same-route", replay) && replay.successes() == 0);

    // Without a mandatory cushion, zero additional reserve is a valid proven
    // start. Count the timing guard for misses, but never add it again in cache.
    for (unsigned session = 0; session < 3; ++session) {
        allowNextSave();
        Vrr::Reserve run;
        assert(Vrr::loadProfile(provenPath, "same-route", run));
        for (int i = 0; i < 37000; ++i)
            run.observe(0, 50000, (10000 + session * 1000LL) * Vrr::Reserve::Second + int64_t(i) * 8333333, 50000);
        assert(run.successfulBuffer() == 0);
        assert(Vrr::saveProfile(provenPath, "same-route", run, Vrr::PresentationValidation::Passed));
        assert(Vrr::loadProfile(provenPath, "same-route", replay));
        assert(replay.successes() == session + 1 && replay.provenBuffer() == 0);
    }
    assert(replay.target(100000000, 16666667) == 0);
    auto staleModel = replay.profile(); staleModel[0] = 11;
    assert(!replay.loadProfile(staleModel));

    // Recovery headroom is available timing budget, never proven queued delay.
    Vrr::Reserve withHeadroom;
    for (int i = 0; i < 37000; ++i)
        withHeadroom.observe(6000000, 10050000,
            Vrr::Reserve::Second + int64_t(i) * 8333333, 10050000);
    assert(withHeadroom.reliable() && withHeadroom.successfulBuffer() == 0);
    assert(withHeadroom.target(100000000, 8333333, 10000000) == 0);
    assert(replay.loadProfile(withHeadroom.profile()));
    assert(replay.common() == 6000000); // Preserve raw evidence for other FPS.

    std::cout << "Bounded reserve profiles, isolation, lock contention, warm start and corrupt-cache recovery passed\n";
}
