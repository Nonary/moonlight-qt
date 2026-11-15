#pragma once

#include <QSortFilterProxyModel>
#include <QVariantList>

// A live view of the host catalogue in this client's last-played order.
// Metadata always comes from the source model, including asynchronous artwork.
class RecentAppsModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QVariantList appIds READ appIds WRITE setAppIds NOTIFY appIdsChanged)
public:
    explicit RecentAppsModel(QObject* parent = nullptr);
    QVariantList appIds() const { return m_AppIds; }
    void setAppIds(const QVariantList& ids);
    void setSourceModel(QAbstractItemModel* model) override;
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int indexOfApp(int appId) const;
signals:
    void appIdsChanged();
protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
private:
    QVariantList m_AppIds;
    int m_AppIdRole = -1;
    int m_HiddenRole = -1;
};
