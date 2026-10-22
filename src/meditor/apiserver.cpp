/*
    SPDX-FileCopyrightText: 2026 MEditor Contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "apiserver.h"
#include "eventbridge.h"
#include "handlers.h"

#include "core.h"
#include "doc/docundostack.hpp"
#include "kdenlivesettings.h"
#include "project/projectmanager.h"
#include <config-kdenlive.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>

MEditorApiServer::MEditorApiServer(QObject *parent, bool forceEnable)
    : QObject(parent)
    , m_batchUndo([]() { return true; })
    , m_batchRedo([]() { return true; })
{
    if (!shouldStartServer(forceEnable)) {
        return;
    }

    m_eventBridge = new EventBridge(this, this);
    m_handlers = new MEditor::Handlers(this, this);
    m_handlers->setAgentScopeCallback([this](bool active) {
        if (active) {
            enterApiCall();
        } else {
            leaveApiCall();
        }
    });

    if (startServer()) {
        if (QCoreApplication *app = QCoreApplication::instance()) {
            connect(app, &QCoreApplication::aboutToQuit, this, &MEditorApiServer::stopServer);
        }
        // Defer EventBridge binding to next event-loop tick so MainWindow::init() finishes creating m_commandStack
        QTimer::singleShot(0, this, [this]() {
            if (m_eventBridge) {
                m_eventBridge->attachToCore();
            }
        });
    }
}

MEditorApiServer::~MEditorApiServer()
{
    stopServer();
}

bool MEditorApiServer::shouldStartServer(bool forceEnable) const
{
    if (forceEnable) {
        return true;
    }
    if (qEnvironmentVariableIntValue("MEDITOR_API") == 1) {
        return true;
    }
    const QStringList args = QCoreApplication::arguments();
    if (args.contains(QStringLiteral("--api"))) {
        return true;
    }
    return KdenliveSettings::meditor_api_enabled();
}

QString MEditorApiServer::generateSecureToken()
{
    QByteArray rawBytes(32, Qt::Uninitialized);
    QRandomGenerator *rng = QRandomGenerator::system();
    for (int i = 0; i < 32; i += 4) {
        const quint32 word = rng->generate();
        rawBytes[i + 0] = static_cast<char>((word >> 24) & 0xFF);
        rawBytes[i + 1] = static_cast<char>((word >> 16) & 0xFF);
        rawBytes[i + 2] = static_cast<char>((word >> 8) & 0xFF);
        rawBytes[i + 3] = static_cast<char>(word & 0xFF);
    }
    return QString::fromLatin1(rawBytes.toHex());
}

bool MEditorApiServer::constantTimeEquals(const QString &a, const QString &b)
{
    const QByteArray ba = a.toUtf8();
    const QByteArray bb = b.toUtf8();
    if (ba.size() != bb.size() || ba.isEmpty()) {
        return false;
    }
    unsigned char diff = 0;
    for (qsizetype i = 0; i < ba.size(); ++i) {
        diff |= static_cast<unsigned char>(ba[i] ^ bb[i]);
    }
    return diff == 0;
}

bool MEditorApiServer::startServer()
{
    m_token = generateSecureToken();
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_serverName = QStringLiteral("meditor-api-%1").arg(QCoreApplication::applicationPid());

    QLocalServer::removeServer(m_serverName);
    if (!m_server.listen(m_serverName)) {
        qWarning() << "MEditorApiServer: failed to listen on" << m_serverName << ":" << m_server.errorString();
        return false;
    }

    connect(&m_server, &QLocalServer::newConnection, this, &MEditorApiServer::onNewConnection);

    if (!writeDiscoveryFile()) {
        qWarning() << "MEditorApiServer: listening on" << m_server.fullServerName()
                   << "but failed to write discovery file" << m_apiJsonPath;
    } else {
        qInfo() << "MEditorApiServer: listening on" << m_server.fullServerName()
                << "discovery:" << m_apiJsonPath;
    }
    return true;
}

void MEditorApiServer::stopServer()
{
    removeDiscoveryFile();

    const QList<QLocalSocket *> sockets = m_buffers.keys();
    for (QLocalSocket *sock : sockets) {
        if (sock) {
            sock->disconnect(this);
            sock->close();
            sock->deleteLater();
        }
    }
    m_buffers.clear();
    m_authenticatedClients.clear();
    m_clientNames.clear();

    if (m_server.isListening()) {
        m_server.close();
    }
}

bool MEditorApiServer::writeDiscoveryFile()
{
    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::homePath() + QStringLiteral("/.local/share/meditor");
    }
    const QString meditorDir = QDir(baseDir).filePath(QStringLiteral("meditor"));
    QDir dir;
    if (!dir.mkpath(meditorDir)) {
        return false;
    }

    m_apiJsonPath = QDir(meditorDir).filePath(QStringLiteral("api.json"));

    QJsonObject discovery;
    discovery.insert(QStringLiteral("pipe"), m_server.fullServerName());
    discovery.insert(QStringLiteral("serverName"), m_serverName);
    discovery.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
    discovery.insert(QStringLiteral("token"), m_token);
    discovery.insert(QStringLiteral("version"), 1);
    discovery.insert(QStringLiteral("apiVersion"), 1);
    discovery.insert(QStringLiteral("appVersion"), QStringLiteral(KDENLIVE_VERSION));

    QFile file(m_apiJsonPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray payload = QJsonDocument(discovery).toJson(QJsonDocument::Indented);
    const qint64 written = file.write(payload);
    file.flush();
    file.close();
    QFile::setPermissions(m_apiJsonPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return written == payload.size();
}

void MEditorApiServer::removeDiscoveryFile()
{
    if (!m_apiJsonPath.isEmpty()) {
        QFile::remove(m_apiJsonPath);
        m_apiJsonPath.clear();
    }
}

bool MEditorApiServer::isListening() const
{
    return m_server.isListening();
}

QString MEditorApiServer::fullServerName() const
{
    return m_server.fullServerName();
}

QStringList MEditorApiServer::supportedMethods()
{
    return {
        QStringLiteral("hello"),
        QStringLiteral("ping"),
        QStringLiteral("project.open"),
        QStringLiteral("project.new"),
        QStringLiteral("project.save"),
        QStringLiteral("bin.import"),
        QStringLiteral("timeline.get"),
        QStringLiteral("clip.insert"),
        QStringLiteral("clip.cut"),
        QStringLiteral("clip.resize"),
        QStringLiteral("clip.delete"),
        QStringLiteral("clip.move"),
        QStringLiteral("clip.speed"),
        QStringLiteral("marker.add"),
        QStringLiteral("subtitle.add"),
        QStringLiteral("undo.begin"),
        QStringLiteral("undo.commit"),
        QStringLiteral("undo.rollback"),
        QStringLiteral("undo"),
        QStringLiteral("redo"),
        QStringLiteral("ui.status"),
        QStringLiteral("playhead.seek"),
    };
}

QList<QByteArray> MEditorApiServer::takeMessages(QByteArray &buffer)
{
    QList<QByteArray> messages;
    int newlineIdx = buffer.indexOf('\n');
    while (newlineIdx >= 0) {
        QByteArray line = buffer.left(newlineIdx);
        buffer.remove(0, newlineIdx + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (!line.trimmed().isEmpty()) {
            messages.append(line);
        }
        newlineIdx = buffer.indexOf('\n');
    }
    return messages;
}

void MEditorApiServer::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QLocalSocket *socket = m_server.nextPendingConnection();
        if (!socket) {
            continue;
        }
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, &MEditorApiServer::onSocketReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            readSocketMessages(socket);
            m_buffers.remove(socket);
            m_authenticatedClients.remove(socket);
            m_clientNames.remove(socket);
            socket->deleteLater();
        });
    }
}

void MEditorApiServer::onSocketReadyRead()
{
    readSocketMessages(qobject_cast<QLocalSocket *>(sender()));
}

void MEditorApiServer::readSocketMessages(QLocalSocket *socket)
{
    if (!socket || socket->bytesAvailable() <= 0) {
        return;
    }

    QByteArray &buffer = m_buffers[socket];
    buffer.append(socket->readAll());
    const QList<QByteArray> lines = takeMessages(buffer);
    for (const QByteArray &line : lines) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            sendError(socket, QJsonValue::Null, kErrParseError,
                      QStringLiteral("Parse error: %1").arg(parseError.errorString()));
            continue;
        }
        handleJsonMessage(doc.object(), socket);
    }
}

void MEditorApiServer::handleJsonMessage(const QJsonObject &msg, QLocalSocket *socket)
{
    const QJsonValue id = msg.contains(QStringLiteral("id")) ? msg.value(QStringLiteral("id")) : QJsonValue::Undefined;
    const bool isNotification = id.isUndefined();

    const QString jsonrpc = msg.value(QStringLiteral("jsonrpc")).toString(QStringLiteral("2.0"));
    const QString method = msg.value(QStringLiteral("method")).toString().trimmed();
    if (jsonrpc != QLatin1String("2.0") || method.isEmpty()) {
        if (!isNotification) {
            sendError(socket, id.isUndefined() ? QJsonValue::Null : id, kErrInvalidRequest,
                      QStringLiteral("Invalid JSON-RPC 2.0 request: missing 'method' or invalid 'jsonrpc' version"));
        }
        return;
    }

    QJsonObject params;
    if (msg.contains(QStringLiteral("params"))) {
        const QJsonValue rawParams = msg.value(QStringLiteral("params"));
        if (rawParams.isObject()) {
            params = rawParams.toObject();
        } else if (!rawParams.isNull() && !rawParams.isUndefined()) {
            if (!isNotification) {
                sendError(socket, id, kErrInvalidParams, QStringLiteral("'params' must be a JSON object"));
            }
            return;
        }
    }

    // 1. Always allow "ping" (used for readiness probing before/after hello)
    if (method == QLatin1String("ping")) {
        if (!isNotification) {
            sendResponse(socket, id, handlePing(socket));
        }
        return;
    }

    // 2. Handle "hello" authentication handshake
    if (method == QLatin1String("hello")) {
        bool ok = false;
        QString errMsg;
        const QJsonObject result = handleHello(params, socket, &ok, &errMsg);
        if (!isNotification) {
            if (ok) {
                sendResponse(socket, id, result);
            } else {
                sendError(socket, id, kErrUnauthorized, errMsg);
            }
        }
        return;
    }

    // 3. Require authentication for all other methods
    if (!m_authenticatedClients.contains(socket)) {
        if (!isNotification) {
            sendError(socket, id, kErrUnauthorized,
                      QStringLiteral("Unauthorized: call 'hello' with valid token before '%1'").arg(method));
        }
        return;
    }

    // 4. Execute within RAII ApiCallScope so any DocUndoStack commands are marked origin: "claude"
    //    (unless simulateUser=true is explicitly passed for automated testing of origin:"user" events)
    const bool simulateUser = params.value(QStringLiteral("simulateUser")).toBool(false);
    ApiCallScope apiScope(simulateUser ? nullptr : this);

    if (method == QLatin1String("undo.begin")) {
        if (m_inUndoBatch) {
            if (!isNotification) {
                sendError(socket, id, kErrOperationFailed, QStringLiteral("An undo batch is already active"));
            }
            return;
        }
        const QString label = params.value(QStringLiteral("label"))
                                  .toString(params.value(QStringLiteral("text")).toString(QStringLiteral("Claude: edição em lote")));
        beginUndoBatch(label);
        if (!isNotification) {
            QJsonObject res;
            res.insert(QStringLiteral("ok"), true);
            res.insert(QStringLiteral("label"), m_batchLabel);
            res.insert(QStringLiteral("rev"), currentRev());
            sendResponse(socket, id, res);
        }
        return;
    }

    if (method == QLatin1String("undo.commit") || method == QLatin1String("undo.end")) {
        if (!m_inUndoBatch) {
            if (!isNotification) {
                sendError(socket, id, kErrOperationFailed, QStringLiteral("No active undo batch to commit"));
            }
            return;
        }
        const QString label = params.value(QStringLiteral("label"))
                                  .toString(params.value(QStringLiteral("text")).toString());
        const QJsonObject res = commitUndoBatch(label);
        if (!isNotification) {
            sendResponse(socket, id, res);
        }
        return;
    }

    if (method == QLatin1String("undo.rollback") || method == QLatin1String("undo.abort")) {
        if (!m_inUndoBatch) {
            if (!isNotification) {
                sendError(socket, id, kErrOperationFailed, QStringLiteral("No active undo batch to rollback"));
            }
            return;
        }
        const QJsonObject res = rollbackUndoBatch();
        if (!isNotification) {
            sendResponse(socket, id, res);
        }
        return;
    }

    if (!m_handlers) {
        if (!isNotification) {
            sendError(socket, id, kErrInternalError, QStringLiteral("MEditorHandlers not initialized"));
        }
        return;
    }

    QJsonObject resultObj;
    int errCode = 0;
    QString errMsg;
    const bool ok = m_handlers->dispatch(method, params, resultObj, errCode, errMsg);
    if (!isNotification) {
        if (ok) {
            sendResponse(socket, id, resultObj);
        } else {
            sendError(socket, id, errCode != 0 ? errCode : kErrOperationFailed,
                      errMsg.isEmpty() ? QStringLiteral("Operation failed: %1").arg(method) : errMsg);
        }
    }
}

QJsonObject MEditorApiServer::handleHello(const QJsonObject &params, QLocalSocket *socket, bool *outOk, QString *outError)
{
    const QString candidateToken = params.value(QStringLiteral("token")).toString();
    if (!constantTimeEquals(candidateToken, m_token)) {
        m_authenticatedClients.remove(socket);
        if (outOk) {
            *outOk = false;
        }
        if (outError) {
            *outError = QStringLiteral("Invalid authentication token");
        }
        return {};
    }

    m_authenticatedClients.insert(socket);
    const QString clientName = params.value(QStringLiteral("client")).toString(QStringLiteral("mcp-client"));
    m_clientNames.insert(socket, clientName);

    if (outOk) {
        *outOk = true;
    }

    QJsonArray methodsArr;
    for (const QString &m : supportedMethods()) {
        methodsArr.append(m);
    }

    QJsonObject res;
    res.insert(QStringLiteral("ok"), true);
    res.insert(QStringLiteral("authenticated"), true);
    res.insert(QStringLiteral("version"), 1);
    res.insert(QStringLiteral("apiVersion"), 1);
    res.insert(QStringLiteral("appVersion"), QStringLiteral(KDENLIVE_VERSION));
    res.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
    res.insert(QStringLiteral("rev"), currentRev());
    res.insert(QStringLiteral("methods"), methodsArr);
    return res;
}

QJsonObject MEditorApiServer::handlePing(QLocalSocket *socket) const
{
    QJsonObject res;
    res.insert(QStringLiteral("ok"), true);
    res.insert(QStringLiteral("pong"), true);
    res.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
    res.insert(QStringLiteral("version"), 1);
    res.insert(QStringLiteral("authenticated"), m_authenticatedClients.contains(socket));
    res.insert(QStringLiteral("rev"), currentRev());
    return res;
}

Fun &MEditorApiServer::batchUndo()
{
    m_batchTouched = true;
    return m_batchUndo;
}

Fun &MEditorApiServer::batchRedo()
{
    m_batchTouched = true;
    return m_batchRedo;
}

bool MEditorApiServer::beginUndoBatch(const QString &label)
{
    m_inUndoBatch = true;
    m_batchTouched = false;
    m_batchLabel = label.isEmpty() ? QStringLiteral("Claude: edição em lote") : label;
    m_batchUndo = []() { return true; };
    m_batchRedo = []() { return true; };
    m_batchStepCount = 0;
    return true;
}

QJsonObject MEditorApiServer::commitUndoBatch(const QString &label)
{
    const QString finalLabel = !label.isEmpty()
                                   ? label
                                   : (m_batchLabel.isEmpty() ? QStringLiteral("Claude: edição em lote") : m_batchLabel);
    const int steps = m_batchStepCount;
    const bool shouldPush = (m_batchStepCount > 0 || m_batchTouched);

    Fun undoToPush = std::move(m_batchUndo);
    Fun redoToPush = std::move(m_batchRedo);

    m_inUndoBatch = false;
    m_batchTouched = false;
    m_batchLabel.clear();
    m_batchStepCount = 0;
    m_batchUndo = []() { return true; };
    m_batchRedo = []() { return true; };

    if (shouldPush && pCore) {
        ApiCallScope scope(this);
        pCore->pushUndo(undoToPush, redoToPush, finalLabel);
    }

    int undoIdx = 0;
    if (pCore && pCore->projectManager() && pCore->projectManager()->undoStack()) {
        undoIdx = pCore->projectManager()->undoStack()->index();
    }

    QJsonObject res;
    res.insert(QStringLiteral("ok"), true);
    res.insert(QStringLiteral("committed"), true);
    res.insert(QStringLiteral("committedOperations"), steps);
    res.insert(QStringLiteral("steps"), steps);
    res.insert(QStringLiteral("label"), finalLabel);
    res.insert(QStringLiteral("undoIndex"), undoIdx);
    res.insert(QStringLiteral("rev"), currentRev());
    return res;
}

bool MEditorApiServer::commitUndo(const QString &label)
{
    if (!m_inUndoBatch) {
        return false;
    }
    commitUndoBatch(label);
    return true;
}

QJsonObject MEditorApiServer::rollbackUndoBatch()
{
    const int steps = m_batchStepCount;
    const bool hadChanges = (m_batchStepCount > 0 || m_batchTouched);
    Fun undoToRun = std::move(m_batchUndo);

    m_inUndoBatch = false;
    m_batchTouched = false;
    m_batchLabel.clear();
    m_batchStepCount = 0;
    m_batchUndo = []() { return true; };
    m_batchRedo = []() { return true; };

    bool rolledBack = true;
    if (hadChanges && undoToRun) {
        ApiCallScope scope(this);
        rolledBack = undoToRun();
    }

    QJsonObject res;
    res.insert(QStringLiteral("ok"), rolledBack);
    res.insert(QStringLiteral("rolledBack"), rolledBack);
    res.insert(QStringLiteral("rolledBackOperations"), steps);
    res.insert(QStringLiteral("steps"), steps);
    res.insert(QStringLiteral("rev"), currentRev());
    return res;
}

void MEditorApiServer::accumulateUndo(Fun &undo, Fun &redo)
{
    PUSH_LAMBDA(redo, m_batchRedo);
    PUSH_FRONT_LAMBDA(undo, m_batchUndo);
    ++m_batchStepCount;
}

void MEditorApiServer::recordUndoStep(const Fun &undo, const Fun &redo)
{
    Fun u = undo;
    Fun r = redo;
    accumulateUndo(u, r);
}

void MEditorApiServer::pushOrAccumulateUndo(Fun &undo, Fun &redo, const QString &label)
{
    if (m_inUndoBatch) {
        accumulateUndo(undo, redo);
    } else if (pCore) {
        pCore->pushUndo(undo, redo, label.isEmpty() ? QStringLiteral("Claude: operação") : label);
    }
}

void MEditorApiServer::pushUndoOrAccumulate(const Fun &undo, const Fun &redo, const QString &label)
{
    Fun u = undo;
    Fun r = redo;
    pushOrAccumulateUndo(u, r, label);
}

qint64 MEditorApiServer::currentRev() const
{
    if (m_eventBridge) {
        return m_eventBridge->currentRev();
    }
    return 0;
}

qint64 MEditorApiServer::bumpRev(const QString &reason)
{
    if (m_eventBridge) {
        return m_eventBridge->bumpRev(reason);
    }
    return 0;
}

void MEditorApiServer::broadcastNotification(const QString &method, const QJsonObject &params)
{
    if (m_authenticatedClients.isEmpty()) {
        return;
    }

    QJsonObject msg;
    msg.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    msg.insert(QStringLiteral("method"), method);
    msg.insert(QStringLiteral("params"), params);

    const QList<QLocalSocket *> clients = m_authenticatedClients.values();
    for (QLocalSocket *client : clients) {
        if (client && client->state() == QLocalSocket::ConnectedState) {
            writeJsonLine(client, msg);
        }
    }
}

void MEditorApiServer::broadcastEvent(const QString &type, const QJsonObject &data)
{
    if (m_eventBridge) {
        m_eventBridge->emitEvent(type, data);
    }
}

void MEditorApiServer::sendResponse(QLocalSocket *socket, const QJsonValue &id, const QJsonValue &result)
{
    if (!socket) {
        return;
    }
    QJsonObject msg;
    msg.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    msg.insert(QStringLiteral("id"), id);
    msg.insert(QStringLiteral("result"), result);
    writeJsonLine(socket, msg);
}

void MEditorApiServer::sendError(QLocalSocket *socket, const QJsonValue &id, int code, const QString &message,
                                 const QJsonValue &data)
{
    if (!socket) {
        return;
    }
    QJsonObject errObj;
    errObj.insert(QStringLiteral("code"), code);
    errObj.insert(QStringLiteral("message"), message);
    if (!data.isUndefined()) {
        errObj.insert(QStringLiteral("data"), data);
    }

    QJsonObject msg;
    msg.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    msg.insert(QStringLiteral("id"), id.isUndefined() ? QJsonValue::Null : id);
    msg.insert(QStringLiteral("error"), errObj);
    writeJsonLine(socket, msg);
}

void MEditorApiServer::writeJsonLine(QLocalSocket *socket, const QJsonObject &payload)
{
    if (!socket || !socket->isWritable()) {
        return;
    }
    QByteArray line = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    line.append('\n');
    socket->write(line);
    socket->flush();
}
