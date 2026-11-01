// SPDX-License-Identifier: GPL-3.0-only

#include "CrossSourceResolveTask.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include "Application.h"
#include "BaseInstance.h"
#include "BuildConfig.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/mod/MetadataHandler.h"
#include "minecraft/mod/ModFolderModel.h"
#include "modplatform/CrossSourceRefs.h"
#include "modplatform/ModIndex.h"
#include "net/ApiDownload.h"

namespace {
// Per-run cap so a first open on a huge instance stays a background chore,
// not a flood; later opens pick up where this left off (the table keeps
// what was found).
constexpr int MAX_LOOKUPS = 60;
}  // namespace

CrossSourceResolveTask::CrossSourceResolveTask(BaseInstance& instance, QObject* parent) : Task(parent)
{
    auto& mc = static_cast<MinecraftInstance&>(instance);
    m_indexDirPath = mc.loaderModList()->indexDir().absolutePath();
}

void CrossSourceResolveTask::executeTask()
{
    const auto known = CrossSourceRefs::allPairs();
    QDir indexDir(m_indexDirPath);
    if (!indexDir.exists()) {
        emitSucceeded();
        return;
    }
    for (const auto& entry : indexDir.entryList({ QStringLiteral("*.pw.toml") }, QDir::Files)) {
        const QString slug = entry.chopped(8);  // strip ".pw.toml"
        const auto meta = Metadata::get(indexDir, slug);
        if (!meta.isValid())
            continue;
        const bool installedViaModrinth = meta.provider == ModPlatform::ResourceProvider::MODRINTH;
        const auto recorded = known.value(slug.toLower()).toObject();
        const bool otherKnown = installedViaModrinth ? recorded.contains(QStringLiteral("curseforge"))
                                                     : recorded.contains(QStringLiteral("modrinth"));
        if (otherKnown)
            continue;
        m_pending.append({ slug, !installedViaModrinth, meta.project_id.toString() });
        if (m_pending.size() >= MAX_LOOKUPS)
            break;
    }
    if (m_pending.isEmpty()) {
        emitSucceeded();
        return;
    }
    startNext();
}

void CrossSourceResolveTask::startNext()
{
    if (m_pending.isEmpty()) {
        finishAll();
        return;
    }
    const auto lookup = m_pending.takeFirst();
    m_attempted++;
    QString url;
    if (lookup.wantModrinth) {
        url = QStringLiteral("https://api.modrinth.com/v2/project/") + lookup.slug;
    } else {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("gameId"), QStringLiteral("432"));
        query.addQueryItem(QStringLiteral("slug"), lookup.slug);
        url = QString(BuildConfig.FLAME_BASE_URL) + QStringLiteral("/mods/search?") + query.toString(QUrl::FullyEncoded);
    }
    m_currentJob = makeShared<NetJob>(QStringLiteral("CrossSourceResolve"), APPLICATION->network());
    auto [action, sink] = Net::ApiDownload::makeByteArray(QUrl(url));
    m_sink = sink;
    m_currentJob->addNetAction(action);
    connect(m_currentJob.get(), &NetJob::succeeded, this, [this, lookup] {
        const auto doc = QJsonDocument::fromJson(m_sink ? *m_sink : QByteArray());
        if (lookup.wantModrinth) {
            const auto id = doc.object().value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                QJsonObject pair;
                pair.insert(QStringLiteral("modrinth"), id);
                pair.insert(QStringLiteral("curseforge"), QJsonValue::fromVariant(QVariant(lookup.ownId.toLongLong())));
                m_found.insert(lookup.slug.toLower(), pair);
            }
        } else {
            for (const auto& item : doc.object().value(QStringLiteral("data")).toArray()) {
                const auto obj = item.toObject();
                if (obj.value(QStringLiteral("slug")).toString() == lookup.slug) {
                    QJsonObject pair;
                    pair.insert(QStringLiteral("modrinth"), lookup.ownId);
                    pair.insert(QStringLiteral("curseforge"), QJsonValue::fromVariant(obj.value(QStringLiteral("id")).toVariant()));
                    m_found.insert(lookup.slug.toLower(), pair);
                    break;
                }
            }
        }
        startNext();
    });
    connect(m_currentJob.get(), &NetJob::failed, this, [this](const QString&) { startNext(); });
    m_currentJob->start();
}

void CrossSourceResolveTask::finishAll()
{
    if (!m_found.isEmpty())
        CrossSourceRefs::mergePairs(m_found);
    emitSucceeded();
}
