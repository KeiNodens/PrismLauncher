// BridgeServer: a local, same-user JSON-RPC bridge into the running launcher.
//
// Transport: QLocalServer (named pipe on Windows, unix domain socket elsewhere).
// The socket name is derived from the data directory (the process working
// directory at runtime, the same input LocalPeer uses):
//   "pl" + sha1(utf8(canonical current path)).hex()[0..12) + "-bridge"
// On startup the server also writes a discovery file with the socket name to
// <data dir>/prismlauncher-mcp/bridge.json so clients do not have to re-derive
// it (canonicalization differs between runtimes).
//
// Framing: 4-byte big-endian length prefix + one UTF-8 JSON object per frame.
//   Request:      {"id": <any>, "method": "<name>", "params": {...}}
//   Response:     {"id": <any>, "result": <any>}
//                 {"id": <any>, "error": {"code": "<code>", "message": "<text>"}}
//   Notification: {"method": "task.progress", "params": {...}}
//                 (sent only to clients that called "subscribe")
//
// There is deliberately no authentication: a local socket is reachable only
// from the same user session, exactly like the launcher's own single-instance
// pipe (LocalPeer). The bridge is gated by the "AgentBridgeEnabled" setting.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

#include "tasks/Task.h"

class QFileSystemWatcher;
class QLocalServer;
class QLocalSocket;
class QTimer;

class BridgeServer : public QObject {
    Q_OBJECT
   public:
    explicit BridgeServer(QObject* parent = nullptr);
    ~BridgeServer() override;

    static QString socketNameFor(const QString& dataDir);

    bool start();
    bool isListening() const;

   private:
    struct Client {
        QLocalSocket* socket = nullptr;
        QByteArray buffer;
        bool subscribed = false;
        // Set to false when the client disconnects; async callbacks capture
        // this flag (plus the socket) instead of the Client itself.
        std::shared_ptr<bool> alive = std::make_shared<bool>(true);
        // log.subscribe state (one log per client in v1)
        QString logInstanceId;
        QString logPath;
        qint64 logOffset = 0;
        QByteArray logPartial;
    };

    struct TrackedTask {
        QString id;
        QString label;
        QPointer<Task> task;
        qint64 current = 0;
        qint64 total = 0;
        QString state = QStringLiteral("running");
        QString statusText;
        QString failReason;
    };

    void onNewConnection();
    void onReadyRead(Client* client);
    void handleFrame(Client* client, const QJsonObject& request);
    void sendJson(Client* client, const QJsonObject& payload);
    static void writeFrame(QLocalSocket* socket, const QJsonObject& payload);
    void sendResponse(QLocalSocket* socket, const QJsonValue& id, const QJsonValue& result, const QString& errorCode,
                      const QString& errorMessage);
    void handleModSearch(Client* client, const QJsonValue& id, const QJsonObject& params);
    void handleModInstall(Client* client, const QJsonValue& id, const QJsonObject& params);
    void broadcastNotification(const QString& method, const QJsonObject& params);

    // Returns the result value on success; on failure returns a null QJsonValue
    // and fills errorCode/errorMessage.
    QJsonValue dispatch(const QString& method, const QJsonObject& params, Client* client, QString& errorCode, QString& errorMessage);

    QString trackTask(Task* task, const QString& label);
    QJsonObject taskJson(const TrackedTask& tracked) const;
    void notifyTask(const TrackedTask& tracked);

    void pumpLog(Client* client);
    void onLogDirectoryChanged(const QString& path);

    QLocalServer* m_server = nullptr;
    QFileSystemWatcher* m_logWatcher = nullptr;
    QTimer* m_logTimer = nullptr;
    QList<Task::Ptr> m_pendingJobs;
    QVector<Client*> m_clients;
    QHash<QString, TrackedTask> m_tasks;
    QStringList m_taskOrder;
    int m_taskCounter = 0;
};
