/*
    SPDX-FileCopyrightText: 2026 MEditor Contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QUuid>
#include <QVector>
#include <memory>

class DocUndoStack;
class KdenliveDoc;
class MarkerListModel;
class MEditorApiServer;
class QUndoGroup;
class QUndoStack;
class SubtitleModel;
class TimelineItemModel;

/**
 * @class EventBridge
 * @brief Bridges Kdenlive document/model/undo signals into JSON-RPC 2.0 notifications.
 *
 * Connects to:
 *  - DocUndoStack (QUndoStack::indexChanged(int), DocUndoStack::invalidate(int))
 *  - ProjectManager (docOpened / active document & timeline transitions)
 *  - TimelineItemModel (rowsInserted, rowsRemoved, dataChanged, invalidateZone, durationUpdated, clipTimeWarpChanged, subtitleModelInitialized)
 *  - MarkerListModel (modelChanged, rowsInserted, rowsRemoved, dataChanged)
 *  - SubtitleModel (modelChanged, rowsInserted, rowsRemoved, dataChanged)
 *
 * Maintains an incremental revision counter (m_rev) on every timeline/document mutation
 * and a monotonic event sequence counter (m_seq) for JSON-RPC "event" notifications.
 */
class EventBridge : public QObject
{
    Q_OBJECT

public:
    explicit EventBridge(MEditorApiServer *apiServer, QObject *parent = nullptr);
    ~EventBridge() override;

    /** @brief Connect to Core, MainWindow, and ProjectManager signals; bind current doc if already open. */
    void attachToCore();

    /** @brief Rebind undo stack, active TimelineItemModel, MarkerListModel, and SubtitleModel for @p doc. */
    void bindDocument(KdenliveDoc *doc, bool emitLoadedEvent = false);

    /** @brief Rebind the active timeline and its associated guide/subtitle models. */
    void rebindActiveTimeline();

    /** @brief Monotonically increasing revision number bumped on any timeline/model/undo change. */
    qint64 currentRev() const { return m_rev; }

    /** @brief Increment revision counter and return the new value. */
    qint64 bumpRev(const QString &reason = QString());

    /** @brief Monotonically increasing notification sequence counter. */
    qint64 currentSeq() const { return m_seq; }

    /** @brief Emit a structured JSON-RPC 2.0 notification {"jsonrpc":"2.0","method":"event","params":{...}}. */
    void emitEvent(const QString &type, const QJsonObject &extraParams = QJsonObject());

public Q_SLOTS:
    /** @brief Slot invoked when ProjectManager finishes opening or creating a document. */
    void onDocOpened(KdenliveDoc *document);

    /** @brief Slot invoked when QUndoGroup switches active stack (new/opened project). */
    void onActiveUndoStackChanged(QUndoStack *stack);

    /** @brief Slot invoked when DocUndoStack index changes (push, undo, redo). */
    void onUndoIndexChanged(int idx);

    /** @brief Slot invoked when DocUndoStack invalidates future redo history. */
    void onUndoInvalidated(int idx);

private:
    void bindUndoStack(const std::shared_ptr<DocUndoStack> &stack);
    void bindTimelineModel(const std::shared_ptr<TimelineItemModel> &timeline);
    void bindGuideModel(const std::shared_ptr<MarkerListModel> &guideModel);
    void bindSubtitleModel(const std::shared_ptr<SubtitleModel> &subtitleModel);
    void clearModelConnections();

    static QString stripTimePrefix(const QString &commandText);

    MEditorApiServer *m_apiServer{nullptr};
    bool m_coreAttached{false};

    qint64 m_seq{0};
    qint64 m_rev{0};

    QPointer<KdenliveDoc> m_boundDoc;
    QUuid m_boundTimelineUuid;
    QString m_lastEmittedProjectKey;

    std::weak_ptr<DocUndoStack> m_undoStack;
    std::weak_ptr<TimelineItemModel> m_timelineModel;
    std::weak_ptr<MarkerListModel> m_guideModel;
    std::weak_ptr<SubtitleModel> m_subtitleModel;

    int m_lastUndoIndex{0};
    QPair<int, int> m_lastInvalidatedRange{-1, -1};

    QVector<QMetaObject::Connection> m_undoConnections;
    QVector<QMetaObject::Connection> m_timelineConnections;
    QVector<QMetaObject::Connection> m_guideConnections;
    QVector<QMetaObject::Connection> m_subtitleConnections;
};

namespace MEditor {
using EventBridge = ::EventBridge;
} // namespace MEditor

