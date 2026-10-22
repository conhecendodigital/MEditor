/*
    SPDX-FileCopyrightText: 2018 Jean-Baptiste Mardelle <jb@kdenlive.org>
    This file is part of Kdenlive. See www.kdenlive.org.

    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "monitorproxy.h"
#include "KLocalizedString"
#include "bin/bin.h"
#include "bin/projectclip.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "doc/kthumb.h"
#include "kdenlivesettings.h"
#include "markersortmodel.h"
#include "monitormanager.h"
#include "profiles/profilemodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "dialogs/subtitleedit.h"
#include "mainwindow.h"
#include "meditor/apiserver.h"
#include "meditor/handlers.h"
#include "monitor/monitor.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QFont>
#include <QFontDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>

#include <mlt++/MltConsumer.h>
#include <mlt++/MltFilter.h>
#include <mlt++/MltProducer.h>
#include <mlt++/MltProfile.h>

MonitorProxy::MonitorProxy(VideoWidget *parent)
    : QObject(parent)
    , q(parent)
    , m_position(-1)
    , m_zoneIn(0)
    , m_zoneOut(-1)
    , m_hasAV(false)
    , m_speed(0)
    , m_clipType(0)
    , m_clipId(-1)
    , m_seekFinished(true)
    , m_td(nullptr)
    , m_trimmingFrames1(0)
    , m_trimmingFrames2(0)
    , m_boundsCount(0)
{
    if (q->m_id == int(Kdenlive::ClipMonitor)) {
        connect(pCore->bin(), &Bin::clipNameChanged, this, &MonitorProxy::updateClipName);
    }
}

void MonitorProxy::switchGrid()
{
    KdenliveSettings::setShowMonitorGrid(!KdenliveSettings::showMonitorGrid());
}

int MonitorProxy::getPosition() const
{
    return m_position;
}

void MonitorProxy::updateClipName(int id, const QString newName)
{
    for (int i = 0; i < m_lastClipsIds.size(); i++) {
        if (m_lastClipsIds.at(i).first == id) {
            m_lastClipsIds.remove(i);
            m_lastClipsIds.insert(i, {id, newName});
            m_lastClips.clear();
            for (int j = 0; j < 4 && j < m_lastClipsIds.size(); j++) {
                m_lastClips << m_lastClipsIds.at(j).second;
            }
            m_clipName = newName;
            Q_EMIT clipNameChanged();
            Q_EMIT lastClipsChanged();
            break;
        }
    }
}

void MonitorProxy::resetPosition()
{
    m_position = -1;
}

int MonitorProxy::rulerHeight() const
{
    return q->m_rulerHeight;
}

void MonitorProxy::setRulerHeight(int addedHeight)
{
    q->updateRulerHeight(addedHeight);
}

void MonitorProxy::seek(int delta, uint modifiers)
{
    Q_EMIT q->mouseSeek(delta, modifiers);
}

QString MonitorProxy::thumbPath(int frame) const
{
    if (m_clipType == ClipType::AV || m_clipType == ClipType::Video || m_clipType == ClipType::SlideShow) {
        return QStringLiteral("image://thumbnail/%1%2").arg(pCore->bin()->getBinClip(QString::number(m_clipId))->baseThumbPath(), QString::number(frame));
    }
    return {};
}

int MonitorProxy::maskOpacity() const
{
    return KdenliveSettings::maskOpacity();
}

void MonitorProxy::setMaskOpacity(int opacity)
{
    KdenliveSettings::setMaskOpacity(qBound(0, opacity, 100));
    qDebug() << " = = = SETTING MASK OPACITY";
    Q_EMIT maskOpacityChanged();
    Q_EMIT refreshMask();
}

bool MonitorProxy::maskInverted() const
{
    return KdenliveSettings::maskInverted();
}

void MonitorProxy::setMaskInverted(bool)
{
    KdenliveSettings::setMaskInverted(!KdenliveSettings::maskInverted());
    if (m_maskMode == MaskModeType::MaskInput) {
        // We are in preview mode, update url
        QImage img(m_previewOverlay.toLocalFile());
        img.invertPixels(QImage::InvertRgba);
        img.save(m_previewOverlay.toLocalFile());
        m_previewOverlay.setQuery(QStringLiteral("pos=%1&ctrl=%2").arg(m_position).arg(QDateTime::currentSecsSinceEpoch()));
        Q_EMIT previewOverlayChanged();
    }
    Q_EMIT refreshMask();
}

void MonitorProxy::setMaskMode(MaskModeType::MaskCreationMode mode)
{
    m_maskMode = mode;
    Q_EMIT maskModeChanged();
}

MaskModeType::MaskCreationMode MonitorProxy::maskMode() const
{
    return m_maskMode;
}

void MonitorProxy::nextOverlay()
{
    if (overlayType() >= 5) {
        setOverlayType(0);
    } else {
        setOverlayType(overlayType() + 1);
    }
}

int MonitorProxy::overlayType() const
{
    return (q->m_id == int(Kdenlive::ClipMonitor) ? KdenliveSettings::clipMonitorOverlayGuides() : KdenliveSettings::projectMonitorOverlayGuides());
}

void MonitorProxy::setOverlayType(int ix)
{
    if (q->m_id == int(Kdenlive::ClipMonitor)) {
        KdenliveSettings::setClipMonitorOverlayGuides(ix);
    } else {
        KdenliveSettings::setProjectMonitorOverlayGuides(ix);
    }
    Q_EMIT overlayTypeChanged();
}

bool MonitorProxy::showSafezone() const
{
    return (q->m_id == int(Kdenlive::ClipMonitor) ? KdenliveSettings::showClipMonitorSafeZone() : KdenliveSettings::showProjectMonitorSafeZone());
}

void MonitorProxy::setShowSafezone(bool display)
{
    if (q->m_id == int(Kdenlive::ClipMonitor)) {
        KdenliveSettings::setShowClipMonitorSafeZone(display);
    } else {
        KdenliveSettings::setShowProjectMonitorSafeZone(display);
    }
    Q_EMIT showSafezoneChanged();
}

bool MonitorProxy::setPosition(int pos)
{
    return setPositionAdvanced(pos, false);
}

bool MonitorProxy::setPositionAdvanced(int pos, bool noAudioScrub)
{
    if (m_position == pos) {
        return true;
    }
    m_position = pos;
    Q_EMIT requestSeek(pos, noAudioScrub);
    if (m_seekFinished) {
        m_seekFinished = false;
        Q_EMIT seekFinishedChanged();
    }
    Q_EMIT positionChanged(pos);
    return false;
}

void MonitorProxy::setCursorPosition(int pos)
{
    if (m_position == pos) {
        return;
    }
    m_position = pos;
    Q_EMIT positionChanged(pos);
}

void MonitorProxy::positionFromConsumer(int pos, bool playing)
{
    if (playing) {
        m_position = pos;
        Q_EMIT positionChanged(pos);
        if (!m_seekFinished) {
            m_seekFinished = true;
            Q_EMIT seekFinishedChanged();
        }
    } else {
        if (!m_seekFinished && m_position == pos) {
            m_seekFinished = true;
            Q_EMIT seekFinishedChanged();
        }
    }
}

void MonitorProxy::setMarker(const QString &comment, const QColor &color)
{
    if (m_markerComment == comment && color == m_markerColor) {
        return;
    }
    m_markerComment = comment;
    m_markerColor = color;
    Q_EMIT markerChanged();
}

int MonitorProxy::zoneIn() const
{
    return m_zoneIn;
}

int MonitorProxy::zoneOut() const
{
    return m_zoneOut;
}

void MonitorProxy::setZoneIn(int pos)
{
    if (m_zoneIn > 0) {
        Q_EMIT removeSnap(m_zoneIn);
    }
    m_zoneIn = pos;
    if (pos > 0) {
        Q_EMIT addSnap(pos);
    }
    Q_EMIT zoneChanged();
    Q_EMIT saveZone(QPoint(m_zoneIn, m_zoneOut));
}

void MonitorProxy::setZoneOut(int pos)
{
    if (m_zoneOut > 0) {
        Q_EMIT removeSnap(m_zoneOut);
    }
    m_zoneOut = pos;
    if (pos > 0) {
        Q_EMIT addSnap(m_zoneOut);
    }
    Q_EMIT zoneChanged();
    Q_EMIT saveZone(QPoint(m_zoneIn, m_zoneOut));
}

void MonitorProxy::startZoneMove()
{
    m_undoZone = QPoint(m_zoneIn, m_zoneOut);
}

void MonitorProxy::endZoneMove()
{
    Q_EMIT saveZoneWithUndo(m_undoZone, QPoint(m_zoneIn, m_zoneOut));
}

void MonitorProxy::setZone(int in, int out, bool sendUpdate)
{
    if (m_zoneIn > 0) {
        Q_EMIT removeSnap(m_zoneIn);
    }
    if (m_zoneOut > 0) {
        Q_EMIT removeSnap(m_zoneOut);
    }
    m_zoneIn = in;
    m_zoneOut = out;
    if (m_zoneIn > 0) {
        Q_EMIT addSnap(m_zoneIn);
    }
    if (m_zoneOut > 0) {
        Q_EMIT addSnap(m_zoneOut);
    }
    Q_EMIT zoneChanged();
    if (sendUpdate) {
        Q_EMIT saveZone(QPoint(m_zoneIn, m_zoneOut));
    }
}

void MonitorProxy::setZone(QPoint zone, bool sendUpdate)
{
    setZone(zone.x(), zone.y(), sendUpdate);
}

void MonitorProxy::resetZone()
{
    m_zoneIn = 0;
    m_zoneOut = -1;
    m_clipBounds = {};
    m_boundsCount = 0;
    Q_EMIT clipBoundsChanged();
    Q_EMIT zoneChanged();
}

QPoint MonitorProxy::zone() const
{
    return {m_zoneIn, m_zoneOut};
}

double MonitorProxy::timeZoomFactor() const
{
    return m_timeZoomFactor;
}

void MonitorProxy::setTimeZoomFactor(double factor)
{
    m_timeZoomFactor = factor;
    Q_EMIT timeZoomFactorChanged();
}

double MonitorProxy::timeZoomOffset() const
{
    return m_timeZoomOffset;
}

void MonitorProxy::setTimeZoomOffset(double offset)
{
    m_timeZoomOffset = offset;
    Q_EMIT timeZoomOffsetChanged();
}

void MonitorProxy::resetTimeZoom()
{
    m_timeZoomFactor = 1;
    m_timeZoomOffset = 0;
    Q_EMIT timeZoomFactorChanged();
    Q_EMIT timeZoomOffsetChanged();
}

void MonitorProxy::extractFrameToFile(int frame_position, const QStringList &pathInfo, bool addToProject, bool useSourceProfile)
{
    const QString path = pathInfo.at(0);
    const QString destPath = pathInfo.at(1);
    const QString folderInfo = pathInfo.at(2);
    QSize finalSize = pCore->getCurrentFrameDisplaySize();
    QSize size = pCore->getCurrentFrameSize();
    int height = size.height();
    int width = size.width();
    if (path.isEmpty()) {
        // Use current monitor producer to extract frame
        Mlt::Frame *frame = q->m_producer->get_frame();
        QImage img = KThumb::getFrame(frame, width, height, finalSize.width());
        delete frame;
        img.save(destPath);
        if (addToProject) {
            QMetaObject::invokeMethod(pCore->bin(), "droppedUrls", Q_ARG(QList<QUrl>, {QUrl::fromLocalFile(destPath)}), Q_ARG(QString, folderInfo));
        }
        return;
    }
    QScopedPointer<Mlt::Producer> producer;
    QScopedPointer<Mlt::Profile> tmpProfile;
    if (useSourceProfile) {
        tmpProfile.reset(new Mlt::Profile());
        producer.reset(new Mlt::Producer(*tmpProfile, path.toUtf8().constData()));
    } else {
        producer.reset(new Mlt::Producer(pCore->getProjectProfile(), path.toUtf8().constData()));
    }
    if (producer && producer->is_valid()) {
        if (useSourceProfile) {
            tmpProfile->from_producer(*producer);
            width = tmpProfile->width();
            height = tmpProfile->height();
            if (tmpProfile->sar() != 1.) {
                finalSize.setWidth(qRound(height * tmpProfile->dar()));
            } else {
                finalSize.setWidth(0);
            }
            double projectFps = pCore->getCurrentFps();
            double currentFps = tmpProfile->fps();
            if (!qFuzzyCompare(projectFps, currentFps)) {
                frame_position = int(frame_position * currentFps / projectFps);
            }
        }
        QImage img = KThumb::getFrame(producer.data(), frame_position, width, height, finalSize.width());
        img.save(destPath);
        if (addToProject) {
            QMetaObject::invokeMethod(pCore->bin(), "droppedUrls", Q_ARG(QList<QUrl>, {QUrl::fromLocalFile(destPath)}), Q_ARG(QString, folderInfo));
        }
    } else {
        qDebug() << "::: INVALID PRODUCER: " << path;
    }
    if (QDir::temp().exists(path)) {
        // This was a temporary playlist file, remove
        QFile::remove(path);
    }
}

QImage MonitorProxy::extractFrame(const QString &path, int width, int height, bool useSourceProfile)
{
    if (width == -1) {
        width = pCore->getCurrentProfile()->width();
        height = pCore->getCurrentProfile()->height();
    } else if (width % 2 == 1) {
        width++;
    }

    if ((q->m_producer == nullptr) || !path.isEmpty()) {
        QImage pix(width, height, QImage::Format_RGB32);
        pix.fill(Qt::black);
        return pix;
    }
    Mlt::Frame *frame = nullptr;
    QImage img;
    if (useSourceProfile) {
        // Our source clip's resolution is higher than current profile, export at full res
        QScopedPointer<Mlt::Profile> tmpProfile(new Mlt::Profile());
        QString service = q->m_producer->get("mlt_service");
        QScopedPointer<Mlt::Producer> tmpProd(new Mlt::Producer(*tmpProfile, service.toUtf8().constData(), q->m_producer->get("resource")));
        tmpProfile->from_producer(*tmpProd);
        width = tmpProfile->width();
        height = tmpProfile->height();
        if (tmpProd && tmpProd->is_valid()) {
            Mlt::Filter scaler(*tmpProfile, "swscale");
            Mlt::Filter converter(*tmpProfile, "avcolor_space");
            tmpProd->attach(scaler);
            tmpProd->attach(converter);
            // TODO: paste effects
            // Clip(*tmpProd).addEffects(*q->m_producer);
            double projectFps = pCore->getCurrentFps();
            double currentFps = tmpProfile->fps();
            if (qFuzzyCompare(projectFps, currentFps)) {
                tmpProd->seek(q->m_producer->position());
            } else {
                int maxLength = int(q->m_producer->get_length() * currentFps / projectFps);
                tmpProd->set("length", maxLength);
                tmpProd->set("out", maxLength - 1);
                tmpProd->seek(int(q->m_producer->position() * currentFps / projectFps));
            }
            frame = tmpProd->get_frame();
            img = KThumb::getFrame(frame, width, height);
            delete frame;
        }
    } else if (KdenliveSettings::gpu_accel()) {
        QString service = q->m_producer->get("mlt_service");
        QScopedPointer<Mlt::Producer> tmpProd(new Mlt::Producer(pCore->getProjectProfile(), service.toUtf8().constData(), q->m_producer->get("resource")));
        Mlt::Filter scaler(pCore->getProjectProfile(), "swscale");
        Mlt::Filter converter(pCore->getProjectProfile(), "avcolor_space");
        tmpProd->attach(scaler);
        tmpProd->attach(converter);
        tmpProd->seek(q->m_producer->position());
        frame = tmpProd->get_frame();
        img = KThumb::getFrame(frame, width, height);
        delete frame;
    } else {
        frame = q->m_producer->get_frame();
        img = KThumb::getFrame(frame, width, height);
        delete frame;
    }
    return img;
}

void MonitorProxy::activateClipMonitor(bool isClipMonitor)
{
    pCore->monitorManager()->activateMonitor(isClipMonitor ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor);
}

QString MonitorProxy::toTimecode(int frames) const
{
    return KdenliveSettings::frametimecode() ? QString::number(frames) : q->frameToTime(frames);
}

void MonitorProxy::selectClip(int ix)
{
    if (ix < m_lastClipsIds.size()) {
        int cid = m_lastClipsIds.at(ix).first;
        pCore->bin()->selectClipById(QString::number(cid));
    }
}

MarkerSortModel *MonitorProxy::markersModel()
{
    return m_markerModel.get();
}

void MonitorProxy::setClipProperties(int clipId, ClipType::ProducerType type, bool hasAV, const QString &clipName, bool audioSynced,
                                     std::shared_ptr<MarkerSortModel> markerModel)
{
    bool idChanged = clipId != m_clipId;
    bool avChanged = hasAV != m_hasAV;
    bool typeChanged = type != m_clipType;
    bool audioChanged = audioSynced != m_audioSynced;
    bool markersChanged = markerModel != m_markerModel;
    m_markerModel = markerModel;
    m_clipId = clipId;
    m_hasAV = hasAV;
    m_clipType = type;
    m_audioSynced = audioSynced;

    if (markersChanged) {
        Q_EMIT markersModelChanged();
    }
    if (idChanged) {
        Q_EMIT clipIdChanged();
    }
    if (avChanged) {
        Q_EMIT clipHasAVChanged();
    }
    if (typeChanged) {
        Q_EMIT clipTypeChanged();
    }
    if (audioChanged) {
        Q_EMIT audioSyncedChanged();
    }
    if (!clipName.isEmpty()) {
        std::pair<int, QString> id = {clipId, clipName};
        for (int i = 0; i < m_lastClipsIds.size(); i++) {
            if (m_lastClipsIds.at(i).first == clipId) {
                m_lastClipsIds.remove(i);
                break;
            }
        }
        m_lastClipsIds.prepend(id);
        while (m_lastClipsIds.size() > 4) {
            m_lastClipsIds.removeLast();
        }
        m_lastClips.clear();
        for (int i = 0; i < 4 && i < m_lastClipsIds.size(); i++) {
            m_lastClips << m_lastClipsIds.at(i).second;
        }
        Q_EMIT lastClipsChanged();
    }
    if (clipName == m_clipName) {
        m_clipName.clear();
        Q_EMIT clipNameChanged();
    }
    m_clipName = clipName;
    Q_EMIT clipNameChanged();
}

void MonitorProxy::clipDeleted(int cid)
{
    for (int i = 0; i < m_lastClipsIds.size(); i++) {
        if (m_lastClipsIds.at(i).first == cid) {
            m_lastClipsIds.remove(i);
            m_lastClips.clear();
            for (int j = 0; j < 4 && j < m_lastClipsIds.size(); j++) {
                m_lastClips << m_lastClipsIds.at(j).second;
            }
            Q_EMIT lastClipsChanged();
            break;
        }
    }
}

void MonitorProxy::documentClosed()
{
    m_lastClipsIds.clear();
    m_lastClips.clear();
    m_clipName.clear();
    Q_EMIT lastClipsChanged();
    Q_EMIT clipNameChanged();
}

void MonitorProxy::setAudioThumb(const QList<int> &streamIndexes, const QList<int> &channels)
{
    m_audioChannels = channels;
    m_audioStreams = streamIndexes;
    Q_EMIT audioThumbChanged();
}

void MonitorProxy::setAudioStream(const QString &name)
{
    m_clipStream = name;
    Q_EMIT clipStreamChanged();
}

QPoint MonitorProxy::profile()
{
    QSize s = pCore->getCurrentFrameSize();
    return QPoint(s.width(), s.height());
}

const QString MonitorProxy::timecode() const
{
    if (m_td) {
        return m_td->displayText();
    }
    return QString();
}

const QString MonitorProxy::trimmingTC1() const
{
    return toTimecode(m_trimmingFrames1);
}

const QString MonitorProxy::trimmingTC2() const
{
    return toTimecode(m_trimmingFrames2);
}

void MonitorProxy::setTimeCode(TimecodeDisplay *td)
{
    m_td = td;
    connect(m_td, &TimecodeDisplay::timeCodeUpdated, this, &MonitorProxy::timecodeChanged);
}

void MonitorProxy::setTrimmingTC1(int frames, bool isRelativ)
{
    if (isRelativ) {
        m_trimmingFrames1 -= frames;
    } else {
        m_trimmingFrames1 = frames;
    }
    Q_EMIT trimmingTC1Changed();
}

void MonitorProxy::setTrimmingTC2(int frames, bool isRelativ)
{
    if (isRelativ) {
        m_trimmingFrames2 -= frames;
    } else {
        m_trimmingFrames2 = frames;
    }
    Q_EMIT trimmingTC2Changed();
}

void MonitorProxy::setWidgetKeyBinding(const QString &text) const
{
    pCore->setWidgetKeyBinding(text);
}

void MonitorProxy::setSpeed(double speed)
{
    if (m_speed == speed) {
        return;
    }
    m_speed = speed;
    Q_EMIT speedChanged();
}

QByteArray MonitorProxy::getUuid() const
{
    return QUuid::createUuid().toByteArray();
}

void MonitorProxy::updateClipBounds(const QVector<QPoint> &bounds)
{
    if (bounds.size() == m_boundsCount) {
        // Enforce refresh, in/out points may have changed
        m_boundsCount = 0;
        Q_EMIT clipBoundsChanged();
    }
    m_clipBounds = bounds;
    m_boundsCount = bounds.size();
    Q_EMIT clipBoundsChanged();
}

const QPoint MonitorProxy::clipBoundary(int ix)
{
    return m_clipBounds.at(ix);
}

void MonitorProxy::addEffect(const QString &effectData, const QString &effectSource)
{
    QStringList effectInfo = effectSource.split(QLatin1Char(','));
    effectInfo.prepend(effectData);
    if (m_clipId > -1) {
        QMetaObject::invokeMethod(pCore->bin(), "slotAddEffect", Qt::QueuedConnection, Q_ARG(std::vector<QString>, {QString::number(m_clipId)}),
                                  Q_ARG(QStringList, effectInfo));
    } else {
        // Dropped in project monitor
        QMetaObject::invokeMethod(this, "addTimelineEffect", Qt::QueuedConnection, Q_ARG(QStringList, effectInfo));
    }
}

void MonitorProxy::setJobsProgress(const ObjectId &owner, const QStringList &jobNames, const QList<int> &jobProgress, const QStringList &jobUuids)
{
    if (owner.itemId != m_clipId) {
        // Not interested
        return;
    }

    m_jobsProgress = jobProgress;
    m_jobsUuids = jobUuids;
    if (m_runningJobs != jobNames) {
        m_runningJobs = jobNames;
        Q_EMIT runningJobsChanged();
    }
    Q_EMIT jobsProgressChanged();
}

void MonitorProxy::clearJobsProgress()
{
    if (!m_runningJobs.isEmpty()) {
        m_jobsProgress.clear();
        m_jobsUuids.clear();
        m_runningJobs.clear();
        Q_EMIT runningJobsChanged();
    }
}

void MonitorProxy::terminateJob(const QString &uuid)
{
    pCore->taskManager.discardJob(ObjectId(KdenliveObjectType::BinClip, m_clipId, QUuid()), QUuid(uuid));
}

void MonitorProxy::resizeMarker(int position, int duration, bool isStart, int newPosition)
{
    std::shared_ptr<MarkerListModel> markerModel;

    if (q->m_id == int(Kdenlive::ClipMonitor)) {
        // For clip monitor, use the currently active clip
        auto activeClip = pCore->monitorManager()->clipMonitor()->activeClipId();
        if (!activeClip.isEmpty()) {
            auto clip = pCore->bin()->getBinClip(activeClip);
            if (clip) {
                markerModel = clip->getMarkerModel();
            }
        }
    } else {
        // For project monitor, use the timeline guide model
        if (pCore->currentDoc()) {
            markerModel = pCore->currentDoc()->getGuideModel(pCore->currentTimelineId());
        }
    }

    if (markerModel) {
        GenTime pos(position, pCore->getCurrentFps());
        bool exists;
        CommentedTime marker = markerModel->getMarker(pos, &exists);
        if (exists && marker.hasRange()) {
            GenTime newDuration(duration, pCore->getCurrentFps());
            GenTime newStartTime;

            if (isStart) {
                if (newPosition != -1) {
                    newStartTime = GenTime(newPosition, pCore->getCurrentFps());
                } else {
                    GenTime endTime = marker.endTime();
                    newStartTime = endTime - newDuration;
                }
            } else {
                newStartTime = pos;
            }

            if (newDuration < GenTime(1, pCore->getCurrentFps())) {
                newDuration = GenTime(1, pCore->getCurrentFps());
            }

            markerModel->editMarker(pos, newStartTime, marker.comment(), marker.markerType(), newDuration);
        }
    }
}

bool MonitorProxy::createRangeMarkerFromZone(const QString &comment, int type)
{
    if (m_zoneIn < 0 || m_zoneOut <= 0 || m_zoneIn >= m_zoneOut) {
        return false;
    }

    std::shared_ptr<MarkerListModel> markerModel;

    if (q->m_id == int(Kdenlive::ClipMonitor)) {
        auto activeClip = pCore->monitorManager()->clipMonitor()->activeClipId();
        if (!activeClip.isEmpty()) {
            auto clip = pCore->bin()->getBinClip(activeClip);
            if (clip) {
                markerModel = clip->getMarkerModel();
            }
        }
    } else {
        if (pCore->currentDoc()) {
            markerModel = pCore->currentDoc()->getGuideModel(pCore->currentTimelineId());
        }
    }

    if (!markerModel) {
        return false;
    }

    GenTime startPos(m_zoneIn, pCore->getCurrentFps());
    GenTime duration(m_zoneOut - m_zoneIn, pCore->getCurrentFps());
    QString markerComment = comment.isEmpty() ? i18n("Zone marker") : comment;

    if (type == -1) {
        type = KdenliveSettings::default_marker_type();
    }

    bool success = markerModel->addRangeMarker(startPos, duration, markerComment, type);
    return success;
}

bool MonitorProxy::monitorIsActive() const
{
    return pCore->monitorManager()->isActive(Kdenlive::MonitorId(q->m_id));
}

bool MonitorProxy::isKeyframe() const
{
    return m_isKeyframe;
}

bool MonitorProxy::cursorOutsideEffect() const
{
    return m_cursorOutsideEffect;
}

void MonitorProxy::setIsKeyframe(bool isKeyframe)
{
    m_isKeyframe = isKeyframe;
    Q_EMIT isKeyframeChanged();
}

void MonitorProxy::setCursorOutsideEffect(bool isOutside)
{
    m_cursorOutsideEffect = isOutside;
    Q_EMIT cursorOutsideEffectChanged();
}

const QString MonitorProxy::dragType()
{
    return m_dragType;
}

void MonitorProxy::setDragType(const QString dragType)
{
    m_dragType = dragType;
    Q_EMIT dragTypeChanged();
}

void MonitorProxy::setAudioSynced(bool synced)
{
    m_audioSynced = synced;
    Q_EMIT audioSyncedChanged();
}

void MonitorProxy::refreshAudio()
{
    Q_EMIT rebuildAudio(m_clipId);
}

namespace {
std::shared_ptr<TimelineItemModel> currentTimelineForMonitor()
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
} // namespace

QVariantMap MonitorProxy::canvasOverlayState() const
{
    QVariantMap state;
    const int pos = qMax(0, m_position);
    const double fps = pCore->getCurrentFps() > 0.0 ? pCore->getCurrentFps() : 60.0;
    state.insert(QStringLiteral("frame"), pos);
    state.insert(QStringLiteral("fps"), fps);
    state.insert(QStringLiteral("selectedZone"), m_selectedCanvasZone);
    state.insert(QStringLiteral("hasTitle"), false);
    state.insert(QStringLiteral("titleClipId"), -1);
    state.insert(QStringLiteral("titleText"), QString());
    state.insert(QStringLiteral("hasMotion"), false);
    state.insert(QStringLiteral("motionClipId"), -1);
    state.insert(QStringLiteral("motionName"), QString());
    state.insert(QStringLiteral("hasSubtitle"), false);
    state.insert(QStringLiteral("subtitleId"), -1);
    state.insert(QStringLiteral("subtitleText"), QString());
    state.insert(QStringLiteral("subtitleStyle"), QStringLiteral("Costura"));
    state.insert(QStringLiteral("subNormY"), 0.515);
    state.insert(QStringLiteral("hasCamera"), false);
    state.insert(QStringLiteral("cameraClipId"), -1);
    state.insert(QStringLiteral("cameraName"), QString());
    state.insert(QStringLiteral("cameraStartFrame"), 0);
    state.insert(QStringLiteral("cameraDurationFrames"), 0);
    state.insert(QStringLiteral("cutIndex"), 0);
    state.insert(QStringLiteral("totalCuts"), 0);

    QJsonObject tlResult;
    int errCode = 0;
    QString errMsg;
    if (!MEditor::Handlers::dispatch(nullptr, QStringLiteral("timeline.get"), QJsonObject(), tlResult, errCode, errMsg)) {
        return state;
    }

    const QJsonArray clips = tlResult.value(QStringLiteral("clips")).toArray();
    struct CutEntry
    {
        int clipId;
        int start;
        int dur;
        QString name;
    };
    QVector<CutEntry> v1Cuts;

    for (const QJsonValue &val : clips) {
        const QJsonObject c = val.toObject();
        if (c.value(QStringLiteral("isAudio")).toBool()) {
            continue;
        }
        const int cid = c.value(QStringLiteral("clipId")).toInt(-1);
        const int cStart = c.value(QStringLiteral("position")).toInt(0);
        const int cDur = c.value(QStringLiteral("duration")).toInt(0);
        const int cEnd = cStart + cDur;
        const QString trackTag = c.value(QStringLiteral("trackTag")).toString();
        const int clipType = c.value(QStringLiteral("clipType")).toInt(0);
        const QString cName = c.value(QStringLiteral("name")).toString();
        const QString tText = c.value(QStringLiteral("titleText")).toString();

        if (clipType == int(ClipType::Text) || trackTag == QLatin1String("V3") || !tText.isEmpty()) {
            if (!state.value(QStringLiteral("hasTitle")).toBool() || (pos >= cStart && pos < cEnd)) {
                state.insert(QStringLiteral("hasTitle"), true);
                state.insert(QStringLiteral("titleClipId"), cid);
                state.insert(QStringLiteral("titleText"), tText.isEmpty() ? cName : tText);
            }
        } else if (trackTag == QLatin1String("V2")) {
            if (pos >= cStart && pos < cEnd) {
                state.insert(QStringLiteral("hasMotion"), true);
                state.insert(QStringLiteral("motionClipId"), cid);
                state.insert(QStringLiteral("motionName"), cName);
            }
        } else if (trackTag == QLatin1String("V1")) {
            v1Cuts.push_back({cid, cStart, cDur, cName});
        }
    }

    std::sort(v1Cuts.begin(), v1Cuts.end(), [](const CutEntry &a, const CutEntry &b) { return a.start < b.start; });
    state.insert(QStringLiteral("totalCuts"), v1Cuts.size());
    for (int i = 0; i < v1Cuts.size(); ++i) {
        const CutEntry &cut = v1Cuts.at(i);
        if (pos >= cut.start && pos < cut.start + cut.dur) {
            state.insert(QStringLiteral("hasCamera"), true);
            state.insert(QStringLiteral("cameraClipId"), cut.clipId);
            state.insert(QStringLiteral("cameraName"), cut.name);
            state.insert(QStringLiteral("cameraStartFrame"), cut.start);
            state.insert(QStringLiteral("cameraDurationFrames"), cut.dur);
            state.insert(QStringLiteral("cutIndex"), i + 1);
            break;
        }
    }

    const QJsonArray subs = tlResult.value(QStringLiteral("subtitles")).toArray();
    for (const QJsonValue &sVal : subs) {
        const QJsonObject s = sVal.toObject();
        const int sIn = s.value(QStringLiteral("startFrame")).toInt(0);
        const int sOut = s.value(QStringLiteral("endFrame")).toInt(0);
        if (pos >= sIn && pos <= sOut) {
            const QString stName = s.value(QStringLiteral("style")).toString(QStringLiteral("Costura"));
            state.insert(QStringLiteral("hasSubtitle"), true);
            state.insert(QStringLiteral("subtitleId"), s.value(QStringLiteral("id")).toInt(-1));
            state.insert(QStringLiteral("subtitleText"), s.value(QStringLiteral("text")).toString());
            state.insert(QStringLiteral("subtitleStyle"), stName);
            if (auto tl = currentTimelineForMonitor()) {
                if (auto subModel = tl->getSubtitleModel()) {
                    const auto &styles = subModel->getAllSubtitleStyles();
                    auto it = styles.find(stName);
                    if (it != styles.end()) {
                        const int mv = it->second.marginV();
                        const double normY = qBound(0.12, 1.0 - (double(mv) / 1920.0), 0.88);
                        state.insert(QStringLiteral("subNormY"), normY);
                    }
                }
            }
            break;
        }
    }
    return state;
}

QVariantMap MonitorProxy::selectCanvasZone(const QString &zone)
{
    m_selectedCanvasZone = zone;
    const QVariantMap state = canvasOverlayState();
    auto tl = currentTimelineForMonitor();
    if (tl) {
        int itemId = -1;
        if (zone == QLatin1String("title")) {
            itemId = state.value(QStringLiteral("titleClipId"), -1).toInt();
        } else if (zone == QLatin1String("motion")) {
            itemId = state.value(QStringLiteral("motionClipId"), -1).toInt();
        } else if (zone == QLatin1String("subtitle")) {
            itemId = state.value(QStringLiteral("subtitleId"), -1).toInt();
        } else if (zone == QLatin1String("camera")) {
            itemId = state.value(QStringLiteral("cameraClipId"), -1).toInt();
        }
        if (itemId >= 0) {
            tl->requestSetSelection({itemId});
        }
        if (zone == QLatin1String("subtitle")) {
            if (pCore->subtitleWidget() && itemId >= 0) {
                pCore->subtitleWidget()->setActiveSubtitle(itemId);
            }
            if (pCore->window()) {
                pCore->window()->focusSubtitlesDock();
            }
        }
    }
    Q_EMIT canvasOverlayChanged();
    return canvasOverlayState();
}

void MonitorProxy::togglePlay()
{
    if (pCore->monitorManager() && pCore->monitorManager()->projectMonitor()) {
        pCore->monitorManager()->projectMonitor()->slotPlay();
    }
}

void MonitorProxy::jumpPrevCut()
{
    const QVariantMap state = canvasOverlayState();
    QJsonObject tlResult;
    int errCode = 0;
    QString errMsg;
    if (!MEditor::Handlers::dispatch(nullptr, QStringLiteral("timeline.get"), QJsonObject(), tlResult, errCode, errMsg)) {
        return;
    }
    QVector<QPair<int, int>> v1Starts;
    const QJsonArray clips = tlResult.value(QStringLiteral("clips")).toArray();
    for (const QJsonValue &v : clips) {
        const QJsonObject c = v.toObject();
        if (c.value(QStringLiteral("trackTag")).toString() == QLatin1String("V1")) {
            v1Starts.push_back({c.value(QStringLiteral("position")).toInt(0), c.value(QStringLiteral("clipId")).toInt(-1)});
        }
    }
    std::sort(v1Starts.begin(), v1Starts.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    int targetFrame = 0;
    int targetClipId = -1;
    const int cur = qMax(0, m_position);
    for (int i = v1Starts.size() - 1; i >= 0; --i) {
        if (v1Starts.at(i).first < cur - 4) {
            targetFrame = v1Starts.at(i).first;
            targetClipId = v1Starts.at(i).second;
            break;
        }
    }
    if (targetClipId < 0 && !v1Starts.isEmpty()) {
        targetFrame = v1Starts.first().first;
        targetClipId = v1Starts.first().second;
    }
    setPosition(targetFrame);
    if (targetClipId >= 0) {
        if (auto tl = currentTimelineForMonitor()) {
            tl->requestSetSelection({targetClipId});
        }
    }
    Q_EMIT canvasOverlayChanged();
}

void MonitorProxy::jumpNextCut()
{
    QJsonObject tlResult;
    int errCode = 0;
    QString errMsg;
    if (!MEditor::Handlers::dispatch(nullptr, QStringLiteral("timeline.get"), QJsonObject(), tlResult, errCode, errMsg)) {
        return;
    }
    QVector<QPair<int, int>> v1Starts;
    const QJsonArray clips = tlResult.value(QStringLiteral("clips")).toArray();
    for (const QJsonValue &v : clips) {
        const QJsonObject c = v.toObject();
        if (c.value(QStringLiteral("trackTag")).toString() == QLatin1String("V1")) {
            v1Starts.push_back({c.value(QStringLiteral("position")).toInt(0), c.value(QStringLiteral("clipId")).toInt(-1)});
        }
    }
    std::sort(v1Starts.begin(), v1Starts.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    const int cur = qMax(0, m_position);
    for (const auto &pair : v1Starts) {
        if (pair.first > cur + 2) {
            setPosition(pair.first);
            if (auto tl = currentTimelineForMonitor()) {
                tl->requestSetSelection({pair.second});
            }
            Q_EMIT canvasOverlayChanged();
            return;
        }
    }
}

bool MonitorProxy::splitActiveClipAtPlayhead()
{
    const QVariantMap state = canvasOverlayState();
    const int cid = state.value(QStringLiteral("cameraClipId"), -1).toInt();
    const int cur = qMax(0, m_position);
    if (cid < 0) {
        return false;
    }
    QJsonObject outResult;
    int errCode = 0;
    QString errMsg;
    const bool ok = MEditor::Handlers::dispatch(
        nullptr, QStringLiteral("clip.cut"),
        QJsonObject{{QStringLiteral("clipId"), cid}, {QStringLiteral("frame"), cur}, {QStringLiteral("simulateUser"), true}},
        outResult, errCode, errMsg);
    Q_EMIT canvasOverlayChanged();
    return ok;
}

bool MonitorProxy::rippleTrimActiveCut(int deltaFrames)
{
    const QVariantMap state = canvasOverlayState();
    const int cid = state.value(QStringLiteral("cameraClipId"), -1).toInt();
    const int curDur = state.value(QStringLiteral("cameraDurationFrames"), 0).toInt();
    const int cStart = state.value(QStringLiteral("cameraStartFrame"), 0).toInt();
    if (cid < 0 || curDur <= 0) {
        return false;
    }
    const int newDur = qMax(15, curDur + deltaFrames);
    QJsonObject outResult;
    int errCode = 0;
    QString errMsg;
    const bool ok = MEditor::Handlers::dispatch(
        nullptr, QStringLiteral("clip.resize"),
        QJsonObject{{QStringLiteral("clipId"), cid},
                    {QStringLiteral("duration"), newDur},
                    {QStringLiteral("edge"), QStringLiteral("right")},
                    {QStringLiteral("ripple"), true},
                    {QStringLiteral("simulateUser"), true}},
        outResult, errCode, errMsg);
    if (ok) {
        setPosition(qMax(0, cStart + newDur - 3));
    }
    Q_EMIT canvasOverlayChanged();
    return ok;
}

QString MonitorProxy::cycleSubtitlePreset()
{
    static const QStringList presets = {
        QStringLiteral("Costura"),
        QStringLiteral("Padrao_EditorIA"),
        QStringLiteral("Destaque_Amarelo"),
        QStringLiteral("Pilula_Verde"),
        QStringLiteral("Pilula_Azul"),
        QStringLiteral("Caixa_Alta_Impacto"),
        QStringLiteral("Editorial_Limpa"),
        QStringLiteral("Ancora_Amarela")
    };
    m_presetCycleIndex = (m_presetCycleIndex + 1) % presets.size();
    const QString chosen = presets.at(m_presetCycleIndex);
    applySubtitlePresetFromMonitor(chosen);
    return chosen;
}

void MonitorProxy::applySubtitlePresetFromMonitor(const QString &presetName)
{
    if (presetName.isEmpty()) {
        return;
    }
    QJsonObject outResult;
    int errCode = 0;
    QString errMsg;
    MEditor::Handlers::dispatch(
        nullptr, QStringLiteral("subtitle.style"),
        QJsonObject{{QStringLiteral("preset"), presetName}, {QStringLiteral("applyToAll"), true}, {QStringLiteral("simulateUser"), true}},
        outResult, errCode, errMsg);
    Q_EMIT canvasOverlayChanged();
}

void MonitorProxy::openSubtitleFontDialog()
{
    bool ok = false;
    const QFont initialFont(QStringLiteral("Arial"), 54, QFont::Bold);
    const QFont chosen = QFontDialog::getFont(&ok, initialFont, pCore->window(), QStringLiteral("Escolher Fonte de Todas as Legendas"));
    if (!ok) {
        return;
    }
    QJsonObject outResult;
    int errCode = 0;
    QString errMsg;
    MEditor::Handlers::dispatch(
        nullptr, QStringLiteral("subtitle.style"),
        QJsonObject{{QStringLiteral("fontName"), chosen.family()},
                    {QStringLiteral("fontSize"), double(chosen.pointSize() > 16 ? chosen.pointSize() : 54)},
                    {QStringLiteral("bold"), chosen.bold()},
                    {QStringLiteral("italic"), chosen.italic()},
                    {QStringLiteral("applyToAll"), true},
                    {QStringLiteral("simulateUser"), true}},
        outResult, errCode, errMsg);
    Q_EMIT canvasOverlayChanged();
}

bool MonitorProxy::updateActiveSubtitleText(const QString &newText)
{
    const QString trimmed = newText.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const QVariantMap state = canvasOverlayState();
    const int subId = state.value(QStringLiteral("subtitleId"), -1).toInt();
    if (subId < 0) {
        return false;
    }
    QJsonObject outResult;
    int errCode = 0;
    QString errMsg;
    const bool ok = MEditor::Handlers::dispatch(
        nullptr, QStringLiteral("subtitle.edit"),
        QJsonObject{{QStringLiteral("id"), subId}, {QStringLiteral("text"), trimmed}, {QStringLiteral("simulateUser"), true}},
        outResult, errCode, errMsg);
    Q_EMIT canvasOverlayChanged();
    return ok;
}

void MonitorProxy::dragSubtitleToNormY(double normY)
{
    const double clamped = qBound(0.14, normY, 0.88);
    const int marginV = qRound((1.0 - clamped) * 1920.0);
    QJsonObject outResult;
    int errCode = 0;
    QString errMsg;
    MEditor::Handlers::dispatch(
        nullptr, QStringLiteral("subtitle.style"),
        QJsonObject{{QStringLiteral("marginV"), marginV},
                    {QStringLiteral("alignment"), 2},
                    {QStringLiteral("applyToAll"), true},
                    {QStringLiteral("simulateUser"), true}},
        outResult, errCode, errMsg);
    Q_EMIT canvasOverlayChanged();
}

bool MonitorProxy::updateFullVideoTitleText(const QString &newText)
{
    const QString trimmed = newText.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    QJsonObject outResult;
    int errCode = 0;
    QString errMsg;
    bool ok = MEditor::Handlers::dispatch(
        nullptr, QStringLiteral("title.edit"),
        QJsonObject{{QStringLiteral("text"), trimmed}, {QStringLiteral("simulateUser"), true}},
        outResult, errCode, errMsg);
    if (!ok) {
        ok = MEditor::Handlers::dispatch(
            nullptr, QStringLiteral("title.add"),
            QJsonObject{{QStringLiteral("text"), trimmed}, {QStringLiteral("track"), QStringLiteral("V3")}, {QStringLiteral("simulateUser"), true}},
            outResult, errCode, errMsg);
    }
    Q_EMIT canvasOverlayChanged();
    return ok;
}
