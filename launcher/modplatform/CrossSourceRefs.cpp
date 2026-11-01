// SPDX-License-Identifier: GPL-3.0-only

#include "CrossSourceRefs.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>

#include "Application.h"

namespace {

QJsonObject& pairsCache()
{
    static QJsonObject cache;
    return cache;
}

QDateTime& cacheStamp()
{
    static QDateTime stamp;
    return stamp;
}

void reloadIfChanged()
{
    const QString path = APPLICATION->dataRoot() + QStringLiteral("/prismlauncher-mcp/crossrefs.json");
    const QFileInfo info(path);
    if (!info.exists()) {
        if (!pairsCache().isEmpty())
            pairsCache() = {};
        cacheStamp() = {};
        return;
    }
    if (cacheStamp().isValid() && info.lastModified() <= cacheStamp())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isObject())
        pairsCache() = doc.object().value(QStringLiteral("pairs")).toObject();
    cacheStamp() = info.lastModified();
}

}  // namespace

QVariant CrossSourceRefs::projectId(const QString& slug, bool modrinth)
{
    static QMutex mutex;
    const QMutexLocker lock(&mutex);
    reloadIfChanged();
    const auto entry = pairsCache().value(slug.toLower()).toObject();
    if (entry.isEmpty())
        return {};
    if (modrinth) {
        const auto value = entry.value(QStringLiteral("modrinth"));
        return value.isString() ? QVariant(value.toString()) : QVariant();
    }
    const auto value = entry.value(QStringLiteral("curseforge"));
    return value.isDouble() ? QVariant::fromValue<qlonglong>(static_cast<qlonglong>(value.toDouble())) : QVariant();
}
