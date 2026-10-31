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
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

class QLocalServer;
class QLocalSocket;
class Task;

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
    void broadcastNotification(const QString& method, const QJsonObject& params);

    // Returns the result value on success; on failure returns a null QJsonValue
    // and fills errorCode/errorMessage.
    QJsonValue dispatch(const QString& method, const QJsonObject& params, Client* client, QString& errorCode, QString& errorMessage);

    QString trackTask(Task* task, const QString& label);
    QJsonObject taskJson(const TrackedTask& tracked) const;
    void notifyTask(const TrackedTask& tracked);

    QLocalServer* m_server = nullptr;
    QVector<Client*> m_clients;
    QHash<QString, TrackedTask> m_tasks;
    QStringList m_taskOrder;
    int m_taskCounter = 0;
};
