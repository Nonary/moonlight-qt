#pragma once

#include <QObject>
#include <QVariantList>
#include <QString>
#include <QSettings>
#include <QVector>
#include <atomic>

class ProfileManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QString activeProfileId READ activeProfileIdValue NOTIFY activeProfileChanged)
    Q_PROPERTY(QString activeProfileName READ activeProfileName NOTIFY activeProfileChanged)
    Q_PROPERTY(QString defaultProfileId READ defaultProfileId NOTIFY profilesChanged)
    Q_PROPERTY(QString autoLoginProfileId READ autoLoginProfileId NOTIFY profilesChanged)
    Q_PROPERTY(bool autoLoginEnabled READ autoLoginEnabled NOTIFY profilesChanged)
    Q_PROPERTY(bool hasActiveProfile READ hasActiveProfileValue NOTIFY activeProfileChanged)

public:
    struct Profile
    {
        QString id;
        QString name;
        QString iconId;
        QString createdAt;
        QString updatedAt;
    };

    static ProfileManager*
    get();

    static QString
    activeProfileId();

    static bool
    hasActiveProfile();

    static void
    beginProfileSettings(QSettings& settings);

    static void
    beginProfileSettings(QSettings& settings, QString profileId);

    void suspendRequests();
    void resumeRequests();
    bool requestsAllowed() const { return m_RequestsAllowed.load(); }
    void deactivateProfile();

    QVariantList
    profiles() const;

    QString
    activeProfileIdValue() const;

    bool
    hasActiveProfileValue() const;

    QString
    activeProfileName() const;

    QString
    defaultProfileId() const;

    QString
    autoLoginProfileId() const;

    bool
    autoLoginEnabled() const;

    Q_INVOKABLE bool
    activateProfile(QString id);

    Q_INVOKABLE bool
    switchToProfile(QString id);

    bool
    activateProfileByNameOrId(QString nameOrId, QString* error = nullptr);

    bool
    activateDefaultProfile();

    bool
    activateAutoLoginProfile();

    Q_INVOKABLE QString
    createProfile(QString name);

    Q_INVOKABLE bool
    renameProfile(QString id, QString name);

    Q_INVOKABLE bool
    setProfileIcon(QString id, QString iconId);

    Q_INVOKABLE bool
    removeProfile(QString id);

    Q_INVOKABLE bool
    setDefaultProfile(QString id);

    Q_INVOKABLE bool
    setAutoLoginProfile(QString id, bool enabled);

    Q_INVOKABLE bool
    setAutoLoginEnabled(bool enabled);

    Q_INVOKABLE bool
    isDefaultProfile(QString id) const;

    Q_INVOKABLE bool
    isAutoLoginProfile(QString id) const;

signals:
    void pendingRequestsCanceled();
    void activeProfileAboutToChange();
    void profilesChanged();
    void activeProfileChanged();

private:
    explicit ProfileManager(QObject* parent = nullptr);

    static QString
    profileSettingsGroup(QString profileId);

    void
    loadProfiles();

    void
    saveProfiles();

    void
    ensureProfilesExist();

    void
    migrateLegacyProfileData(QString profileId);

    int
    profileIndex(QString id) const;

    QString
    normalizedName(QString name) const;

    bool
    profileNameExists(QString name, QString exceptId = QString()) const;

    static ProfileManager* s_Pm;
    static QString s_ActiveProfileId;

    QVector<Profile> m_Profiles;
    QString m_DefaultProfileId;
    QString m_AutoLoginProfileId;
    std::atomic_bool m_RequestsAllowed{true};
};
