#include "bridge/BridgeServer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QtEndian>

#include "Application.h"
#include "BaseInstance.h"
#include "FileSystem.h"
#include "InstanceList.h"
#include "meta/Index.h"
#include "minecraft/launch/MinecraftTarget.h"
#include "minecraft/VanillaInstanceCreationTask.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "modplatform/ModIndex.h"
#include "modplatform/ResourceAPI.h"
#include "modplatform/modrinth/ModrinthAPI.h"
#include "ResourceDownloadTask.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "net/Mode.h"
#include "minecraft/mod/ModFolderModel.h"
#include "settings/SettingsObject.h"
#include "tasks/Task.h"

namespace {

QJsonObject instanceJson(BaseInstance* inst)
{
    QJsonObject obj;
    obj["id"] = inst->id();
    obj["name"] = inst->name();
    obj["icon"] = inst->iconKey();
    obj["running"] = inst->isRunning();
    obj["instanceRoot"] = inst->instanceRoot();
    obj["gameRoot"] = inst->gameRoot();
    return obj;
}

QVariant jsonToVariant(const QJsonValue& value)
{
    if (value.isBool())
        return value.toBool();
    if (value.isDouble()) {
        double d = value.toDouble();
        if (d == static_cast<double>(static_cast<qint64>(d)))
            return static_cast<qint64>(d);
        return d;
    }
    if (value.isString())
        return value.toString();
    return QVariant();
}

QString accountTypeName(AccountType type)
{
    switch (type) {
        case AccountType::MSA:
            return QStringLiteral("msa");
        case AccountType::Offline:
            return QStringLiteral("offline");
        default:
            return QStringLiteral("other");
    }
}

}  // namespace

BridgeServer::BridgeServer(QObject* parent) : QObject(parent) {}

BridgeServer::~BridgeServer()
{
    for (auto* client : m_clients) {
        if (client->socket)
            client->socket->disconnectFromServer();
        delete client;
    }
    if (m_server)
        m_server->close();
}

QString BridgeServer::socketNameFor(const QString& dataDir)
{
    QString canonical = QDir(dataDir).canonicalPath();
    if (canonical.isEmpty())
        canonical = QDir(dataDir).absolutePath();
    QByteArray hash = QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("pl") + QString::fromLatin1(hash.left(12)) + QStringLiteral("-bridge");
}

bool BridgeServer::isListening() const
{
    return m_server && m_server->isListening();
}

bool BridgeServer::start()
{
    if (isListening())
        return true;

    const QString dataDir = QDir::currentPath();
    const QString name = socketNameFor(dataDir);

    m_server = new QLocalServer(this);
    // Remove a stale socket left by a crashed previous process (no-op on Windows).
    QLocalServer::removeServer(name);
    if (!m_server->listen(name)) {
        qWarning() << "Agent bridge: failed to listen on" << name << ":" << m_server->errorString();
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &BridgeServer::onNewConnection);
    qInfo() << "Agent bridge listening on" << name << "for data dir" << dataDir;

    m_logWatcher = new QFileSystemWatcher(this);
    connect(m_logWatcher, &QFileSystemWatcher::directoryChanged, this, &BridgeServer::onLogDirectoryChanged);
    // Backstop: directory watching alone misses logs/ dirs that do not exist
    // yet at subscribe time (the game creates them at launch). Poll cheaply.
    m_logTimer = new QTimer(this);
    m_logTimer->setInterval(2000);
    connect(m_logTimer, &QTimer::timeout, this, [this]() { onLogDirectoryChanged(QString()); });
    m_logTimer->start();

    // Discovery file for clients (the MCP server / prism-ctl).
    const QString dirPath = FS::PathCombine(dataDir, QStringLiteral("prismlauncher-mcp"));
    QDir().mkpath(dirPath);
    QJsonObject info;
    info["socketName"] = name;
    info["pid"] = QCoreApplication::applicationPid();
    info["startedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    info["protocol"] = 1;
    QFile file(FS::PathCombine(dirPath, QStringLiteral("bridge.json")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(info).toJson(QJsonDocument::Indented));
    }
    return true;
}

void BridgeServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        auto* socket = m_server->nextPendingConnection();
        auto* client = new Client;
        client->socket = socket;
        m_clients.append(client);
        connect(socket, &QLocalSocket::readyRead, this, [this, client]() { onReadyRead(client); });
        connect(socket, &QLocalSocket::disconnected, this, [this, client]() {
            *client->alive = false;
            m_clients.removeAll(client);
            client->socket->deleteLater();
            delete client;
        });
    }
}

void BridgeServer::onReadyRead(Client* client)
{
    client->buffer += client->socket->readAll();
    while (client->buffer.size() >= 4) {
        quint32 length = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(client->buffer.constData()));
        if (length > 16 * 1024 * 1024) {  // sanity cap
            client->socket->disconnectFromServer();
            return;
        }
        if (client->buffer.size() < static_cast<int>(4 + length))
            return;
        QByteArray payload = client->buffer.mid(4, static_cast<int>(length));
        client->buffer.remove(0, 4 + static_cast<int>(length));

        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            QJsonObject error;
            error["id"] = QJsonValue::Null;
            QJsonObject detail;
            detail["code"] = QStringLiteral("parse_error");
            detail["message"] = parseError.errorString();
            error["error"] = detail;
            sendJson(client, error);
            continue;
        }
        handleFrame(client, doc.object());
    }
}

void BridgeServer::writeFrame(QLocalSocket* socket, const QJsonObject& payload)
{
    if (!socket)
        return;
    QByteArray data = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.resize(4);
    qToBigEndian<quint32>(static_cast<quint32>(data.size()), reinterpret_cast<uchar*>(frame.data()));
    frame += data;
    socket->write(frame);
    socket->flush();
}

void BridgeServer::sendJson(Client* client, const QJsonObject& payload)
{
    writeFrame(client->socket, payload);
}

void BridgeServer::sendResponse(QLocalSocket* socket, const QJsonValue& id, const QJsonValue& result, const QString& errorCode,
                                const QString& errorMessage)
{
    QJsonObject response;
    response["id"] = id;
    if (errorCode.isEmpty()) {
        response["result"] = result;
    } else {
        QJsonObject detail;
        detail["code"] = errorCode;
        detail["message"] = errorMessage;
        response["error"] = detail;
    }
    writeFrame(socket, response);
}

void BridgeServer::broadcastNotification(const QString& method, const QJsonObject& params)
{
    QJsonObject note;
    note["method"] = method;
    note["params"] = params;
    for (auto* client : m_clients) {
        if (client->subscribed)
            sendJson(client, note);
    }
}

void BridgeServer::handleFrame(Client* client, const QJsonObject& request)
{
    const QString method = request.value("method").toString();
    const QJsonValue id = request.value("id");
    QJsonObject params = request.value("params").toObject();

    // Async methods respond from callbacks, not from dispatch().
    if (method == QStringLiteral("mod.search")) {
        handleModSearch(client, id, params);
        return;
    }
    if (method == QStringLiteral("mod.install")) {
        handleModInstall(client, id, params);
        return;
    }

    QString errorCode;
    QString errorMessage;
    QJsonValue result = dispatch(method, params, client, errorCode, errorMessage);
    sendResponse(client->socket, id, result, errorCode, errorMessage);
}

void BridgeServer::handleModSearch(Client* client, const QJsonValue& id, const QJsonObject& params)
{
    const QString query = params.value("query").toString();
    auto socket = client->socket;
    auto alive = client->alive;

    auto api = std::make_shared<ModrinthAPI>();
    ResourceAPI::Callback<QList<ModPlatform::IndexedPack::Ptr>> callbacks{};
    callbacks.on_succeed = [this, socket, alive, id, api](QList<ModPlatform::IndexedPack::Ptr>& packs) {
        if (!*alive)
            return;
        QJsonArray hits;
        for (const auto& pack : packs) {
            QJsonObject hit;
            hit["id"] = pack->addonId.toString();
            hit["slug"] = pack->slug;
            hit["name"] = pack->name;
            hit["description"] = pack->description;
            QJsonArray authors;
            for (const auto& author : pack->authors)
                authors.append(author.name);
            hit["authors"] = authors;
            hit["websiteUrl"] = pack->websiteUrl;
            hits.append(hit);
        }
        QJsonObject result;
        result["hits"] = hits;
        sendResponse(socket, id, result, QString(), QString());
    };
    callbacks.on_fail = [this, socket, alive, id, api](QString reason, int) {
        if (*alive)
            sendResponse(socket, id, QJsonValue(), QStringLiteral("upstream"), QStringLiteral("Modrinth search failed: %1").arg(reason));
    };
    callbacks.on_abort = [this, socket, alive, id, api]() {
        if (*alive)
            sendResponse(socket, id, QJsonValue(), QStringLiteral("upstream"), QStringLiteral("Modrinth search aborted"));
    };

    ResourceAPI::SortingMethod sort{};
    sort.name = QStringLiteral("relevance");
    auto job = api->searchProjects({ ModPlatform::ResourceType::Mod, 0, query, sort, {}, {}, ModPlatform::Side::NoSide, {}, {} },
                                   std::move(callbacks));
    if (!job) {
        sendResponse(socket, id, QJsonValue(), QStringLiteral("upstream"), QStringLiteral("could not create search job"));
        return;
    }
    Task::Ptr jobPtr = job;
    m_pendingJobs.append(jobPtr);
    connect(job.get(), &Task::finished, this, [this, jobPtr]() { m_pendingJobs.removeAll(jobPtr); });
    job->start();
}

void BridgeServer::handleModInstall(Client* client, const QJsonValue& id, const QJsonObject& params)
{
    auto socket = client->socket;
    auto alive = client->alive;
    auto respondError = [this, socket, alive, id](const QString& code, const QString& message) {
        if (*alive)
            sendResponse(socket, id, QJsonValue(), code, message);
    };

    const QString instanceId = params.value("instanceId").toString();
    const QString projectId = params.value("projectId").toString();
    const QString wantedVersionId = params.value("versionId").toString();
    auto* baseInst = APPLICATION->instances()->getInstanceById(instanceId);
    auto* inst = dynamic_cast<MinecraftInstance*>(baseInst);
    if (!inst) {
        respondError(QStringLiteral("not_found"), QStringLiteral("no Minecraft instance with id %1").arg(instanceId));
        return;
    }
    if (projectId.isEmpty()) {
        respondError(QStringLiteral("invalid_params"), QStringLiteral("mod.install needs projectId"));
        return;
    }

    // Resolve the instance's Minecraft version and loader from its PackProfile.
    // The profile's component list is loaded lazily; force the on-disk load so
    // the version filters below are not silently empty.
    auto* profile = inst->getPackProfile();
    if (profile && profile->getComponentVersion(QStringLiteral("net.minecraft")).isEmpty())
        profile->reload(Net::Mode::Offline);
    const QString mcVersion = profile ? profile->getComponentVersion(QStringLiteral("net.minecraft")) : QString();
    ModPlatform::ModLoaderTypes loaderTypes;
    QString loaderUid;
    if (profile) {
        const QList<QPair<QString, ModPlatform::ModLoaderType>> candidates = {
            { QStringLiteral("net.minecraftforge"), ModPlatform::ModLoaderType::Forge },
            { QStringLiteral("net.neoforged"), ModPlatform::ModLoaderType::NeoForge },
            { QStringLiteral("net.fabricmc.fabric-loader"), ModPlatform::ModLoaderType::Fabric },
            { QStringLiteral("org.quiltmc.quilt-loader"), ModPlatform::ModLoaderType::Quilt },
        };
        for (const auto& candidate : candidates) {
            if (!profile->getComponentVersion(candidate.first).isEmpty()) {
                loaderUid = candidate.first;
                loaderTypes = ModPlatform::ModLoaderTypes(candidate.second);
                break;
            }
        }
    }

    auto pack = std::make_shared<ModPlatform::IndexedPack>();
    pack->addonId = projectId;
    pack->provider = ModPlatform::ResourceProvider::MODRINTH;

    auto api = std::make_shared<ModrinthAPI>();
    // The resource metadata writer needs the pack's name/slug, which only the
    // project-info response carries; fetch it first, then resolve versions
    // for the enriched pack.
    ResourceAPI::Callback<ModPlatform::IndexedPack::Ptr> infoCallbacks{};
    infoCallbacks.on_succeed = [this, socket, alive, id, api, inst, wantedVersionId, mcVersion, loaderTypes,
                                respondError](ModPlatform::IndexedPack::Ptr& enriched) {
        if (!*alive)
            return;
        ResourceAPI::Callback<QVector<ModPlatform::IndexedVersion>> callbacks{};
        callbacks.on_succeed = [this, socket, alive, id, api, enriched, inst, wantedVersionId](QVector<ModPlatform::IndexedVersion>& versions) {
            if (!*alive)
                return;
            const ModPlatform::IndexedVersion* chosen = nullptr;
            for (const auto& version : versions) {
                if (!wantedVersionId.isEmpty()) {
                    if (version.fileId.toString() == wantedVersionId || version.addonId.toString() == wantedVersionId)
                        chosen = &version;
                    continue;
                }
                if (!chosen || (chosen->version_type != ModPlatform::IndexedVersionType::Release &&
                                version.version_type == ModPlatform::IndexedVersionType::Release))
                    chosen = &version;
            }
            if (!chosen) {
                sendResponse(socket, id, QJsonValue(), QStringLiteral("not_found"),
                             QStringLiteral("no matching version for this instance (mc/loader filter applied)"));
                return;
            }
            ModPlatform::IndexedVersion version = *chosen;

            auto download = makeShared<ResourceDownloadTask>(enriched, version, inst->loaderModList());
            const QString taskId = trackTask(download.get(), QStringLiteral("install mod %1").arg(enriched->name));
            Task::Ptr holder = download;
            m_pendingJobs.append(holder);
            connect(download.get(), &Task::finished, this, [this, socket, alive, id, holder, version, taskId]() {
                m_pendingJobs.removeAll(holder);
                if (!*alive)
                    return;
                auto it = m_tasks.find(taskId);
                if (it != m_tasks.end() && it->state == QStringLiteral("succeeded")) {
                    QJsonObject result;
                    result["installed"] = true;
                    result["fileName"] = version.fileName;
                    result["version"] = version.version_number;
                    result["taskId"] = taskId;
                    QJsonArray deps;
                    for (const auto& dep : version.dependencies) {
                        QJsonObject depObj;
                        depObj["projectId"] = dep.addonId.toString();
                        depObj["type"] = ModPlatform::DependencyTypeUtils::toString(dep.type);
                        if (!dep.version.isEmpty())
                            depObj["version"] = dep.version;
                        deps.append(depObj);
                    }
                    result["dependencies"] = deps;
                    sendResponse(socket, id, result, QString(), QString());
                } else {
                    QString reason = (it != m_tasks.end() ? it->failReason : QStringLiteral("download failed"));
                    sendResponse(socket, id, QJsonValue(), QStringLiteral("task_failed"), reason);
                }
            });
            download->start();
        };
        callbacks.on_fail = [respondError](QString reason, int) {
            respondError(QStringLiteral("upstream"), QStringLiteral("Modrinth versions lookup failed: %1").arg(reason));
        };
        callbacks.on_abort = [respondError]() { respondError(QStringLiteral("upstream"), QStringLiteral("Modrinth versions lookup aborted")); };

        ResourceAPI::VersionSearchArgs args{};
        args.pack = enriched;
        if (!mcVersion.isEmpty())
            args.mcVersions = std::vector<Version>{ Version(mcVersion) };
        if (loaderTypes != ModPlatform::ModLoaderTypes{})
            args.loaders = loaderTypes;
        args.resourceType = ModPlatform::ResourceType::Mod;

        auto job = api->getProjectVersions(std::move(args), std::move(callbacks));
        if (!job) {
            respondError(QStringLiteral("upstream"), QStringLiteral("could not create versions job"));
            return;
        }
        Task::Ptr jobPtr = job;
        m_pendingJobs.append(jobPtr);
        connect(job.get(), &Task::finished, this, [this, jobPtr]() { m_pendingJobs.removeAll(jobPtr); });
        job->start();
    };
    infoCallbacks.on_fail = [respondError](QString reason, int) {
        respondError(QStringLiteral("upstream"), QStringLiteral("Modrinth project lookup failed: %1").arg(reason));
    };
    infoCallbacks.on_abort = [respondError]() { respondError(QStringLiteral("upstream"), QStringLiteral("Modrinth project lookup aborted")); };

    auto infoJob = api->getProjectInfo({ pack }, std::move(infoCallbacks), false);
    if (!infoJob) {
        respondError(QStringLiteral("upstream"), QStringLiteral("could not create project info job"));
        return;
    }
    Task::Ptr infoJobPtr = infoJob;
    m_pendingJobs.append(infoJobPtr);
    connect(infoJob.get(), &Task::finished, this, [this, infoJobPtr]() { m_pendingJobs.removeAll(infoJobPtr); });
    infoJob->start();
}

QString BridgeServer::trackTask(Task* task, const QString& label)
{
    const QString id = QStringLiteral("task-%1").arg(++m_taskCounter);
    TrackedTask tracked;
    tracked.id = id;
    tracked.label = label;
    tracked.task = task;
    m_tasks.insert(id, tracked);
    m_taskOrder.append(id);

    connect(task, &Task::progress, this, [this, id](qint64 current, qint64 total) {
        auto it = m_tasks.find(id);
        if (it == m_tasks.end())
            return;
        it->current = current;
        it->total = total;
        notifyTask(*it);
    });
    connect(task, &Task::status, this, [this, id](const QString& status) {
        auto it = m_tasks.find(id);
        if (it == m_tasks.end())
            return;
        it->statusText = status;
        notifyTask(*it);
    });
    connect(task, &Task::finished, this, [this, id]() {
        auto it = m_tasks.find(id);
        if (it == m_tasks.end())
            return;
        Task* t = it->task;
        if (t) {
            if (t->getState() == Task::State::Succeeded)
                it->state = QStringLiteral("succeeded");
            else if (t->getState() == Task::State::AbortedByUser)
                it->state = QStringLiteral("aborted");
            else {
                it->state = QStringLiteral("failed");
                it->failReason = t->failReason();
            }
        }
        notifyTask(*it);
    });

    // Keep at most the 20 most recent finished tasks.
    while (m_taskOrder.size() > 20) {
        const QString oldest = m_taskOrder.first();
        auto it = m_tasks.find(oldest);
        if (it != m_tasks.end() && it->state == QStringLiteral("running"))
            break;
        m_tasks.remove(oldest);
        m_taskOrder.removeFirst();
    }
    return id;
}

QJsonObject BridgeServer::taskJson(const TrackedTask& tracked) const
{
    QJsonObject obj;
    obj["id"] = tracked.id;
    obj["label"] = tracked.label;
    obj["state"] = tracked.state;
    obj["current"] = tracked.current;
    obj["total"] = tracked.total;
    obj["status"] = tracked.statusText;
    if (!tracked.failReason.isEmpty())
        obj["failReason"] = tracked.failReason;
    return obj;
}

void BridgeServer::notifyTask(const TrackedTask& tracked)
{
    broadcastNotification(QStringLiteral("task.progress"), taskJson(tracked));
}

void BridgeServer::pumpLog(Client* client)
{
    if (client->logPath.isEmpty())
        return;
    QFile file(client->logPath);
    if (!file.exists()) {
        client->logOffset = 0;
        client->logPartial.clear();
        return;
    }
    if (file.size() < client->logOffset) {  // rotated or truncated
        client->logOffset = 0;
        client->logPartial.clear();
    }
    if (!file.open(QIODevice::ReadOnly))
        return;
    file.seek(client->logOffset);
    QByteArray fresh = file.readAll();
    client->logOffset = file.pos();
    if (fresh.isEmpty())
        return;

    client->logPartial += fresh;
    QJsonArray lines;
    int newline;
    while ((newline = client->logPartial.indexOf('\n')) >= 0) {
        QByteArray line = client->logPartial.left(newline);
        if (line.endsWith('\r'))
            line.chop(1);
        lines.append(QString::fromUtf8(line));
        client->logPartial.remove(0, newline + 1);
    }
    if (lines.isEmpty())
        return;
    QJsonObject params;
    params["id"] = client->logInstanceId;
    params["lines"] = lines;
    QJsonObject note;
    note["method"] = QStringLiteral("log.line");
    note["params"] = params;
    sendJson(client, note);
}

void BridgeServer::onLogDirectoryChanged(const QString& path)
{
    Q_UNUSED(path);
    for (auto* client : m_clients) {
        if (client->logInstanceId.isEmpty())
            continue;
        // Start watching the logs dir as soon as it exists.
        const QString logsDir = QFileInfo(client->logPath).absolutePath();
        if (m_logWatcher && QDir(logsDir).exists() && !m_logWatcher->directories().contains(logsDir))
            m_logWatcher->addPath(logsDir);
        pumpLog(client);
    }
}

QJsonValue BridgeServer::dispatch(const QString& method, const QJsonObject& params, Client* client, QString& errorCode,
                                  QString& errorMessage)
{
    auto fail = [&errorCode, &errorMessage](const QString& code, const QString& message) {
        errorCode = code;
        errorMessage = message;
        return QJsonValue();
    };
    Application* app = APPLICATION;

    if (method == QStringLiteral("ping")) {
        QJsonObject obj;
        obj["bridge"] = QStringLiteral("prismlauncher-agent-bridge");
        obj["protocol"] = 1;
        obj["dataDir"] = QDir::currentPath();
        return obj;
    }

    if (method == QStringLiteral("subscribe")) {
        client->subscribed = true;
        QJsonObject obj;
        obj["subscribed"] = true;
        return obj;
    }

    if (method == QStringLiteral("instance.list")) {
        QJsonArray array;
        auto* list = app->instances();
        for (int i = 0; i < list->count(); i++)
            array.append(instanceJson(list->at(i)));
        return array;
    }

    if (method == QStringLiteral("instance.get")) {
        const QString id = params.value("id").toString();
        auto* inst = app->instances()->getInstanceById(id);
        if (!inst)
            return fail(QStringLiteral("not_found"), QStringLiteral("no instance with id %1").arg(id));
        return instanceJson(inst);
    }

    if (method == QStringLiteral("instance.create")) {
        const QString name = params.value("name").toString();
        const QString versionId = params.value("version").toString();
        if (name.isEmpty() || versionId.isEmpty())
            return fail(QStringLiteral("invalid_params"), QStringLiteral("instance.create needs name and version"));

        auto* index = app->metadataIndex();
        auto version = index ? index->get(QStringLiteral("net.minecraft"), versionId) : nullptr;
        if (!version)
            return fail(QStringLiteral("unavailable"),
                        QStringLiteral("Minecraft version %1 not found (meta index may still be loading)").arg(versionId));

        VanillaCreationTask* creation = nullptr;
        const QString loader = params.value("loader").toString();
        if (!loader.isEmpty()) {
            QString loaderUid;
            if (loader == QStringLiteral("forge"))
                loaderUid = QStringLiteral("net.minecraftforge");
            else if (loader == QStringLiteral("neoforge"))
                loaderUid = QStringLiteral("net.neoforged");
            else if (loader == QStringLiteral("fabric"))
                loaderUid = QStringLiteral("net.fabricmc.fabric-loader");
            else if (loader == QStringLiteral("quilt"))
                loaderUid = QStringLiteral("org.quiltmc.quilt-loader");
            else
                return fail(QStringLiteral("invalid_params"), QStringLiteral("unknown loader %1").arg(loader));

            const QString loaderVersionId = params.value("loaderVersion").toString();
            if (loaderVersionId.isEmpty())
                return fail(QStringLiteral("invalid_params"), QStringLiteral("loaderVersion is required with loader"));
            auto loaderVersion = index->get(loaderUid, loaderVersionId);
            if (!loaderVersion)
                return fail(QStringLiteral("unavailable"),
                            QStringLiteral("loader version %1 %2 not found").arg(loaderUid, loaderVersionId));
            creation = new VanillaCreationTask(version, loaderUid, loaderVersion);
        } else {
            creation = new VanillaCreationTask(version);
        }
        creation->setName(name);
        const QString icon = params.value("icon").toString();
        if (!icon.isEmpty())
            creation->setIcon(icon);
        const QString group = params.value("group").toString();
        if (!group.isEmpty())
            creation->setGroup(group);

        Task* wrapped = app->instances()->wrapInstanceTask(creation);
        wrapped->setParent(this);
        const QString taskId = trackTask(wrapped, QStringLiteral("create instance %1").arg(name));
        wrapped->start();
        QJsonObject obj;
        obj["taskId"] = taskId;
        return obj;
    }

    if (method == QStringLiteral("launch") || method == QStringLiteral("launch.status") || method == QStringLiteral("launch.stop")) {
        const QString id = params.value("id").toString();
        auto* inst = app->instances()->getInstanceById(id);
        if (!inst)
            return fail(QStringLiteral("not_found"), QStringLiteral("no instance with id %1").arg(id));

        if (method == QStringLiteral("launch.status")) {
            QJsonObject obj;
            obj["id"] = id;
            obj["running"] = inst->isRunning();
            return obj;
        }
        if (method == QStringLiteral("launch.stop")) {
            bool ok = app->kill(inst);
            QJsonObject obj;
            obj["id"] = id;
            obj["stopped"] = ok;
            return obj;
        }
        // launch
        MinecraftTarget::Ptr target;
        const QString server = params.value("server").toString();
        if (!server.isEmpty())
            target.reset(new MinecraftTarget(MinecraftTarget::parse(server, false)));
        MinecraftAccountPtr account;
        const QString profile = params.value("profile").toString();
        if (!profile.isEmpty()) {
            account = app->accounts()->getAccountByProfileName(profile);
            if (!account)
                return fail(QStringLiteral("not_found"), QStringLiteral("no account with profile name %1").arg(profile));
        }
        const QString offlineName = params.value("offlineName").toString();
        bool ok;
        if (!offlineName.isEmpty())
            ok = app->launch(inst, LaunchMode::Offline, target, account, offlineName);
        else
            ok = app->launch(inst, LaunchMode::Normal, target, account);
        QJsonObject obj;
        obj["id"] = id;
        obj["launched"] = ok;
        return obj;
    }

    if (method == QStringLiteral("log.tail")) {
        const QString id = params.value("id").toString();
        auto* inst = app->instances()->getInstanceById(id);
        if (!inst)
            return fail(QStringLiteral("not_found"), QStringLiteral("no instance with id %1").arg(id));
        const int lines = params.value("lines").toInt(100);
        const QString path = FS::PathCombine(inst->gameRoot(), QStringLiteral("logs/latest.log"));
        QFile file(path);
        if (!file.exists()) {
            QJsonObject obj;
            obj["id"] = id;
            obj["path"] = path;
            obj["lines"] = QJsonArray();
            return obj;
        }
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return fail(QStringLiteral("internal"), QStringLiteral("cannot open %1").arg(path));
        QStringList all = QString::fromUtf8(file.readAll()).split('\n');
        QJsonArray tail;
        for (int i = qMax(0, static_cast<int>(all.size()) - lines); i < all.size(); i++)
            tail.append(all.at(i));
        QJsonObject obj;
        obj["id"] = id;
        obj["path"] = path;
        obj["lines"] = tail;
        return obj;
    }

    if (method == QStringLiteral("log.subscribe")) {
        const QString id = params.value("id").toString();
        auto* inst = app->instances()->getInstanceById(id);
        if (!inst)
            return fail(QStringLiteral("not_found"), QStringLiteral("no instance with id %1").arg(id));
        const QString logsDir = FS::PathCombine(inst->gameRoot(), QStringLiteral("logs"));
        client->logInstanceId = id;
        client->logPath = FS::PathCombine(logsDir, QStringLiteral("latest.log"));
        client->logPartial.clear();
        QFileInfo info(client->logPath);
        client->logOffset = info.exists() ? info.size() : 0;  // stream only new lines
        if (m_logWatcher && QDir(logsDir).exists() && !m_logWatcher->directories().contains(logsDir))
            m_logWatcher->addPath(logsDir);
        QJsonObject obj;
        obj["id"] = id;
        obj["subscribed"] = true;
        return obj;
    }

    if (method == QStringLiteral("log.unsubscribe")) {
        client->logInstanceId.clear();
        client->logPath.clear();
        client->logOffset = 0;
        client->logPartial.clear();
        QJsonObject obj;
        obj["subscribed"] = false;
        return obj;
    }

    if (method == QStringLiteral("settings.get")) {
        const QString key = params.value("key").toString();
        if (key.isEmpty())
            return fail(QStringLiteral("invalid_params"), QStringLiteral("settings.get needs key"));
        QJsonObject obj;
        obj["key"] = key;
        obj["value"] = QJsonValue::fromVariant(app->settings()->get(key));
        return obj;
    }

    if (method == QStringLiteral("settings.set")) {
        const QString key = params.value("key").toString();
        if (key.isEmpty() || !params.contains("value"))
            return fail(QStringLiteral("invalid_params"), QStringLiteral("settings.set needs key and value"));
        app->settings()->set(key, jsonToVariant(params.value("value")));
        QJsonObject obj;
        obj["key"] = key;
        obj["value"] = QJsonValue::fromVariant(app->settings()->get(key));
        return obj;
    }

    if (method == QStringLiteral("account.list")) {
        QJsonArray array;
        auto* accounts = app->accounts();
        for (int i = 0; i < accounts->count(); i++) {
            auto account = accounts->at(i);
            QJsonObject obj;
            obj["profileName"] = account->profileName();
            obj["type"] = accountTypeName(account->accountType());
            obj["active"] = account->isActive();
            array.append(obj);
        }
        return array;
    }

    if (method == QStringLiteral("task.list")) {
        QJsonArray array;
        for (const QString& id : m_taskOrder) {
            auto it = m_tasks.find(id);
            if (it != m_tasks.end())
                array.append(taskJson(*it));
        }
        return array;
    }

    return fail(QStringLiteral("unknown_method"), QStringLiteral("unknown method %1").arg(method));
}
