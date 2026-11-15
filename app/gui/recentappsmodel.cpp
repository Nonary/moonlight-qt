#include "recentappsmodel.h"

RecentAppsModel::RecentAppsModel(QObject* parent) : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    sort(0);
}

void RecentAppsModel::setAppIds(const QVariantList& ids)
{
    if (ids == m_AppIds) return;
    m_AppIds = ids;
    invalidate();
    emit appIdsChanged();
}

void RecentAppsModel::setSourceModel(QAbstractItemModel* model)
{
    m_AppIdRole = m_HiddenRole = -1;
    if (model) {
        const auto roles = model->roleNames();
        m_AppIdRole = roles.key("appid", -1);
        m_HiddenRole = roles.key("hidden", -1);
    }
    setSortRole(m_AppIdRole);
    setFilterRole(m_HiddenRole);
    QSortFilterProxyModel::setSourceModel(model);
}

bool RecentAppsModel::filterAcceptsRow(int row, const QModelIndex& parent) const
{
    if (!sourceModel() || m_AppIdRole < 0) return false;
    const auto item = sourceModel()->index(row, 0, parent);
    const int id = item.data(m_AppIdRole).toInt();
    return id > 0 && m_AppIds.contains(id) &&
        (m_HiddenRole < 0 || !item.data(m_HiddenRole).toBool());
}

bool RecentAppsModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    return m_AppIds.indexOf(left.data(m_AppIdRole)) < m_AppIds.indexOf(right.data(m_AppIdRole));
}

QVariantMap RecentAppsModel::get(int row) const
{
    QVariantMap result;
    if (row < 0 || row >= rowCount()) return result;
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        result.insert(QString::fromUtf8(it.value()), data(index(row, 0), it.key()));
    return result;
}

int RecentAppsModel::indexOfApp(int appId) const
{
    for (int row = 0; row < rowCount(); ++row)
        if (data(index(row, 0), m_AppIdRole).toInt() == appId) return row;
    return -1;
}
