/*
    SPDX-FileCopyrightText: 2026 MEditor Project
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "handlers.h"
#include "apiserver.h"
#include "jsonserialize.h"

#include "bin/bin.h"
#include "bin/clipcreator.hpp"
#include "bin/model/markerlistmodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "definitions.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "mainwindow.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <atomic>
#include <cmath>
#include <mlt++/MltProducer.h>

namespace MEditor {

namespace {

struct AgentScopeGuard
{
    Handlers::ScopeCallback cb;
    explicit AgentScopeGuard(Handlers::ScopeCallback c)
        : cb(std::move(c))
    {
        if (cb) {
            cb(true);
        }
    }
    ~AgentScopeGuard()
    {
        if (cb) {
            cb(false);
        }
    }
};

std::shared_ptr<TimelineItemModel> activeTimeline()
{
    if (pCore->projectManager()) {
        if (auto tl = pCore->projectManager()->getTimeline()) {
            return tl;
        }
    }
    if (KdenliveDoc *doc = pCore->currentDoc()) {
        return doc->getTimeline(doc->uuid(), true);
    }
    return nullptr;
}

double activeFps()
{
    if (KdenliveDoc *doc = pCore->currentDoc()) {
        if (doc->timecode().fps() > 0.0) {
            return doc->timecode().fps();
        }
    }
    const double fps = pCore->getCurrentFps();
    return fps > 0.0 ? fps : 30.0;
}

int parseFrameParam(const QJsonObject &params, const QString &frameKey, const QString &secKey, double fps,
                    int defaultVal = -1)
{
    if (params.contains(frameKey) && !params.value(frameKey).isNull()) {
        return params.value(frameKey).toInt(defaultVal);
    }
    if (!secKey.isEmpty() && params.contains(secKey) && !params.value(secKey).isNull()) {
        const double sec = params.value(secKey).toDouble(-1.0);
        if (sec >= 0.0) {
            return int(std::llround(sec * fps));
        }
    }
    return defaultVal;
}

void setClipCustomProperty(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, const char *key,
                           const QString &value)
{
    if (!timeline || clipId < 0 || value.isEmpty()) {
        return;
    }
    std::shared_ptr<Mlt::Producer> prod = timeline->getClipProducer(clipId);
    if (prod && prod->is_valid()) {
        const QByteArray utf8 = value.toUtf8();
        prod->set(key, utf8.constData());
    }
}

} // namespace

Handlers::Handlers(QObject *parent)
    : QObject(parent)
    , m_batchUndo([]() { return true; })
    , m_batchRedo([]() { return true; })
{
}

Handlers::Handlers(MEditorApiServer *server, QObject *parent)
    : QObject(parent ? parent : server)
    , m_batchUndo([]() { return true; })
    , m_batchRedo([]() { return true; })
    , m_apiServer(server)
{
}

void Handlers::setApiServer(MEditorApiServer *server)
{
    m_apiServer = server;
}

void Handlers::setEventCallback(EventCallback cb)
{
    m_eventCallback = std::move(cb);
}

void Handlers::setAgentScopeCallback(ScopeCallback cb)
{
    m_scopeCallback = std::move(cb);
}

void Handlers::emitEvent(const QString &eventType, const QJsonObject &payload)
{
    if (m_apiServer) {
        m_apiServer->broadcastEvent(eventType, payload);
    }
    if (m_eventCallback) {
        m_eventCallback(eventType, payload);
    }
    Q_EMIT eventEmitted(eventType, payload);
}

void Handlers::finalizeOperationUndo(Fun &localUndo, Fun &localRedo, const QString &defaultLabel)
{
    if (m_apiServer) {
        m_apiServer->pushOrAccumulateUndo(localUndo, localRedo, defaultLabel);
        return;
    }
    if (m_inBatch) {
        PUSH_LAMBDA(localRedo, m_batchRedo);
        PUSH_FRONT_LAMBDA(localUndo, m_batchUndo);
        ++m_batchOpCount;
    } else {
        pCore->pushUndo(localUndo, localRedo, defaultLabel);
    }
}

bool Handlers::dispatch(MEditorApiServer *server, const QString &method, const QJsonObject &params,
                        QJsonObject &outResult, int &outErrorCode, QString &outErrorMessage)
{
    Handlers h(server);
    return h.dispatch(method, params, outResult, outErrorCode, outErrorMessage);
}

bool Handlers::dispatch(const QString &method, const QJsonObject &params, QJsonObject &outResult, int &outErrorCode,
                        QString &outErrorMessage)
{
    outErrorCode = 0;
    outErrorMessage.clear();

    const bool simulateUser = params.value(QStringLiteral("simulateUser")).toBool(false);
    AgentScopeGuard scopeGuard(simulateUser ? ScopeCallback{} : m_scopeCallback);

    auto run = [&](bool (Handlers::*fn)(const QJsonObject &, QJsonObject &, QString &)) -> bool {
        const bool ok = (this->*fn)(params, outResult, outErrorMessage);
        if (!ok && outErrorCode == 0) {
            outErrorCode = -32000;
        }
        return ok;
    };

    if (method == QLatin1String("ping")) {
        outResult.insert(QStringLiteral("pong"), true);
        outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
        return true;
    }
    if (method == QLatin1String("undo.begin")) {
        return run(&Handlers::handleUndoBegin);
    }
    if (method == QLatin1String("undo.commit") || method == QLatin1String("undo.end")) {
        return run(&Handlers::handleUndoCommit);
    }
    if (method == QLatin1String("undo.rollback") || method == QLatin1String("undo.abort")) {
        return run(&Handlers::handleUndoRollback);
    }
    if (method == QLatin1String("project.open")) {
        return run(&Handlers::handleProjectOpen);
    }
    if (method == QLatin1String("project.new")) {
        return run(&Handlers::handleProjectNew);
    }
    if (method == QLatin1String("project.save")) {
        return run(&Handlers::handleProjectSave);
    }
    if (method == QLatin1String("bin.import")) {
        return run(&Handlers::handleBinImport);
    }
    if (method == QLatin1String("timeline.get")) {
        return run(&Handlers::handleTimelineGet);
    }
    if (method == QLatin1String("clip.insert")) {
        return run(&Handlers::handleClipInsert);
    }
    if (method == QLatin1String("clip.cut")) {
        return run(&Handlers::handleClipCut);
    }
    if (method == QLatin1String("clip.resize")) {
        return run(&Handlers::handleClipResize);
    }
    if (method == QLatin1String("clip.delete") || method == QLatin1String("clip.remove")) {
        return run(&Handlers::handleClipDelete);
    }
    if (method == QLatin1String("clip.move")) {
        return run(&Handlers::handleClipMove);
    }
    if (method == QLatin1String("clip.speed")) {
        return run(&Handlers::handleClipSpeed);
    }
    if (method == QLatin1String("marker.add") || method == QLatin1String("guide.add")) {
        return run(&Handlers::handleMarkerAdd);
    }
    if (method == QLatin1String("subtitle.add")) {
        return run(&Handlers::handleSubtitleAdd);
    }
    if (method == QLatin1String("ui.status")) {
        return run(&Handlers::handleUiStatus);
    }
    if (method == QLatin1String("playhead.seek")) {
        return run(&Handlers::handlePlayheadSeek);
    }
    if (method == QLatin1String("undo")) {
        return run(&Handlers::handleUndo);
    }
    if (method == QLatin1String("redo")) {
        return run(&Handlers::handleRedo);
    }

    outErrorCode = -32601;
    outErrorMessage = QStringLiteral("Method not found: %1").arg(method);
    return false;
}

bool Handlers::handleUndoBegin(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    const QString label = params.value(QStringLiteral("label"))
                              .toString(params.value(QStringLiteral("text")).toString(QStringLiteral("MEditor AI Edit")));
    if (m_apiServer) {
        if (!m_apiServer->beginUndoBatch(label)) {
            outError = QStringLiteral("An undo batch is already active");
            return false;
        }
        outResult.insert(QStringLiteral("ok"), true);
        outResult.insert(QStringLiteral("label"), label);
        outResult.insert(QStringLiteral("rev"), m_apiServer->rev());
        return true;
    }
    if (m_inBatch) {
        outError = QStringLiteral("An undo batch is already active");
        return false;
    }
    m_inBatch = true;
    m_batchLabel = label;
    m_batchOpCount = 0;
    m_batchUndo = []() { return true; };
    m_batchRedo = []() { return true; };

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("label"), m_batchLabel);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleUndoCommit(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    const QString label = params.value(QStringLiteral("label"))
                              .toString(params.value(QStringLiteral("text")).toString(m_batchLabel));
    if (m_apiServer) {
        if (!m_apiServer->inUndoBatch()) {
            outError = QStringLiteral("No active undo batch to commit");
            return false;
        }
        outResult = m_apiServer->commitUndoBatch(label);
        outResult.insert(QStringLiteral("ok"), true);
        return true;
    }
    if (!m_inBatch) {
        outError = QStringLiteral("No active undo batch to commit");
        return false;
    }

    const int opCount = m_batchOpCount;
    Fun undoToPush = std::move(m_batchUndo);
    Fun redoToPush = std::move(m_batchRedo);

    m_inBatch = false;
    m_batchLabel.clear();
    m_batchOpCount = 0;
    m_batchUndo = []() { return true; };
    m_batchRedo = []() { return true; };

    if (opCount > 0) {
        pCore->pushUndo(undoToPush, redoToPush, label.isEmpty() ? QStringLiteral("MEditor AI Edit") : label);
    }

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("committedOperations"), opCount);
    outResult.insert(QStringLiteral("label"), label);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleUndoRollback(const QJsonObject &, QJsonObject &outResult, QString &outError)
{
    if (m_apiServer) {
        if (!m_apiServer->inUndoBatch()) {
            outError = QStringLiteral("No active undo batch to rollback");
            return false;
        }
        outResult = m_apiServer->rollbackUndoBatch();
        return outResult.value(QStringLiteral("ok")).toBool(true);
    }
    if (!m_inBatch) {
        outError = QStringLiteral("No active undo batch to rollback");
        return false;
    }

    const int opCount = m_batchOpCount;
    Fun undoToRun = std::move(m_batchUndo);

    m_inBatch = false;
    m_batchLabel.clear();
    m_batchOpCount = 0;
    m_batchUndo = []() { return true; };
    m_batchRedo = []() { return true; };

    bool rolledBack = true;
    if (opCount > 0 && undoToRun) {
        rolledBack = undoToRun();
    }

    outResult.insert(QStringLiteral("ok"), rolledBack);
    outResult.insert(QStringLiteral("rolledBackOperations"), opCount);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return rolledBack;
}

bool Handlers::handleProjectOpen(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    const QString rawPath = params.value(QStringLiteral("path")).toString().trimmed();
    if (rawPath.isEmpty()) {
        outError = QStringLiteral("project.open requires 'path'");
        return false;
    }

    const QFileInfo fi(rawPath);
    if (!fi.exists() || !fi.isFile()) {
        outError = QStringLiteral("Project file does not exist: %1").arg(rawPath);
        return false;
    }

    if (!pCore->projectManager()) {
        outError = QStringLiteral("ProjectManager is not initialized");
        return false;
    }

    if (m_inBatch) {
        m_inBatch = false;
        m_batchOpCount = 0;
    }

    const QString absPath = fi.absoluteFilePath();
    pCore->projectManager()->openFile(QUrl::fromLocalFile(absPath));

    KdenliveDoc *doc = pCore->currentDoc();
    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("path"), absPath);
    outResult.insert(QStringLiteral("uuid"),
                     doc ? doc->uuid().toString(QUuid::WithoutBraces) : QString());
    outResult.insert(QStringLiteral("fps"), activeFps());
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());

    QJsonObject evPayload;
    evPayload.insert(QStringLiteral("path"), absPath);
    evPayload.insert(QStringLiteral("uuid"), outResult.value(QStringLiteral("uuid")));
    evPayload.insert(QStringLiteral("fps"), activeFps());
    evPayload.insert(QStringLiteral("origin"), QStringLiteral("claude"));
    emitEvent(QStringLiteral("project.loaded"), evPayload);

    return true;
}

bool Handlers::handleProjectNew(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    if (!pCore->projectManager()) {
        outError = QStringLiteral("ProjectManager is not initialized");
        return false;
    }

    if (m_inBatch) {
        m_inBatch = false;
        m_batchOpCount = 0;
    }

    const QString profile = params.value(QStringLiteral("profile")).toString().trimmed();
    const bool showSettings = params.value(QStringLiteral("showProjectSettings")).toBool(false);

    if (!profile.isEmpty()) {
        pCore->projectManager()->newFile(profile, showSettings);
    } else {
        pCore->projectManager()->newFile(showSettings);
    }

    KdenliveDoc *doc = pCore->currentDoc();
    const QSize frameSize = pCore->getCurrentFrameSize();

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("uuid"),
                     doc ? doc->uuid().toString(QUuid::WithoutBraces) : QString());
    outResult.insert(QStringLiteral("fps"), activeFps());
    outResult.insert(QStringLiteral("width"), doc ? doc->width() : frameSize.width());
    outResult.insert(QStringLiteral("height"), doc ? doc->height() : frameSize.height());
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());

    QJsonObject evPayload;
    evPayload.insert(QStringLiteral("uuid"), outResult.value(QStringLiteral("uuid")));
    evPayload.insert(QStringLiteral("fps"), outResult.value(QStringLiteral("fps")));
    evPayload.insert(QStringLiteral("width"), outResult.value(QStringLiteral("width")));
    evPayload.insert(QStringLiteral("height"), outResult.value(QStringLiteral("height")));
    evPayload.insert(QStringLiteral("origin"), QStringLiteral("claude"));
    emitEvent(QStringLiteral("project.loaded"), evPayload);

    return true;
}

bool Handlers::handleProjectSave(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    if (!pCore->projectManager()) {
        outError = QStringLiteral("ProjectManager is not initialized");
        return false;
    }

    const QString rawPath = params.value(QStringLiteral("path")).toString().trimmed();
    bool saved = false;
    QString finalPath = rawPath;

    if (!rawPath.isEmpty()) {
        const QFileInfo fi(rawPath);
        finalPath = fi.absoluteFilePath();
        const bool saveACopy = params.value(QStringLiteral("saveACopy")).toBool(false);
        saved = pCore->projectManager()->saveFileAs(finalPath, /*saveOverExistingFile=*/true, saveACopy);
    } else {
        saved = pCore->projectManager()->saveFile();
        if (KdenliveDoc *doc = pCore->currentDoc()) {
            finalPath = doc->url().toLocalFile();
        }
    }

    if (!saved) {
        outError = QStringLiteral("Failed to save project%1")
                       .arg(finalPath.isEmpty() ? QString() : QStringLiteral(": ") + finalPath);
        return false;
    }

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("path"), finalPath);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleBinImport(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    const QString rawPath =
        params.value(QStringLiteral("path")).toString(params.value(QStringLiteral("url")).toString()).trimmed();
    if (rawPath.isEmpty()) {
        outError = QStringLiteral("bin.import requires 'path'");
        return false;
    }

    const QFileInfo fi(rawPath);
    if (!fi.exists() || !fi.isFile()) {
        outError = QStringLiteral("Media file does not exist: %1").arg(rawPath);
        return false;
    }

    const QString absPath = fi.absoluteFilePath();
    auto itemModel = pCore->projectItemModel();
    if (!itemModel) {
        outError = QStringLiteral("ProjectItemModel is not available");
        return false;
    }

    // Se reuseExisting=true e o arquivo já está no Bin, reaproveita o binId existente
    const bool reuseExisting = params.value(QStringLiteral("reuseExisting")).toBool(false);
    if (reuseExisting) {
        for (const QString &existingId : itemModel->getAllClipIds()) {
            auto clip = itemModel->getClipByBinID(existingId);
            if (clip && QFileInfo(clip->clipUrl()).absoluteFilePath() == absPath) {
                const bool isReady = clip->clipStatus() == FileStatus::StatusReady;
                outResult.insert(QStringLiteral("ok"), true);
                outResult.insert(QStringLiteral("binId"), existingId);
                outResult.insert(QStringLiteral("path"), absPath);
                outResult.insert(QStringLiteral("name"), clip->clipName());
                outResult.insert(QStringLiteral("duration"), isReady ? int(clip->frameDuration()) : 0);
                outResult.insert(QStringLiteral("ready"), isReady);
                outResult.insert(QStringLiteral("reused"), true);
                if (isReady) {
                    QJsonObject ev;
                    ev.insert(QStringLiteral("binId"), existingId);
                    ev.insert(QStringLiteral("path"), absPath);
                    ev.insert(QStringLiteral("name"), clip->clipName());
                    ev.insert(QStringLiteral("duration"), int(clip->frameDuration()));
                    ev.insert(QStringLiteral("hasAudioAndVideo"), clip->hasAudioAndVideo());
                    ev.insert(QStringLiteral("origin"), QStringLiteral("claude"));
                    emitEvent(QStringLiteral("bin.clipReady"), ev);
                }
                return true;
            }
        }
    }

    const QString folderId = params.value(QStringLiteral("folderId")).toString(QStringLiteral("-1"));
    const bool checkProfile = params.value(QStringLiteral("checkProfile")).toBool(false);

    Fun localUndo = []() { return true; };
    Fun localRedo = []() { return true; };

    auto emittedFlag = std::make_shared<std::atomic_bool>(false);
    QPointer<Handlers> self(this);

    auto emitClipReady = [self, absPath, emittedFlag](const QString &readyBinId) {
        if (!self) {
            return;
        }
        bool expected = false;
        if (!emittedFlag->compare_exchange_strong(expected, true)) {
            return;
        }
        QMetaObject::invokeMethod(
            self,
            [self, readyBinId, absPath]() {
                if (!self) {
                    return;
                }
                QJsonObject ev;
                ev.insert(QStringLiteral("binId"), readyBinId);
                ev.insert(QStringLiteral("path"), absPath);
                ev.insert(QStringLiteral("origin"), QStringLiteral("claude"));
                if (auto model = pCore->projectItemModel()) {
                    if (auto clip = model->getClipByBinID(readyBinId)) {
                        const bool isClipReady = clip->clipStatus() == FileStatus::StatusReady;
                        ev.insert(QStringLiteral("name"), clip->clipName());
                        ev.insert(QStringLiteral("duration"), isClipReady ? int(clip->frameDuration()) : 0);
                        ev.insert(QStringLiteral("playtime"), isClipReady ? clip->getFramePlaytime() : 0);
                        ev.insert(QStringLiteral("hasAudioAndVideo"), isClipReady ? clip->hasAudioAndVideo() : false);
                        ev.insert(QStringLiteral("type"), int(clip->clipType()));
                    }
                }
                self->emitEvent(QStringLiteral("bin.clipReady"), ev);
            },
            Qt::QueuedConnection);
    };

    const QString binId = ClipCreator::createClipFromFile(absPath, folderId, itemModel, localUndo, localRedo,
                                                          emitClipReady, checkProfile);
    if (binId == QLatin1String("-1") || binId.isEmpty()) {
        outError = QStringLiteral("ClipCreator::createClipFromFile failed for: %1").arg(absPath);
        return false;
    }

    finalizeOperationUndo(localUndo, localRedo, QStringLiteral("Import clip"));

    bool isImmediatelyReady = false;
    int durationFrames = 0;
    QString clipName = fi.fileName();
    if (auto clip = itemModel->getClipByBinID(binId)) {
        isImmediatelyReady = (clip->clipStatus() == FileStatus::StatusReady);
        if (!clip->clipName().isEmpty()) {
            clipName = clip->clipName();
        }
        if (isImmediatelyReady) {
            durationFrames = int(clip->frameDuration());
            emitClipReady(binId);
        }
    }

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("binId"), binId);
    outResult.insert(QStringLiteral("path"), absPath);
    outResult.insert(QStringLiteral("name"), clipName);
    outResult.insert(QStringLiteral("duration"), durationFrames);
    outResult.insert(QStringLiteral("ready"), isImmediatelyReady);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleTimelineGet(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    if (!activeTimeline() && !pCore->currentDoc()) {
        outError = QStringLiteral("No active timeline or document");
        return false;
    }

    const int sinceRev = params.value(QStringLiteral("sinceRev")).toInt(0);
    const int currentRev = m_apiServer ? m_apiServer->rev() : pCore->undoIndex();
    outResult = JsonSerialize::serializeTimeline(currentRev, sinceRev);
    return true;
}

bool Handlers::handleClipInsert(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    auto timeline = activeTimeline();
    if (!timeline) {
        outError = QStringLiteral("No active timeline");
        return false;
    }

    QString binId;
    if (params.value(QStringLiteral("binId")).isDouble()) {
        binId = QString::number(params.value(QStringLiteral("binId")).toInt());
    } else {
        binId = params.value(QStringLiteral("binId")).toString().trimmed();
    }
    if (binId.isEmpty()) {
        outError = QStringLiteral("clip.insert requires 'binId'");
        return false;
    }

    const bool audioOnly = params.value(QStringLiteral("audioOnly")).toBool(false);
    const bool videoOnly = params.value(QStringLiteral("videoOnly")).toBool(false);

    const QJsonValue trackVal = params.contains(QStringLiteral("trackId")) ? params.value(QStringLiteral("trackId"))
                                                                           : params.value(QStringLiteral("track"));
    const int trackId = JsonSerialize::resolveTrackId(timeline, trackVal, audioOnly);
    if (trackId < 0) {
        outError = QStringLiteral("Could not resolve target track for clip.insert");
        return false;
    }

    const double fps = activeFps();
    int position = parseFrameParam(params, QStringLiteral("position"), QStringLiteral("positionSeconds"), fps, -1);
    if (position < 0) {
        position = parseFrameParam(params, QStringLiteral("pos"), QStringLiteral("posSeconds"), fps, 0);
    }

    int inFrame = parseFrameParam(params, QStringLiteral("in"), QStringLiteral("inSeconds"), fps, -1);
    if (inFrame < 0) {
        inFrame = params.value(QStringLiteral("inFrame")).toInt(-1);
    }
    int outFrame = parseFrameParam(params, QStringLiteral("out"), QStringLiteral("outSeconds"), fps, -1);
    if (outFrame < 0) {
        outFrame = params.value(QStringLiteral("outFrame")).toInt(-1);
    }

    // Constrói string binClipId no formato aceito por TimelineModel::requestClipInsertion ("binId" ou "binId/in/out")
    QString baseBinSpec = binId;
    if (inFrame >= 0 && outFrame >= inFrame) {
        baseBinSpec = QStringLiteral("%1/%2/%3").arg(binId).arg(inFrame).arg(outFrame);
    }

    const bool isTargetAudio = timeline->isAudioTrack(trackId);
    QString primarySpec = baseBinSpec;
    if (audioOnly || isTargetAudio) {
        if (!primarySpec.startsWith(QLatin1Char('A')) && !primarySpec.startsWith(QLatin1Char('V'))) {
            primarySpec.prepend(QLatin1Char('A'));
        }
    } else if (videoOnly) {
        if (!primarySpec.startsWith(QLatin1Char('A')) && !primarySpec.startsWith(QLatin1Char('V'))) {
            primarySpec.prepend(QLatin1Char('V'));
        }
    }

    int newClipId = -1;
    Fun localUndo = []() { return true; };
    Fun localRedo = []() { return true; };

    bool inserted = timeline->requestClipInsertion(primarySpec, trackId, position, newClipId, /*logUndo=*/false,
                                                   /*refreshView=*/true, /*useTargets=*/false, localUndo, localRedo);

    // Fallback: se o drop AV falhou porque a trilha espelho de áudio não estava livre/disponível,
    // tenta inserção apenas na trilha solicitada (V ou A)
    if (!inserted && !audioOnly && !videoOnly && !primarySpec.startsWith(QLatin1Char('V')) &&
        !primarySpec.startsWith(QLatin1Char('A'))) {
        localUndo = []() { return true; };
        localRedo = []() { return true; };
        const QString fallbackSpec = (isTargetAudio ? QStringLiteral("A") : QStringLiteral("V")) + baseBinSpec;
        inserted = timeline->requestClipInsertion(fallbackSpec, trackId, position, newClipId, /*logUndo=*/false,
                                                  /*refreshView=*/true, /*useTargets=*/false, localUndo, localRedo);
    }

    if (!inserted || newClipId < 0) {
        outError = QStringLiteral("TimelineModel::requestClipInsertion failed (binId=%1, trackId=%2, pos=%3)")
                       .arg(binId)
                       .arg(trackId)
                       .arg(position);
        return false;
    }

    QString uid = params.value(QStringLiteral("meditor_uid"))
                      .toString(params.value(QStringLiteral("uid")).toString())
                      .trimmed();
    if (uid.isEmpty()) {
        uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const QString reason = params.value(QStringLiteral("reason")).toString();
    const QString kind =
        params.value(QStringLiteral("kind")).toString(isTargetAudio ? QStringLiteral("audio") : QStringLiteral("video"));
    const QString keepIndexStr =
        params.contains(QStringLiteral("keepIndex")) ? QString::number(params.value(QStringLiteral("keepIndex")).toInt())
                                                     : QString();

    auto applyMetadata = [&](int cid) {
        if (cid < 0) {
            return;
        }
        setClipCustomProperty(timeline, cid, "meditor:uid", uid);
        setClipCustomProperty(timeline, cid, "kdenlive:meditor_uid", uid);
        setClipCustomProperty(timeline, cid, "meditor:kind", kind);
        if (!reason.isEmpty()) {
            setClipCustomProperty(timeline, cid, "meditor:reason", reason);
        }
        if (!keepIndexStr.isEmpty()) {
            setClipCustomProperty(timeline, cid, "meditor:keep_index", keepIndexStr);
        }
    };

    const int splitPartnerId = timeline->getClipSplitPartner(newClipId);
    applyMetadata(newClipId);
    if (splitPartnerId >= 0) {
        applyMetadata(splitPartnerId);
    }

    finalizeOperationUndo(localUndo, localRedo, QStringLiteral("Insert clip"));

    outResult = JsonSerialize::serializeClip(timeline, newClipId, fps);
    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleClipCut(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    auto timeline = activeTimeline();
    if (!timeline) {
        outError = QStringLiteral("No active timeline");
        return false;
    }

    const int clipId = params.value(QStringLiteral("clipId")).toInt(params.value(QStringLiteral("id")).toInt(-1));
    if (clipId < 0 || !timeline->isClip(clipId)) {
        outError = QStringLiteral("Invalid or unknown clipId: %1").arg(clipId);
        return false;
    }

    const double fps = activeFps();
    int position = parseFrameParam(params, QStringLiteral("position"), QStringLiteral("positionSeconds"), fps, -1);
    if (position < 0) {
        position = parseFrameParam(params, QStringLiteral("frame"), QStringLiteral("seconds"), fps, -1);
    }
    if (position < 0) {
        position = parseFrameParam(params, QStringLiteral("pos"), QStringLiteral("posSeconds"), fps, -1);
    }

    const int clipStart = timeline->getClipPosition(clipId);
    const int clipPlaytime = timeline->getClipPlaytime(clipId);
    if (position <= clipStart || position >= clipStart + clipPlaytime) {
        outError = QStringLiteral("Cut position %1 must be strictly inside clip [%2, %3)")
                       .arg(position)
                       .arg(clipStart)
                       .arg(clipStart + clipPlaytime);
        return false;
    }

    const int trackId = timeline->getClipTrackId(clipId);

    Fun localUndo = []() { return true; };
    Fun localRedo = []() { return true; };

    const bool ok = TimelineFunctions::requestClipCut(timeline, clipId, position, localUndo, localRedo);
    if (!ok) {
        outError = QStringLiteral("TimelineFunctions::requestClipCut failed for clipId=%1 at position=%2")
                       .arg(clipId)
                       .arg(position);
        return false;
    }

    const int rightClipId = trackId >= 0 ? timeline->getClipByStartPosition(trackId, position) : -1;
    if (rightClipId >= 0 && rightClipId != clipId) {
        const QString rightUid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        setClipCustomProperty(timeline, rightClipId, "meditor:uid", rightUid);
        setClipCustomProperty(timeline, rightClipId, "kdenlive:meditor_uid", rightUid);
    }

    finalizeOperationUndo(localUndo, localRedo, QStringLiteral("Cut clip"));

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("clipId"), clipId);
    outResult.insert(QStringLiteral("leftClipId"), clipId);
    outResult.insert(QStringLiteral("rightClipId"), rightClipId);
    outResult.insert(QStringLiteral("cutPosition"), position);
    outResult.insert(QStringLiteral("leftClip"), JsonSerialize::serializeClip(timeline, clipId, fps));
    if (rightClipId >= 0) {
        outResult.insert(QStringLiteral("rightClip"), JsonSerialize::serializeClip(timeline, rightClipId, fps));
    }
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleClipResize(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    auto timeline = activeTimeline();
    if (!timeline) {
        outError = QStringLiteral("No active timeline");
        return false;
    }

    const int clipId = params.value(QStringLiteral("clipId")).toInt(params.value(QStringLiteral("id")).toInt(-1));
    if (clipId < 0 || !timeline->isClip(clipId)) {
        outError = QStringLiteral("Invalid or unknown clipId: %1").arg(clipId);
        return false;
    }

    const double fps = activeFps();
    const std::pair<int, int> currentInOut = timeline->getClipInOut(clipId);
    const int currentPlaytime = timeline->getClipPlaytime(clipId);

    Fun localUndo = []() { return true; };
    Fun localRedo = []() { return true; };

    bool resizedAny = false;

    // Modo 1: `in` e/ou `out` explícitos
    const int requestedIn = parseFrameParam(params, QStringLiteral("in"), QStringLiteral("inSeconds"), fps, -1);
    const int requestedOut = parseFrameParam(params, QStringLiteral("out"), QStringLiteral("outSeconds"), fps, -1);

    if (requestedIn >= 0 || requestedOut >= 0) {
        if (requestedIn >= 0 && requestedIn != currentInOut.first) {
            const int deltaIn = requestedIn - currentInOut.first;
            int targetSize = currentPlaytime - deltaIn;
            if (targetSize <= 0) {
                outError = QStringLiteral("Invalid 'in' frame %1 resulting in non-positive duration %2")
                               .arg(requestedIn)
                               .arg(targetSize);
                return false;
            }
            if (!timeline->requestItemResize(clipId, targetSize, /*right=*/false, /*logUndo=*/false, localUndo,
                                             localRedo, /*blockUndo=*/false)) {
                outError = QStringLiteral("requestItemResize (left/in) failed for clipId=%1").arg(clipId);
                return false;
            }
            resizedAny = true;
        }
        if (requestedOut >= 0) {
            const std::pair<int, int> updatedInOut = timeline->getClipInOut(clipId);
            const int updatedPlaytime = timeline->getClipPlaytime(clipId);
            if (requestedOut != updatedInOut.second) {
                const int deltaOut = requestedOut - updatedInOut.second;
                int targetSize = updatedPlaytime + deltaOut;
                if (targetSize <= 0) {
                    outError = QStringLiteral("Invalid 'out' frame %1 resulting in non-positive duration %2")
                                   .arg(requestedOut)
                                   .arg(targetSize);
                    return false;
                }
                if (!timeline->requestItemResize(clipId, targetSize, /*right=*/true, /*logUndo=*/false, localUndo,
                                                 localRedo, /*blockUndo=*/false)) {
                    outError = QStringLiteral("requestItemResize (right/out) failed for clipId=%1").arg(clipId);
                    return false;
                }
                resizedAny = true;
            }
        }
    } else {
        // Modo 2: `size` / `duration` / `playtime` + `right` (ou `edge: "right"|"left"`)
        int targetSize = parseFrameParam(params, QStringLiteral("size"), QStringLiteral("durationSeconds"), fps, -1);
        if (targetSize < 0) {
            targetSize = params.value(QStringLiteral("duration"))
                             .toInt(params.value(QStringLiteral("playtime")).toInt(-1));
        }
        if (targetSize <= 0) {
            outError = QStringLiteral("clip.resize requires positive 'size'/'duration' or 'in'/'out'");
            return false;
        }

        bool rightEdge = params.value(QStringLiteral("right")).toBool(true);
        if (params.contains(QStringLiteral("edge"))) {
            const QString edgeStr = params.value(QStringLiteral("edge")).toString().trimmed().toLower();
            if (edgeStr == QLatin1String("left") || edgeStr == QLatin1String("in") ||
                edgeStr == QLatin1String("start")) {
                rightEdge = false;
            } else if (edgeStr == QLatin1String("right") || edgeStr == QLatin1String("out") ||
                       edgeStr == QLatin1String("end")) {
                rightEdge = true;
            }
        }

        if (!timeline->requestItemResize(clipId, targetSize, rightEdge, /*logUndo=*/false, localUndo, localRedo,
                                         /*blockUndo=*/false)) {
            outError = QStringLiteral("TimelineModel::requestItemResize failed for clipId=%1 (size=%2, right=%3)")
                           .arg(clipId)
                           .arg(targetSize)
                           .arg(rightEdge);
            return false;
        }
        resizedAny = true;
    }

    if (resizedAny) {
        finalizeOperationUndo(localUndo, localRedo, QStringLiteral("Resize clip"));
    }

    outResult = JsonSerialize::serializeClip(timeline, clipId, fps);
    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleClipDelete(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    auto timeline = activeTimeline();
    if (!timeline) {
        outError = QStringLiteral("No active timeline");
        return false;
    }

    const int clipId = params.value(QStringLiteral("clipId")).toInt(params.value(QStringLiteral("id")).toInt(-1));
    if (clipId < 0 || !timeline->isClip(clipId)) {
        outError = QStringLiteral("Invalid or unknown clipId: %1").arg(clipId);
        return false;
    }

    Fun localUndo = []() { return true; };
    Fun localRedo = []() { return true; };

    const bool deleted = timeline->requestItemDeletion(clipId, localUndo, localRedo, /*logUndo=*/false);
    if (!deleted) {
        outError = QStringLiteral("TimelineModel::requestItemDeletion failed for clipId=%1").arg(clipId);
        return false;
    }

    finalizeOperationUndo(localUndo, localRedo, QStringLiteral("Delete clip"));

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("deletedClipId"), clipId);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleClipMove(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    auto timeline = activeTimeline();
    if (!timeline) {
        outError = QStringLiteral("No active timeline");
        return false;
    }

    const int clipId = params.value(QStringLiteral("clipId")).toInt(params.value(QStringLiteral("id")).toInt(-1));
    if (clipId < 0 || !timeline->isClip(clipId)) {
        outError = QStringLiteral("Invalid or unknown clipId: %1").arg(clipId);
        return false;
    }

    const int currentTrackId = timeline->getClipTrackId(clipId);
    const bool isAudio = currentTrackId >= 0 ? timeline->isAudioTrack(currentTrackId) : false;

    int targetTrackId = currentTrackId;
    if (params.contains(QStringLiteral("trackId")) || params.contains(QStringLiteral("track"))) {
        const QJsonValue trackVal = params.contains(QStringLiteral("trackId")) ? params.value(QStringLiteral("trackId"))
                                                                               : params.value(QStringLiteral("track"));
        const int resolved = JsonSerialize::resolveTrackId(timeline, trackVal, isAudio);
        if (resolved >= 0) {
            targetTrackId = resolved;
        }
    }

    const double fps = activeFps();
    int position = parseFrameParam(params, QStringLiteral("position"), QStringLiteral("positionSeconds"), fps, -1);
    if (position < 0) {
        position = parseFrameParam(params, QStringLiteral("pos"), QStringLiteral("posSeconds"), fps, -1);
    }
    if (position < 0) {
        outError = QStringLiteral("clip.move requires 'position' or 'pos'");
        return false;
    }

    const bool moveMirrorTracks = params.value(QStringLiteral("moveMirrorTracks")).toBool(true);

    Fun localUndo = []() { return true; };
    Fun localRedo = []() { return true; };

    const TimelineModel::MoveResult moveRes =
        timeline->requestClipMove(clipId, targetTrackId, position, moveMirrorTracks, /*updateView=*/true,
                                  /*invalidateTimeline=*/true, /*finalMove=*/true, localUndo, localRedo);
    if (moveRes != TimelineModel::MoveSuccess) {
        outError = QStringLiteral("TimelineModel::requestClipMove failed for clipId=%1 to trackId=%2 pos=%3 (code=%4)")
                       .arg(clipId)
                       .arg(targetTrackId)
                       .arg(position)
                       .arg(int(moveRes));
        return false;
    }

    finalizeOperationUndo(localUndo, localRedo, QStringLiteral("Move clip"));

    outResult = JsonSerialize::serializeClip(timeline, clipId, fps);
    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleClipSpeed(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    auto timeline = activeTimeline();
    if (!timeline) {
        outError = QStringLiteral("No active timeline");
        return false;
    }

    const int clipId = params.value(QStringLiteral("clipId")).toInt(params.value(QStringLiteral("id")).toInt(-1));
    if (clipId < 0 || !timeline->isClip(clipId)) {
        outError = QStringLiteral("Invalid or unknown clipId: %1").arg(clipId);
        return false;
    }

    double speedRatio = 1.0;
    if (params.contains(QStringLiteral("speedPercent"))) {
        speedRatio = params.value(QStringLiteral("speedPercent")).toDouble(100.0) / 100.0;
    } else {
        const double rawSpeed = params.value(QStringLiteral("speed")).toDouble(1.0);
        const bool isPercent = params.value(QStringLiteral("isPercent")).toBool(false);
        if (isPercent || std::abs(rawSpeed) > 10.0) {
            speedRatio = rawSpeed / 100.0;
        } else {
            speedRatio = rawSpeed;
        }
    }

    if (std::abs(speedRatio) < 0.01) {
        outError = QStringLiteral("Invalid clip speed ratio: %1").arg(speedRatio);
        return false;
    }

    const bool pitchCompensate = params.value(QStringLiteral("pitchCompensate")).toBool(true);
    const bool changeDuration = params.value(QStringLiteral("changeDuration")).toBool(true);

    Fun localUndo = []() { return true; };
    Fun localRedo = []() { return true; };

    bool ok = true;
    const int splitId = timeline->getClipSplitPartner(clipId);
    if (splitId > -1) {
        ok = timeline->requestClipTimeWarp(splitId, speedRatio, pitchCompensate, changeDuration, localUndo, localRedo);
    }
    if (ok) {
        ok = timeline->requestClipTimeWarp(clipId, speedRatio, pitchCompensate, changeDuration, localUndo, localRedo);
    }

    if (!ok) {
        localUndo();
        outError = QStringLiteral("TimelineModel::requestClipTimeWarp failed for clipId=%1 (speedRatio=%2)")
                       .arg(clipId)
                       .arg(speedRatio);
        return false;
    }

    finalizeOperationUndo(localUndo, localRedo, QStringLiteral("Change clip speed"));

    const double fps = activeFps();
    outResult = JsonSerialize::serializeClip(timeline, clipId, fps);
    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("speedRatio"), speedRatio);
    outResult.insert(QStringLiteral("speedPercent"), speedRatio * 100.0);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleMarkerAdd(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    auto timeline = activeTimeline();
    KdenliveDoc *doc = pCore->currentDoc();

    std::shared_ptr<MarkerListModel> guideModel;
    if (timeline) {
        guideModel = timeline->getGuideModel();
    }
    if (!guideModel && doc) {
        guideModel = doc->getGuideModel(doc->uuid());
    }
    if (!guideModel && pCore->projectManager()) {
        guideModel = pCore->projectManager()->getGuideModel();
    }
    if (!guideModel) {
        outError = QStringLiteral("MarkerListModel (guides) is not available");
        return false;
    }

    const double fps = activeFps();
    int frame = parseFrameParam(params, QStringLiteral("frame"), QStringLiteral("seconds"), fps, -1);
    if (frame < 0) {
        frame = parseFrameParam(params, QStringLiteral("position"), QStringLiteral("positionSeconds"), fps, -1);
    }
    if (frame < 0) {
        frame = parseFrameParam(params, QStringLiteral("pos"), QStringLiteral("posSeconds"), fps, 0);
    }

    const QString comment = params.value(QStringLiteral("comment"))
                                .toString(params.value(QStringLiteral("text")).toString(QStringLiteral("Marker")));
    const int markerType = params.value(QStringLiteral("type")).toInt(-1);
    const GenTime posTime(frame, fps);

    Fun localUndo = []() { return true; };
    Fun localRedo = []() { return true; };

    QJsonObject singleMarker;
    singleMarker.insert(QStringLiteral("pos"), posTime.seconds());
    singleMarker.insert(QStringLiteral("comment"), comment);
    if (markerType >= 0) {
        singleMarker.insert(QStringLiteral("type"), markerType);
    }
    QJsonArray markerArr;
    markerArr.append(singleMarker);
    const QString jsonStr = QString::fromUtf8(QJsonDocument(markerArr).toJson(QJsonDocument::Compact));

    bool added = guideModel->importFromJson(jsonStr, /*ignoreConflicts=*/true, localUndo, localRedo);
    if (added) {
        finalizeOperationUndo(localUndo, localRedo, QStringLiteral("Add guide"));
    } else if (!m_inBatch) {
        added = guideModel->addMarker(posTime, comment, markerType);
    }

    if (!added) {
        outError = QStringLiteral("MarkerListModel failed to add marker at frame %1").arg(frame);
        return false;
    }

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("frame"), frame);
    outResult.insert(QStringLiteral("pos"), frame);
    outResult.insert(QStringLiteral("seconds"), posTime.seconds());
    outResult.insert(QStringLiteral("comment"), comment);
    outResult.insert(QStringLiteral("text"), comment);
    outResult.insert(QStringLiteral("type"), markerType);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleSubtitleAdd(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    auto timeline = activeTimeline();
    if (!timeline) {
        outError = QStringLiteral("No active timeline");
        return false;
    }

    std::shared_ptr<SubtitleModel> subModel = timeline->getSubtitleModel();
    if (!subModel) {
        timeline->createSubtitleModel();
        subModel = timeline->getSubtitleModel();
    }
    if (!subModel) {
        outError = QStringLiteral("SubtitleModel could not be initialized");
        return false;
    }

    const double fps = activeFps();
    int startFrame = parseFrameParam(params, QStringLiteral("startFrame"), QStringLiteral("startSeconds"), fps, -1);
    if (startFrame < 0) {
        startFrame = parseFrameParam(params, QStringLiteral("in"), QStringLiteral("inSeconds"), fps, -1);
    }
    if (startFrame < 0) {
        startFrame = parseFrameParam(params, QStringLiteral("position"), QStringLiteral("positionSeconds"), fps, 0);
    }

    int endFrame = parseFrameParam(params, QStringLiteral("endFrame"), QStringLiteral("endSeconds"), fps, -1);
    if (endFrame < 0) {
        endFrame = parseFrameParam(params, QStringLiteral("out"), QStringLiteral("outSeconds"), fps, -1);
    }
    if (endFrame <= startFrame) {
        const int durationFrames =
            parseFrameParam(params, QStringLiteral("durationFrames"), QStringLiteral("durationSeconds"), fps,
                            int(std::llround(2.0 * fps)));
        endFrame = startFrame + std::max(1, durationFrames);
    }

    const int layer = params.value(QStringLiteral("layer")).toInt(0);
    const QString text = params.value(QStringLiteral("text")).toString();
    if (text.isEmpty()) {
        outError = QStringLiteral("subtitle.add requires non-empty 'text'");
        return false;
    }

    const GenTime startTime(startFrame, fps);
    const GenTime endTime(endFrame, fps);
    const QString styleName = params.value(QStringLiteral("style")).toString(QStringLiteral("Default"));
    const SubtitleEvent event(true, endTime, styleName, QString(), 0, 0, 0, QString(), text);

    Fun localUndo = []() { return true; };
    Fun localRedo = []() { return true; };

    const bool added = subModel->addSubtitle({layer, startTime}, event, localUndo, localRedo, /*updateFilter=*/true);
    if (!added) {
        outError = QStringLiteral("SubtitleModel::addSubtitle failed at [%1, %2)").arg(startFrame).arg(endFrame);
        return false;
    }

    finalizeOperationUndo(localUndo, localRedo, QStringLiteral("Add subtitle"));

    const int subId = subModel->getIdForStartPos(layer, startTime);
    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("id"), subId);
    outResult.insert(QStringLiteral("layer"), layer);
    outResult.insert(QStringLiteral("startFrame"), startFrame);
    outResult.insert(QStringLiteral("endFrame"), endFrame);
    outResult.insert(QStringLiteral("in"), startFrame);
    outResult.insert(QStringLiteral("out"), endFrame);
    outResult.insert(QStringLiteral("startSeconds"), startTime.seconds());
    outResult.insert(QStringLiteral("endSeconds"), endTime.seconds());
    outResult.insert(QStringLiteral("text"), text);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleUiStatus(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    const QString message =
        params.value(QStringLiteral("message")).toString(params.value(QStringLiteral("text")).toString()).trimmed();
    if (message.isEmpty()) {
        outError = QStringLiteral("ui.status requires 'message' or 'text'");
        return false;
    }

    const int timeoutMs = params.value(QStringLiteral("timeout")).toInt(5000);
    pCore->displayMessage(message, InformationMessage, timeoutMs);

    if (MainWindow *win = pCore->window()) {
        if (QStatusBar *sb = win->statusBar()) {
            sb->showMessage(message, timeoutMs);
        }
    }

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("message"), message);
    return true;
}

bool Handlers::handlePlayheadSeek(const QJsonObject &params, QJsonObject &outResult, QString &outError)
{
    const double fps = activeFps();
    int frame = parseFrameParam(params, QStringLiteral("frame"), QStringLiteral("seconds"), fps, -1);
    if (frame < 0) {
        frame = parseFrameParam(params, QStringLiteral("position"), QStringLiteral("positionSeconds"), fps, -1);
    }
    if (frame < 0) {
        frame = parseFrameParam(params, QStringLiteral("pos"), QStringLiteral("posSeconds"), fps, -1);
    }
    if (frame < 0) {
        outError = QStringLiteral("playhead.seek requires 'frame', 'position', or 'seconds'");
        return false;
    }

    pCore->seekMonitor(Kdenlive::ProjectMonitor, frame);
    if (MainWindow *win = pCore->window()) {
        if (TimelineWidget *tlWidget = win->getCurrentTimeline()) {
            if (TimelineController *ctrl = tlWidget->controller()) {
                ctrl->setPosition(frame);
            }
        }
    }

    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("frame"), frame);
    outResult.insert(QStringLiteral("position"), frame);
    outResult.insert(QStringLiteral("seconds"), fps > 0.0 ? double(frame) / fps : 0.0);
    return true;
}

bool Handlers::handleUndo(const QJsonObject &, QJsonObject &outResult, QString &outError)
{
    auto stack = pCore->undoStack();
    if (!stack || !stack->canUndo()) {
        outError = QStringLiteral("Nothing to undo");
        return false;
    }
    stack->undo();
    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

bool Handlers::handleRedo(const QJsonObject &, QJsonObject &outResult, QString &outError)
{
    auto stack = pCore->undoStack();
    if (!stack || !stack->canRedo()) {
        outError = QStringLiteral("Nothing to redo");
        return false;
    }
    stack->redo();
    outResult.insert(QStringLiteral("ok"), true);
    outResult.insert(QStringLiteral("rev"), pCore->undoIndex());
    return true;
}

} // namespace MEditor
