#include "gamestreamingsettings.h"
#include "backend/profilemanager.h"

#include <QDebug>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>

namespace {
QStringList groupKeys(const QString& key)
{
    if (key == "width") return {"width", "height"};
    if (key == "bitrate") return {"autoadjustbitrate", "bitrate"};
    return {key};
}
}

QString GameStreamingSettings::settingsGroup(const QString& profileId, const QString& hostUuid, int appId)
{
    static const QRegularExpression segment("\\A[a-zA-Z0-9_{}-]+\\z");
    if (!segment.match(profileId).hasMatch() || !segment.match(hostUuid).hasMatch() || appId <= 0) return {};
    return QString("gameStreamingSettings/%1/%2").arg(hostUuid).arg(appId);
}

QVariantMap GameStreamingSettings::load(const QString& profileId, const QString& hostUuid, int appId)
{
    const auto group = settingsGroup(profileId, hostUuid, appId);
    if (group.isEmpty()) return {};
    QSettings settings;
    ProfileManager::beginProfileSettings(settings, profileId);
    settings.beginGroup(group);
    QVariantMap values;
    for (const auto& key : settings.childKeys()) values.insert(key, settings.value(key));
    return StreamingPreferences::validatedGameValues(values);
}

void GameStreamingSettings::apply(StreamingPreferences& preferences, const QVariantMap& overrides)
{
    const bool wasPyroWave = preferences.videoCodecConfig == StreamingPreferences::VCC_FORCE_PYROWAVE;
    preferences.applyGameValues(overrides);
    const bool isPyroWave = preferences.videoCodecConfig == StreamingPreferences::VCC_FORCE_PYROWAVE;
    const bool videoChanged = overrides.contains("width") || overrides.contains("fps") ||
        overrides.contains("yuv444") || overrides.contains("autoadjustbitrate") ||
        (overrides.contains("videocfg") && (wasPyroWave || isPyroWave)) ||
        (overrides.contains("hdr") && isPyroWave);
    // Unrelated overrides must not normalize an otherwise unchanged profile.
    if (preferences.autoAdjustBitrate && videoChanged) {
        preferences.bitrateKbps = preferences.getEffectiveDefaultBitrate();
    }
}

std::unique_ptr<StreamingPreferences> GameStreamingSettings::resolve(const StreamingPreferences& base,
    const QString& profileId, const QString& hostUuid, int appId)
{
    auto preferences = base.createTransientCopy();
    apply(*preferences, load(profileId, hostUuid, appId));
    return preferences;
}

GameStreamingSettings::GameStreamingSettings(QString profileId, QString hostUuid, int appId, QObject* parent)
    : QObject(parent), m_ProfileId(profileId), m_HostUuid(hostUuid), m_AppId(appId),
      m_Valid(!settingsGroup(profileId, hostUuid, appId).isEmpty()),
      m_Base(StreamingPreferences::get()->createTransientCopy()), m_Draft(m_Base->createTransientCopy()),
      m_Overrides(load(profileId, hostUuid, appId)), m_SavedOverrides(m_Overrides)
{
    refreshDraft();
    const auto observer = metaObject()->method(metaObject()->indexOfSlot("observeChanges()"));
    QSet<int> connected;
    for (int i = StreamingPreferences::staticMetaObject.propertyOffset(); i < m_Draft->metaObject()->propertyCount(); ++i) {
        const auto property = m_Draft->metaObject()->property(i);
        if (property.hasNotifySignal() && !connected.contains(property.notifySignalIndex())) {
            connect(m_Draft.get(), property.notifySignal(), this, observer);
            connected.insert(property.notifySignalIndex());
        }
    }
}

void GameStreamingSettings::observeChanges()
{
    const auto current = m_Draft->gameValues();
    for (auto it = current.cbegin(); it != current.cend(); ++it) {
        if (it.value() != m_LastObserved.value(it.key())) m_EditedFields.insert(it.key());
    }
    m_LastObserved = current;
    emit draftChanged();
}

QVariantMap GameStreamingSettings::pendingOverrides() const
{
    auto pending = m_Overrides;
    const auto current = m_Draft->gameValues();
    const auto base = m_Base->gameValues();
    for (const auto& group : StreamingPreferences::gameSettingGroups()) {
        const auto key = group.toMap().value("key").toString();
        const auto keys = groupKeys(key);
        bool changed = false, matchesProfile = true;
        for (const auto& member : keys) {
            // Calculated bitrate is derived, never a fixed-value override.
            if (member == "bitrate" && current.value("autoadjustbitrate").toBool()) continue;
            changed |= m_EditedFields.contains(member) || current.value(member) != m_Baseline.value(member);
            matchesProfile &= current.value(member) == base.value(member);
        }
        if (!changed) continue;
        for (const auto& member : keys) {
            pending.remove(member);
            if (!matchesProfile && !(member == "bitrate" && current.value("autoadjustbitrate").toBool()))
                pending.insert(member, current.value(member));
        }
    }
    return StreamingPreferences::validatedGameValues(pending);
}

QVariantList GameStreamingSettings::customSettings() const
{
    const auto pending = pendingOverrides();
    QVariantList groups;
    for (const auto& group : StreamingPreferences::gameSettingGroups()) {
        for (const auto& key : groupKeys(group.toMap().value("key").toString())) {
            if (pending.contains(key)) {
                groups.append(group);
                break;
            }
        }
    }
    return groups;
}

void GameStreamingSettings::refreshDraft()
{
    const QSignalBlocker blocker(m_Draft.get());
    m_Draft->applyGameValues(m_Base->gameValues());
    apply(*m_Draft, m_Overrides);
    m_Baseline = m_Draft->gameValues();
    m_LastObserved = m_Baseline;
    m_EditedFields.clear();
    emit draftChanged();
}

void GameStreamingSettings::reset(const QString& key)
{
    m_Overrides = pendingOverrides();
    if (key.isEmpty()) m_Overrides.clear();
    else for (const auto& member : groupKeys(key)) m_Overrides.remove(member);
    refreshDraft();
}

bool GameStreamingSettings::save()
{
    if (!m_Valid || ProfileManager::activeProfileId() != m_ProfileId) return false;
    const auto pending = pendingOverrides();
    if (pending == m_SavedOverrides) return true;
    QSettings settings;
    ProfileManager::beginProfileSettings(settings, m_ProfileId);
    const auto group = settingsGroup(m_ProfileId, m_HostUuid, m_AppId);
    settings.remove(group);
    settings.beginGroup(group);
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) settings.setValue(it.key(), it.value());
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        qWarning() << "Unable to save per-game streaming settings";
        return false;
    }
    m_Overrides = m_SavedOverrides = pending;
    m_Baseline = m_Draft->gameValues();
    m_LastObserved = m_Baseline;
    m_EditedFields.clear();
    emit saved();
    emit draftChanged();
    return true;
}

bool GameStreamingSettings::remove(const QString& profileId, const QString& hostUuid, int appId)
{
    const auto group = settingsGroup(profileId, hostUuid, appId);
    if (group.isEmpty()) return false;
    QSettings settings;
    ProfileManager::beginProfileSettings(settings, profileId);
    settings.remove(group);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void GameStreamingSettings::removeHost(const QString& profileId, const QString& hostUuid)
{
    if (settingsGroup(profileId, hostUuid, 1).isEmpty()) return;
    QSettings settings;
    ProfileManager::beginProfileSettings(settings, profileId);
    settings.remove("gameStreamingSettings/" + hostUuid);
}
