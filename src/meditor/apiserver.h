/*
    SPDX-FileCopyrightText: 2026 MEditor Contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "undohelper.hpp"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>

class EventBridge;

namespace MEditor {
class Handlers;
}
using MEditorHandlers = MEditor::Handlers;

/**
 * @class MEditorApiServer
 * @brief Local JSON-RPC 2.0 server over QLocalServer (NDJSON framing) for live MEditor automation.
 *
 * Security model:
 *  - Disabled unless started with `--api` (or `KdenliveSettings::meditor_api_enabled()` / `MEDITOR_API=1`).
 *  - Socket created with `QLocalServer::UserAccessOption` (restricted to current OS user).
 *  - Generates a 256-bit random token and writes `<AppLocalData>/meditor/api.json` with mode 0600
 *    (`QFileDevice::ReadOwner | QFileDevice::WriteOwner`).
 *  - Connections must authenticate via `hello {token}` before invoking any method other than `hello` or `ping`.
 */
class MEditorApiServer : public QObject
{
    Q_OBJECT

public:
    /**
     * @class ApiCallScope
     * @brief RAII guard that increments `m_apiDepth` while an API request is executing on the GUI thread.
     *
     * Any `QUndoCommand` pushed to `DocUndoStack` while `m_apiDepth > 0` is tagged by `EventBridge`
     * with `origin: "claude"`; commands pushed with `m_apiDepth == 0` are tagged with `origin: "user"`.
     */
    class ApiCallScope
    {
    public:
        explicit ApiCallScope(MEditorApiServer *server)
            : m_server(server)
        {
            if (m_server) {
                m_server->enterApiCall();
            }
        }
        ~ApiCallScope()
        {
            if (m_server) {
                m_server->leaveApiCall();
            }
        }
        ApiCallScope(const ApiCallScope &) = delete;
        ApiCallScope &operator=(const ApiCallScope &) = delete;

    private:
        MEditorApiServer *m_server{nullptr};
    };

    using ApiDepthGuard = ApiCallScope;
    using ApiOriginGuard = ApiCallScope;

    explicit MEditorApiServer(QObject *parent = nullptr, bool forceEnable = false);
    ~MEditorApiServer() override;

    /** @brief Extract complete newline-delimited JSON messages from @p buffer, leaving partial trailing bytes. */
    static QList<QByteArray> takeMessages(QByteArray &buffer);

    /** @brief Returns true if the local socket server is actively listening. */
    bool isListening() const;

    /** @brief Short server name passed to QLocalServer::listen (e.g. "meditor-api-12345"). */
    QString serverName() const { return m_serverName; }

    /** @brief Full OS pipe/socket path returned by QLocalServer::fullServerName(). */
    QString fullServerName() const;

    /** @brief Path to `<AppLocalData>/meditor/api.json`. */
    QString apiJsonPath() const { return m_apiJsonPath; }

    /** @brief Random authentication token for this session. */
    QString token() const { return m_token; }

    /** @brief Supported JSON-RPC method names advertised on `hello`. */
    static QStringList supportedMethods();

    // --- API Call Depth / Origin Tracking ---
    void enterApiCall() { ++m_apiDepth; }
    void leaveApiCall()
    {
        if (m_apiDepth > 0) {
            --m_apiDepth;
        }
    }
    int apiDepth() const { return m_apiDepth; }
    bool isInApiCall() const { return m_apiDepth > 0; }
    QString currentOrigin() const { return m_apiDepth > 0 ? QStringLiteral("claude") : QStringLiteral("user"); }

    // --- Batch Undo (`undo.begin` / `undo.commit` / `undo.rollback`) ---
    bool inUndoBatch() const { return m_inUndoBatch; }
    bool isUndoBatchOpen() const { return m_inUndoBatch; }
    bool hasActiveUndoBatch() const { return m_inUndoBatch; }
    QString undoBatchLabel() const { return m_batchLabel; }
    int undoBatchStepCount() const { return m_batchStepCount; }

    /** @brief Direct references to the accumulated batch Fun lambdas (marks batch as touched). */
    Fun &batchUndo();
    Fun &batchRedo();
    Fun &batchUndoRef() { return batchUndo(); }
    Fun &batchRedoRef() { return batchRedo(); }

    /** @brief Begin accumulating `(Fun undo, Fun redo)` operations into a single undo macro step. */
    bool beginUndoBatch(const QString &label = QString());
    void beginUndo(const QString &label = QString()) { beginUndoBatch(label); }

    /** @brief Commit the accumulated `Fun undo, redo` in a single `pCore->pushUndo(undo, redo, label)` call. */
    QJsonObject commitUndoBatch(const QString &label = QString());
    bool commitUndo(const QString &label = QString());

    /** @brief Roll back uncommitted batch operations by invoking `m_batchUndo()`. */
    QJsonObject rollbackUndoBatch();

    /** @brief Chain `(undo, redo)` into the current batch (undo prepended, redo appended). */
    void accumulateUndo(Fun &undo, Fun &redo);
    void recordUndoStep(const Fun &undo, const Fun &redo);
    void addUndoStep(const Fun &undo, const Fun &redo) { recordUndoStep(undo, redo); }

    /**
     * @brief If an `undo.begin` batch is open, accumulate `(undo, redo)` into the batch;
     *        otherwise push immediately via `pCore->pushUndo(undo, redo, label)`.
     */
    void pushOrAccumulateUndo(Fun &undo, Fun &redo, const QString &label);
    void pushUndoOrAccumulate(const Fun &undo, const Fun &redo, const QString &label);

    // --- Event Bridge & JSON-RPC Notifications ---
    EventBridge *eventBridge() const { return m_eventBridge; }
    qint64 currentRev() const;
    int rev() const { return static_cast<int>(currentRev()); }
    qint64 bumpRev(const QString &reason = QString());

    /** @brief Broadcast a JSON-RPC 2.0 notification `{"jsonrpc":"2.0","method":method,"params":params}` to all authenticated clients. */
    void broadcastNotification(const QString &method, const QJsonObject &params);
    void sendNotification(const QString &method, const QJsonObject &params) { broadcastNotification(method, params); }

    /** @brief Emit an event notification `{"jsonrpc":"2.0","method":"event","params":{"seq":...,"type":type,"rev":...,...}}`. */
    void broadcastEvent(const QString &type, const QJsonObject &data = QJsonObject());
    void emitEvent(const QString &type, const QJsonObject &data = QJsonObject()) { broadcastEvent(type, data); }

    // --- JSON-RPC Response Helpers ---
    void sendResponse(QLocalSocket *socket, const QJsonValue &id, const QJsonValue &result);
    void sendError(QLocalSocket *socket, const QJsonValue &id, int code, const QString &message,
                   const QJsonValue &data = QJsonValue::Undefined);

    // Standard JSON-RPC 2.0 & MEditor error codes
    static constexpr int kErrParseError = -32700;
    static constexpr int kErrInvalidRequest = -32600;
    static constexpr int kErrMethodNotFound = -32601;
    static constexpr int kErrInvalidParams = -32602;
    static constexpr int kErrInternalError = -32603;
    static constexpr int kErrUnauthorized = -32001;
    static constexpr int kErrOperationFailed = -32002;

private Q_SLOTS:
    void onNewConnection();
    void onSocketReadyRead();

private:
    bool shouldStartServer(bool forceEnable) const;
    bool startServer();
    void stopServer();
    bool writeDiscoveryFile();
    void removeDiscoveryFile();
    static QString generateSecureToken();
    static bool constantTimeEquals(const QString &a, const QString &b);

    void readSocketMessages(QLocalSocket *socket);
    void handleJsonMessage(const QJsonObject &msg, QLocalSocket *socket);
    void writeJsonLine(QLocalSocket *socket, const QJsonObject &payload);

    QJsonObject handleHello(const QJsonObject &params, QLocalSocket *socket, bool *outOk, QString *outError);
    QJsonObject handlePing(QLocalSocket *socket) const;

    QLocalServer m_server;
    QString m_serverName;
    QString m_apiJsonPath;
    QString m_token;

    QHash<QLocalSocket *, QByteArray> m_buffers;
    QSet<QLocalSocket *> m_authenticatedClients;
    QHash<QLocalSocket *, QString> m_clientNames;

    int m_apiDepth{0};

    bool m_inUndoBatch{false};
    bool m_batchTouched{false};
    QString m_batchLabel;
    Fun m_batchUndo;
    Fun m_batchRedo;
    int m_batchStepCount{0};

    EventBridge *m_eventBridge{nullptr};
    MEditor::Handlers *m_handlers{nullptr};
};

namespace MEditor {
using ApiServer = ::MEditorApiServer;
} // namespace MEditor
