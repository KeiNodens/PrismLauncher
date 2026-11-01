// SPDX-License-Identifier: GPL-3.0-only

#include "CrossSourceRefs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>

#include "Application.h"

namespace {

QString refsPath()
{
    return APPLICATION->dataRoot() + QStringLiteral("/prismlauncher-mcp/crossrefs.json");
}

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
    const QFileInfo info(refsPath());    if (!info.exists()) {
        if (!pairsCache().isEmpty())
            pairsCache() = {};
        cacheStamp() = {};
        return;
    }
    if (cacheStamp().isValid() && info.lastModified() <= cacheStamp())
        return;
    QFile file(refsPath());
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

QJsonObject CrossSourceRefs::allPairs()
{
    static QMutex mutex;
    const QMutexLocker lock(&mutex);
    reloadIfChanged();
    return pairsCache();
}

void CrossSourceRefs::mergePairs(const QJsonObject& newPairs)
{
    static QMutex mutex;
    const QMutexLocker lock(&mutex);
    reloadIfChanged();
    auto merged = pairsCache();
    for (auto it = newPairs.begin(); it != newPairs.end(); ++it) {
        auto entry = merged.value(it.key()).toObject();
        const auto incoming = it.value().toObject();
        for (auto jt = incoming.begin(); jt != incoming.end(); ++jt)
            entry.insert(jt.key(), jt.value());
        merged.insert(it.key(), entry);
    }
    const QJsonObject root{ { QStringLiteral("version"), 1 },
                            { QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
                            { QStringLiteral("pairs"), merged } };
    const QString path = refsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
        pairsCache() = merged;
        cacheStamp() = QFileInfo(path).lastModified();
    }
}
