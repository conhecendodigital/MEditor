/*
    SPDX-FileCopyrightText: 2026 MEditor Contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "eventbridge.h"
#include "apiserver.h"

#include "bin/model/markerlistmodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "mainwindow.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QAbstractItemModel>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoGroup>
#include <QUndoStack>

EventBridge::EventBridge(MEditorApiServer *apiServer, QObject *parent)
    : QObject(parent)
    , m_apiServer(apiServer)
{
}

EventBridge::~EventBridge()
{
    clearModelConnections();
    for (const auto &conn : std::as_const(m_undoConnections)) {
        QObject::disconnect(conn);
    }
    m_undoConnections.clear();
}

void EventBridge::attachToCore()
{
    if (!pCore) {
        return;
    }

    if (!m_coreAttached) {
        m_coreAttached = true;

        // When GUI setup finishes or splash closes, ensure we are bound to the active document & QUndoGroup
        connect(pCore.get(), &Core::GUISetupDone, this, [this]() {
            QTimer::singleShot(0, this, [this]() {
                attachToCore();
                if (pCore && pCore->currentDoc()) {
                    bindDocument(pCore->currentDoc(), true);
                }
            });
        });

        connect(pCore.get(), &Core::closeSplash, this, [this]() {
            QTimer::singleShot(0, this, [this]() {
                if (pCore && pCore->currentDoc()) {
                    bindDocument(pCore->currentDoc(), true);
                }
            });
        });

        connect(pCore.get(), &Core::loadingMessageHide, this, [this]() {
            QTimer::singleShot(0, this, [this]() {
                if (pCore && pCore->currentDoc() && !pCore->currentDoc()->loading) {
                    bindDocument(pCore->currentDoc(), true);
                }
            });
        });
    }

    // Connect to ProjectManager::docOpened(KdenliveDoc*) if present (added in core_integration.patch)
    if (ProjectManager *pm = pCore->projectManager()) {
        const int sigIdx = pm->metaObject()->indexOfSignal("docOpened(KdenliveDoc*)");
        if (sigIdx >= 0) {
            QObject::connect(pm, SIGNAL(docOpened(KdenliveDoc*)), this, SLOT(onDocOpened(KdenliveDoc*)), Qt::UniqueConnection);
        }
    }

    // Connect to MainWindow's QUndoGroup::activeStackChanged, which fires on every MainWindow::connectDocument()
    if (MainWindow *mw = pCore->window()) {
        if (mw->m_commandStack) {
            connect(mw->m_commandStack, &QUndoGroup::activeStackChanged, this, &EventBridge::onActiveUndoStackChanged, Qt::UniqueConnection);
        }
    }

    if (pCore->currentDoc() && !pCore->currentDoc()->loading) {
        bindDocument(pCore->currentDoc(), false);
    }
}

void EventBridge::onDocOpened(KdenliveDoc *document)
{
    KdenliveDoc *doc = document ? document : (pCore ? pCore->currentDoc() : nullptr);
    if (!doc) {
        return;
    }
    // Defer by one event-loop tick if the document is still finalizing its active timeline
    if (doc->loading) {
        QPointer<KdenliveDoc> guardedDoc(doc);
        QTimer::singleShot(0, this, [this, guardedDoc]() {
            if (guardedDoc) {
                bindDocument(guardedDoc.data(), true);
            }
        });
        return;
    }
    bindDocument(doc, true);
}

void EventBridge::onActiveUndoStackChanged(QUndoStack *stack)
{
    Q_UNUSED(stack);
    QTimer::singleShot(0, this, [this]() {
        if (pCore && pCore->currentDoc()) {
            bindDocument(pCore->currentDoc(), true);
        }
    });
}

void EventBridge::bindDocument(KdenliveDoc *doc, bool emitLoadedEvent)
{
    if (!doc || doc->closing) {
        return;
    }

    const QUuid activeUuid = doc->activeUuid.isNull() ? doc->uuid() : doc->activeUuid;
    const bool docChanged = (m_boundDoc != doc);
    const bool timelineChanged = (m_boundTimelineUuid != activeUuid);

    if (docChanged) {
        m_boundDoc = doc;
        m_lastInvalidatedRange = {-1, -1};
        bindUndoStack(doc->commandStack());
    }

    if (docChanged || timelineChanged || m_timelineModel.expired()) {
        rebindActiveTimeline();
    }

    if (emitLoadedEvent) {
        const QString projectPath = doc->url().isLocalFile() ? doc->url().toLocalFile() : doc->url().toString();
        const QString projectKey = QStringLiteral("%1|%2|%3")
                                       .arg(reinterpret_cast<quintptr>(doc), 0, 16)
                                       .arg(activeUuid.toString(), projectPath);
        if (m_lastEmittedProjectKey != projectKey) {
            m_lastEmittedProjectKey = projectKey;
            bumpRev(QStringLiteral("project.loaded"));

            QJsonObject payload;
            payload.insert(QStringLiteral("path"), projectPath);
            payload.insert(QStringLiteral("uuid"), activeUuid.toString(QUuid::WithoutBraces));
            payload.insert(QStringLiteral("fps"), doc->timecode().fps());
            payload.insert(QStringLiteral("width"), doc->width());
            payload.insert(QStringLiteral("height"), doc->height());
            payload.insert(QStringLiteral("undoIndex"), m_lastUndoIndex);
            emitEvent(QStringLiteral("project.loaded"), payload);
        }
    }
}

void EventBridge::rebindActiveTimeline()
{
    if (!pCore || !pCore->currentDoc()) {
        clearModelConnections();
        return;
    }

    KdenliveDoc *doc = pCore->currentDoc();
    const QUuid activeUuid = doc->activeUuid.isNull() ? doc->uuid() : doc->activeUuid;
    m_boundTimelineUuid = activeUuid;

    std::shared_ptr<TimelineItemModel> timeline;
    if (pCore->projectManager()) {
        timeline = pCore->projectManager()->getTimeline();
    }
    if (!timeline) {
        timeline = doc->getTimeline(activeUuid, true);
    }

    bindTimelineModel(timeline);
    bindGuideModel(doc->getGuideModel(activeUuid));
    if (timeline) {
        bindSubtitleModel(timeline->getSubtitleModel());
    } else {
        bindSubtitleModel(nullptr);
    }
}

void EventBridge::bindUndoStack(const std::shared_ptr<DocUndoStack> &stack)
{
    for (const auto &conn : std::as_const(m_undoConnections)) {
        QObject::disconnect(conn);
    }
    m_undoConnections.clear();
    m_undoStack = stack;

    if (!stack) {
        m_lastUndoIndex = 0;
        return;
    }

    m_lastUndoIndex = stack->index();
    m_undoConnections.push_back(connect(stack.get(), &QUndoStack::indexChanged, this, &EventBridge::onUndoIndexChanged));
    m_undoConnections.push_back(connect(stack.get(), &DocUndoStack::invalidate, this, &EventBridge::onUndoInvalidated));
}

void EventBridge::bindTimelineModel(const std::shared_ptr<TimelineItemModel> &timeline)
{
    for (const auto &conn : std::as_const(m_timelineConnections)) {
        QObject::disconnect(conn);
    }
    m_timelineConnections.clear();
    m_timelineModel = timeline;

    if (!timeline) {
        return;
    }

    TimelineItemModel *tl = timeline.get();

    m_timelineConnections.push_back(connect(tl, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex &, int, int) {
        bumpRev(QStringLiteral("timeline.rowsInserted"));
    }));
    m_timelineConnections.push_back(connect(tl, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int, int) {
        bumpRev(QStringLiteral("timeline.rowsRemoved"));
    }));
    m_timelineConnections.push_back(connect(tl, &QAbstractItemModel::dataChanged, this,
                                            [this](const QModelIndex &, const QModelIndex &, const QVector<int> &) {
                                                bumpRev(QStringLiteral("timeline.dataChanged"));
                                            }));
    m_timelineConnections.push_back(connect(tl, &QAbstractItemModel::modelReset, this, [this]() {
        bumpRev(QStringLiteral("timeline.modelReset"));
    }));
    m_timelineConnections.push_back(connect(tl, &TimelineModel::invalidateZone, this, [this](int in, int out, bool) {
        if (in >= 0 && out >= in) {
            m_lastInvalidatedRange = {in, out};
        }
        bumpRev(QStringLiteral("timeline.invalidateZone"));
    }));
    m_timelineConnections.push_back(connect(tl, &TimelineModel::durationUpdated, this, [this](const QUuid &) {
        bumpRev(QStringLiteral("timeline.durationUpdated"));
    }));
    m_timelineConnections.push_back(connect(tl, &TimelineModel::clipTimeWarpChanged, this, [this](int) {
        bumpRev(QStringLiteral("timeline.clipTimeWarpChanged"));
    }));
    m_timelineConnections.push_back(connect(tl, &TimelineItemModel::subtitleModelInitialized, this, [this]() {
        if (auto lockedTl = m_timelineModel.lock()) {
            bindSubtitleModel(lockedTl->getSubtitleModel());
        }
    }));
}

void EventBridge::bindGuideModel(const std::shared_ptr<MarkerListModel> &guideModel)
{
    for (const auto &conn : std::as_const(m_guideConnections)) {
        QObject::disconnect(conn);
    }
    m_guideConnections.clear();
    m_guideModel = guideModel;

    if (!guideModel) {
        return;
    }

    MarkerListModel *gm = guideModel.get();
    m_guideConnections.push_back(connect(gm, &MarkerListModel::modelChanged, this, [this]() {
        bumpRev(QStringLiteral("guides.modelChanged"));
    }));
    m_guideConnections.push_back(connect(gm, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex &, int, int) {
        bumpRev(QStringLiteral("guides.rowsInserted"));
    }));
    m_guideConnections.push_back(connect(gm, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int, int) {
        bumpRev(QStringLiteral("guides.rowsRemoved"));
    }));
    m_guideConnections.push_back(connect(gm, &QAbstractItemModel::dataChanged, this,
                                         [this](const QModelIndex &, const QModelIndex &, const QVector<int> &) {
                                             bumpRev(QStringLiteral("guides.dataChanged"));
                                         }));
}

void EventBridge::bindSubtitleModel(const std::shared_ptr<SubtitleModel> &subtitleModel)
{
    for (const auto &conn : std::as_const(m_subtitleConnections)) {
        QObject::disconnect(conn);
    }
    m_subtitleConnections.clear();
    m_subtitleModel = subtitleModel;

    if (!subtitleModel) {
        return;
    }

    SubtitleModel *sm = subtitleModel.get();
    m_subtitleConnections.push_back(connect(sm, &SubtitleModel::modelChanged, this, [this]() {
        bumpRev(QStringLiteral("subtitles.modelChanged"));
    }));
    m_subtitleConnections.push_back(connect(sm, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex &, int, int) {
        bumpRev(QStringLiteral("subtitles.rowsInserted"));
    }));
    m_subtitleConnections.push_back(connect(sm, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int, int) {
        bumpRev(QStringLiteral("subtitles.rowsRemoved"));
    }));
    m_subtitleConnections.push_back(connect(sm, &QAbstractItemModel::dataChanged, this,
                                            [this](const QModelIndex &, const QModelIndex &, const QVector<int> &) {
                                                bumpRev(QStringLiteral("subtitles.dataChanged"));
                                            }));
}

void EventBridge::clearModelConnections()
{
    for (const auto &conn : std::as_const(m_timelineConnections)) {
        QObject::disconnect(conn);
    }
    m_timelineConnections.clear();

    for (const auto &conn : std::as_const(m_guideConnections)) {
        QObject::disconnect(conn);
    }
    m_guideConnections.clear();

    for (const auto &conn : std::as_const(m_subtitleConnections)) {
        QObject::disconnect(conn);
    }
    m_subtitleConnections.clear();

    m_timelineModel.reset();
    m_guideModel.reset();
    m_subtitleModel.reset();
}

qint64 EventBridge::bumpRev(const QString &reason)
{
    Q_UNUSED(reason);
    return ++m_rev;
}

QString EventBridge::stripTimePrefix(const QString &commandText)
{
    // FunctionalUndoCommand in src/undohelper.cpp formats text as "hh:mm <label>"
    static const QRegularExpression kTimePrefixRe(QStringLiteral("^\\d{2}:\\d{2}\\s+"));
    QString cleaned = commandText;
    cleaned.remove(kTimePrefixRe);
    return cleaned;
}

void EventBridge::onUndoIndexChanged(int idx)
{
    auto stack = m_undoStack.lock();
    if (!stack && pCore && pCore->projectManager()) {
        stack = pCore->projectManager()->undoStack();
    }

    QString rawText;
    QString action = QStringLiteral("push");
    if (stack) {
        if (idx < m_lastUndoIndex && idx >= 0 && idx < stack->count()) {
            // Undo step: command at idx was just undone
            if (const QUndoCommand *cmd = stack->command(idx)) {
                rawText = cmd->text();
            }
            action = QStringLiteral("undo");
        } else if (idx > 0 && (idx - 1) < stack->count()) {
            // Push or Redo step: command at idx - 1 is now at top of active stack
            if (const QUndoCommand *cmd = stack->command(idx - 1)) {
                rawText = cmd->text();
            }
            action = (idx == stack->count()) ? QStringLiteral("push") : QStringLiteral("redo");
        }
    }

    m_lastUndoIndex = idx;
    bumpRev(QStringLiteral("undo.indexChanged"));

    const QString cleanText = stripTimePrefix(rawText);
    const QString origin = (m_apiServer && m_apiServer->isInApiCall())
                               ? QStringLiteral("claude")
                               : QStringLiteral("user");

    QJsonObject params;
    params.insert(QStringLiteral("text"), cleanText);
    if (!rawText.isEmpty() && rawText != cleanText) {
        params.insert(QStringLiteral("rawText"), rawText);
    }
    params.insert(QStringLiteral("origin"), origin);
    params.insert(QStringLiteral("action"), action);
    params.insert(QStringLiteral("undoIndex"), idx);

    if (m_lastInvalidatedRange.first >= 0 && m_lastInvalidatedRange.second >= m_lastInvalidatedRange.first) {
        QJsonArray rangeArr;
        rangeArr.append(m_lastInvalidatedRange.first);
        rangeArr.append(m_lastInvalidatedRange.second);
        params.insert(QStringLiteral("range"), rangeArr);
        m_lastInvalidatedRange = {-1, -1};
    }

    emitEvent(QStringLiteral("undo.push"), params);
}

void EventBridge::onUndoInvalidated(int idx)
{
    Q_UNUSED(idx);
    bumpRev(QStringLiteral("undo.invalidate"));
}

void EventBridge::emitEvent(const QString &type, const QJsonObject &extraParams)
{
    if (!m_apiServer) {
        return;
    }

    if (type == QLatin1String("project.loaded") && pCore && pCore->currentDoc()) {
        KdenliveDoc *doc = pCore->currentDoc();
        bindDocument(doc, false);
        const QUuid activeUuid = doc->activeUuid.isNull() ? doc->uuid() : doc->activeUuid;
        const QString projectPath = doc->url().isLocalFile() ? doc->url().toLocalFile() : doc->url().toString();
        m_lastEmittedProjectKey = QStringLiteral("%1|%2|%3")
                                      .arg(reinterpret_cast<quintptr>(doc), 0, 16)
                                      .arg(activeUuid.toString(), projectPath);
        bumpRev(QStringLiteral("project.loaded"));
    }

    QJsonObject params = extraParams;
    params.insert(QStringLiteral("seq"), ++m_seq);
    params.insert(QStringLiteral("type"), type);
    params.insert(QStringLiteral("rev"), m_rev);
    if (!params.contains(QStringLiteral("origin"))) {
        params.insert(QStringLiteral("origin"),
                      m_apiServer->isInApiCall() ? QStringLiteral("claude") : QStringLiteral("user"));
    }

    m_apiServer->broadcastNotification(QStringLiteral("event"), params);
}
