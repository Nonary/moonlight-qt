#include "profilemanager.h"

#include "identitymanager.h"
#include "path.h"
#include "settings/streamingpreferences.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <QVariantMap>
#include <QDebug>

#define SER_PROFILE_MANAGER "profilemanager"
#define SER_PROFILE_LIST "profiles"
#define SER_PROFILE_ID "id"
#define SER_PROFILE_NAME "name"
#define SER_PROFILE_ICON "icon"
#define SER_PROFILE_CREATED "created"
#define SER_PROFILE_UPDATED "updated"
#define SER_DEFAULT_PROFILE "defaultProfile"
#define SER_AUTOLOGIN_PROFILE "autoLoginProfile"
#define SER_PROFILE_DATA "profiles"

ProfileManager* ProfileManager::s_Pm = nullptr;
QString ProfileManager::s_ActiveProfileId;

static QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

ProfileManager*
ProfileManager::get()
{
    if (s_Pm == nullptr) {
        s_Pm = new ProfileManager();
    }
    return s_Pm;
}

QString
ProfileManager::activeProfileId()
{
    return s_ActiveProfileId;
}

bool
ProfileManager::hasActiveProfile()
{
    return !s_ActiveProfileId.isEmpty();
}

QString
ProfileManager::profileSettingsGroup(QString profileId)
{
    return QString(SER_PROFILE_DATA "/%1").arg(profileId);
}

void
ProfileManager::beginProfileSettings(QSettings& settings)
{
    Q_ASSERT(hasActiveProfile());
    beginProfileSettings(settings, s_ActiveProfileId);
}

void
ProfileManager::beginProfileSettings(QSettings& settings, QString profileId)
{
    Q_ASSERT(!profileId.isEmpty());
    settings.beginGroup(profileSettingsGroup(profileId));
}

ProfileManager::ProfileManager(QObject* parent)
    : QObject(parent)
{
    loadProfiles();
    ensureProfilesExist();
}

void
ProfileManager::loadProfiles()
{
    QSettings settings;
    settings.beginGroup(SER_PROFILE_MANAGER);

    m_DefaultProfileId = settings.value(SER_DEFAULT_PROFILE).toString();
    m_AutoLoginProfileId = settings.value(SER_AUTOLOGIN_PROFILE).toString();

    int profileCount = settings.beginReadArray(SER_PROFILE_LIST);
    for (int i = 0; i < profileCount; i++) {
        settings.setArrayIndex(i);

        Profile profile;
        profile.id = settings.value(SER_PROFILE_ID).toString();
        profile.name = settings.value(SER_PROFILE_NAME).toString();
        profile.iconId = settings.value(SER_PROFILE_ICON).toString();
        profile.createdAt = settings.value(SER_PROFILE_CREATED).toString();
        profile.updatedAt = settings.value(SER_PROFILE_UPDATED).toString();

        if (!profile.id.isEmpty() && !profile.name.isEmpty()) {
            m_Profiles.append(profile);
        }
    }
    settings.endArray();
    settings.endGroup();
}

void
ProfileManager::saveProfiles()
{
    QSettings settings;
    settings.beginGroup(SER_PROFILE_MANAGER);

    settings.setValue(SER_DEFAULT_PROFILE, m_DefaultProfileId);
    settings.setValue(SER_AUTOLOGIN_PROFILE, m_AutoLoginProfileId);

    settings.beginWriteArray(SER_PROFILE_LIST);
    for (int i = 0; i < m_Profiles.count(); i++) {
        settings.setArrayIndex(i);
        settings.setValue(SER_PROFILE_ID, m_Profiles[i].id);
        settings.setValue(SER_PROFILE_NAME, m_Profiles[i].name);
        settings.setValue(SER_PROFILE_ICON, m_Profiles[i].iconId);
        settings.setValue(SER_PROFILE_CREATED, m_Profiles[i].createdAt);
        settings.setValue(SER_PROFILE_UPDATED, m_Profiles[i].updatedAt);
    }
    settings.endArray();

    settings.endGroup();
}

void
ProfileManager::ensureProfilesExist()
{
    if (!m_Profiles.isEmpty()) {
        bool profilesChanged = false;

        if (profileIndex(m_DefaultProfileId) < 0) {
            m_DefaultProfileId = m_Profiles.first().id;
            profilesChanged = true;
        }
        if (!m_AutoLoginProfileId.isEmpty() && profileIndex(m_AutoLoginProfileId) < 0) {
            m_AutoLoginProfileId.clear();
            profilesChanged = true;
        }

        if (!m_AutoLoginProfileId.isEmpty() && m_AutoLoginProfileId != m_DefaultProfileId) {
            m_AutoLoginProfileId = m_DefaultProfileId;
            profilesChanged = true;
        }

        if (profilesChanged) {
            saveProfiles();
        }
        return;
    }

    Profile profile;
    profile.id = "default";
    profile.name = tr("Default");
    profile.createdAt = nowIso();
    profile.updatedAt = profile.createdAt;

    m_Profiles.append(profile);
    m_DefaultProfileId = profile.id;
    m_AutoLoginProfileId.clear();

    migrateLegacyProfileData(profile.id);
    saveProfiles();

    qInfo() << "Created default Moonlight profile";
}

void
ProfileManager::migrateLegacyProfileData(QString profileId)
{
    static const QSet<QString> legacyTopLevelKeys = {
        "uniqueid", "certificate", "key",
        "hosts", "hostsbackup",
        "gcmapping",
        "width", "height", "fps", "bitrate", "unlockbitrate", "autoadjustbitrate",
        "enablevrr", "vrrlatencymode", "vrrlatencyfix", "smoothvrrframetiming", "tracevrrframes",
        "fullscreen", "vsync", "gameopts", "hostaudio", "multicontroller",
        "audiocfg", "videocfg", "hdr", "yuv444", "videodec", "windowmode",
        "mdns", "quitAppAfter", "mouseacceleration", "abstouchmode",
        "startwindowed", "framepacing", "connwarnings", "confwarnings",
        "uidisplaymode", "richpresence", "gamepadmouse", "defaultver",
        "packetsize", "detectnetblocking", "showperfoverlay", "swapmousebuttons",
        "muteonfocusloss", "backgroundgamepad", "reversescroll", "swapfacebuttons",
        "capturesyskeys", "keepawake", "language", "renderer"
    };

    QSettings settings;
    const QString destPrefix = profileSettingsGroup(profileId) + "/";
    const QStringList keys = settings.allKeys();
    bool migratedAny = false;

    for (const QString& key : keys) {
        const QString topLevelKey = key.section('/', 0, 0);
        if (!legacyTopLevelKeys.contains(topLevelKey)) {
            continue;
        }

        settings.setValue(destPrefix + key, settings.value(key));
        migratedAny = true;
    }

    if (migratedAny) {
        qInfo() << "Migrated legacy Moonlight settings into profile" << profileId;
    }

    QDir boxArtRoot(Path::getBoxArtCacheDir());
    if (!boxArtRoot.exists()) {
        return;
    }

    if (!boxArtRoot.exists(profileId)) {
        boxArtRoot.mkdir(profileId);
    }

    const QFileInfoList entries = boxArtRoot.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        if (entry.fileName() == profileId) {
            continue;
        }

        boxArtRoot.rename(entry.fileName(), profileId + "/" + entry.fileName());
    }
}

QVariantList
ProfileManager::profiles() const
{
    QVariantList list;
    for (const Profile& profile : m_Profiles) {
        QVariantMap map;
        map["id"] = profile.id;
        map["name"] = profile.name;
        map["iconId"] = profile.iconId;
        map["createdAt"] = profile.createdAt;
        map["updatedAt"] = profile.updatedAt;
        bool startsAutomatically = !m_AutoLoginProfileId.isEmpty() && profile.id == m_DefaultProfileId;
        map["defaultProfile"] = profile.id == m_DefaultProfileId;
        map["autoLogin"] = startsAutomatically;
        list.append(map);
    }
    return list;
}

QString
ProfileManager::activeProfileIdValue() const
{
    return s_ActiveProfileId;
}

bool
ProfileManager::hasActiveProfileValue() const
{
    return hasActiveProfile();
}

QString
ProfileManager::activeProfileName() const
{
    int index = profileIndex(s_ActiveProfileId);
    return index >= 0 ? m_Profiles[index].name : QString();
}

QString
ProfileManager::defaultProfileId() const
{
    return m_DefaultProfileId;
}

QString
ProfileManager::autoLoginProfileId() const
{
    return m_AutoLoginProfileId;
}

bool
ProfileManager::autoLoginEnabled() const
{
    return !m_AutoLoginProfileId.isEmpty();
}

void ProfileManager::suspendRequests()
{
    m_RequestsAllowed = false;
    emit pendingRequestsCanceled();
}

void ProfileManager::resumeRequests()
{
    m_RequestsAllowed = true;
}

void ProfileManager::deactivateProfile()
{
    suspendRequests();
    if (s_ActiveProfileId.isEmpty()) {
        return;
    }

    emit activeProfileAboutToChange();
    s_ActiveProfileId.clear();
    IdentityManager::reset();
    emit activeProfileChanged();
}

bool
ProfileManager::activateProfile(QString id)
{
    if (profileIndex(id) < 0) {
        qWarning() << "Cannot activate unknown profile" << id;
        return false;
    }

    if (s_ActiveProfileId == id) {
        return true;
    }

    suspendRequests();
    emit activeProfileAboutToChange();
    s_ActiveProfileId = id;

    IdentityManager::reset();
    IdentityManager::get();
    StreamingPreferences::get()->reload();
    resumeRequests();

    qInfo() << "Activated Moonlight profile" << id << activeProfileName();
    emit activeProfileChanged();

    return true;
}

bool
ProfileManager::switchToProfile(QString id)
{
    return activateProfile(id);
}

bool
ProfileManager::activateProfileByNameOrId(QString nameOrId, QString* error)
{
    QString value = nameOrId.trimmed();
    if (value.isEmpty()) {
        if (error) {
            *error = tr("Profile name or ID is empty");
        }
        return false;
    }

    // IDs take precedence over names so every profile can be addressed even
    // when another profile happens to use its ID as a display name.
    for (const Profile& profile : m_Profiles) {
        if (profile.id.compare(value, Qt::CaseInsensitive) == 0) {
            return activateProfile(profile.id);
        }
    }

    int foundIndex = -1;
    for (int i = 0; i < m_Profiles.count(); i++) {
        if (m_Profiles[i].name.compare(value, Qt::CaseInsensitive) == 0) {
            if (foundIndex >= 0) {
                if (error) {
                    *error = tr("More than one profile matches '%1'. Use the profile ID instead.").arg(value);
                }
                return false;
            }
            foundIndex = i;
        }
    }

    if (foundIndex < 0) {
        if (error) {
            *error = tr("Profile '%1' was not found").arg(value);
        }
        return false;
    }

    return activateProfile(m_Profiles[foundIndex].id);
}

bool
ProfileManager::activateDefaultProfile()
{
    if (profileIndex(m_DefaultProfileId) >= 0) {
        return activateProfile(m_DefaultProfileId);
    }
    return !m_Profiles.isEmpty() && activateProfile(m_Profiles.first().id);
}

bool
ProfileManager::activateAutoLoginProfile()
{
    return !m_AutoLoginProfileId.isEmpty() && activateDefaultProfile();
}

QString
ProfileManager::createProfile(QString name)
{
    name = normalizedName(name);
    if (name.isEmpty() || profileNameExists(name)) {
        return QString();
    }

    Profile profile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.name = name;
    profile.createdAt = nowIso();
    profile.updatedAt = profile.createdAt;

    m_Profiles.append(profile);
    saveProfiles();

    emit profilesChanged();
    return profile.id;
}

bool
ProfileManager::renameProfile(QString id, QString name)
{
    int index = profileIndex(id);
    name = normalizedName(name);
    if (index < 0 || name.isEmpty() || profileNameExists(name, id)) {
        return false;
    }

    m_Profiles[index].name = name;
    m_Profiles[index].updatedAt = nowIso();
    saveProfiles();

    emit profilesChanged();
    if (s_ActiveProfileId == id) {
        emit activeProfileChanged();
    }
    return true;
}

bool
ProfileManager::setProfileIcon(QString id, QString iconId)
{
    int index = profileIndex(id);
    if (index < 0) {
        return false;
    }

    m_Profiles[index].iconId = iconId.trimmed();
    m_Profiles[index].updatedAt = nowIso();
    saveProfiles();

    emit profilesChanged();
    return true;
}

bool
ProfileManager::removeProfile(QString id)
{
    int index = profileIndex(id);
    if (index < 0 || m_Profiles.count() <= 1) {
        return false;
    }

    bool removingActiveProfile = s_ActiveProfileId == id;
    if (removingActiveProfile) {
        suspendRequests();
        emit activeProfileAboutToChange();
    }
    bool wasAutoLoginEnabled = autoLoginEnabled();
    m_Profiles.removeAt(index);

    if (m_DefaultProfileId == id) {
        m_DefaultProfileId = m_Profiles.first().id;
    }
    if (wasAutoLoginEnabled) {
        m_AutoLoginProfileId = m_DefaultProfileId;
    }
    else if (m_AutoLoginProfileId == id) {
        m_AutoLoginProfileId.clear();
    }

    QSettings settings;
    settings.remove(profileSettingsGroup(id));

    QDir boxArtRoot(Path::getBoxArtCacheDir());
    QDir profileBoxArt(boxArtRoot.filePath(id));
    if (profileBoxArt.exists()) {
        profileBoxArt.removeRecursively();
    }

    saveProfiles();
    emit profilesChanged();

    if (removingActiveProfile) {
        s_ActiveProfileId.clear();
        IdentityManager::reset();
        emit activeProfileChanged();
    }

    return true;
}

bool
ProfileManager::setDefaultProfile(QString id)
{
    if (profileIndex(id) < 0) {
        return false;
    }

    m_DefaultProfileId = id;
    if (autoLoginEnabled()) {
        m_AutoLoginProfileId = id;
    }
    saveProfiles();
    emit profilesChanged();
    return true;
}

bool
ProfileManager::setAutoLoginProfile(QString id, bool enabled)
{
    if (enabled) {
        if (profileIndex(id) < 0) {
            return false;
        }
        m_DefaultProfileId = id;
        m_AutoLoginProfileId = id;
        saveProfiles();
        emit profilesChanged();
        return true;
    }

    if (!id.isEmpty() && id != m_DefaultProfileId && id != m_AutoLoginProfileId) {
        return false;
    }

    m_AutoLoginProfileId.clear();
    saveProfiles();
    emit profilesChanged();
    return true;
}

bool
ProfileManager::setAutoLoginEnabled(bool enabled)
{
    if (enabled && profileIndex(m_DefaultProfileId) < 0) {
        return false;
    }

    m_AutoLoginProfileId = enabled ? m_DefaultProfileId : QString();
    saveProfiles();
    emit profilesChanged();
    return true;
}

bool
ProfileManager::isDefaultProfile(QString id) const
{
    return id == m_DefaultProfileId;
}

bool
ProfileManager::isAutoLoginProfile(QString id) const
{
    return autoLoginEnabled() && id == m_DefaultProfileId;
}

int
ProfileManager::profileIndex(QString id) const
{
    for (int i = 0; i < m_Profiles.count(); i++) {
        if (m_Profiles[i].id == id) {
            return i;
        }
    }
    return -1;
}

QString
ProfileManager::normalizedName(QString name) const
{
    return name.trimmed();
}

bool
ProfileManager::profileNameExists(QString name, QString exceptId) const
{
    for (const Profile& profile : m_Profiles) {
        if (profile.id != exceptId && profile.name.compare(name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}
