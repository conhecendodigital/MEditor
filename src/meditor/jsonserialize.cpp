/*
    SPDX-FileCopyrightText: 2026 MEditor Project
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "jsonserialize.h"

#include "bin/model/markerlistmodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QDomDocument>
#include <QJsonValue>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <mlt++/MltProducer.h>
#include <vector>

namespace MEditor {

namespace {

QString extractTitleTextsFromXml(const QString &xmlData)
{
    if (xmlData.isEmpty()) {
        return {};
    }
    QDomDocument doc;
    if (!doc.setContent(xmlData)) {
        return {};
    }
    QStringList lines;
    const QDomNodeList items = doc.elementsByTagName(QStringLiteral("item"));
    for (int i = 0; i < items.count(); ++i) {
        const QDomElement itemEl = items.at(i).toElement();
        if (itemEl.attribute(QStringLiteral("type")) == QLatin1String("QGraphicsTextItem")) {
            const QDomElement contentEl = itemEl.firstChildElement(QStringLiteral("content"));
            if (!contentEl.isNull()) {
                const QString txt = contentEl.text().trimmed();
                if (!txt.isEmpty()) {
                    lines.append(txt);
                }
            }
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QString readMltStringProperty(const std::shared_ptr<Mlt::Producer> &prod, const char *key)
{
    if (!prod || !prod->is_valid()) {
        return {};
    }
    if (prod->property_exists(key)) {
        const char *val = prod->get(key);
        if (val && val[0] != '\0') {
            return QString::fromUtf8(val);
        }
    }
    Mlt::Producer &parent = prod->parent();
    if (parent.is_valid() && parent.property_exists(key)) {
        const char *val = parent.get(key);
        if (val && val[0] != '\0') {
            return QString::fromUtf8(val);
        }
    }
    return {};
}

} // namespace

int JsonSerialize::resolveTrackId(const std::shared_ptr<TimelineItemModel> &timeline, const QJsonValue &trackVal,
                                  bool preferAudio)
{
    if (!timeline) {
        return -1;
    }

    const std::unordered_set<int> allIds = timeline->getAllTracksIds();
    if (allIds.empty()) {
        return -1;
    }

    if (trackVal.isDouble()) {
        const int candidate = trackVal.toInt(-1);
        if (allIds.find(candidate) != allIds.end()) {
            return candidate;
        }
    }

    const QString wanted = trackVal.toString().trimmed();
    if (!wanted.isEmpty()) {
        bool okInt = false;
        const int asInt = wanted.toInt(&okInt);
        if (okInt && allIds.find(asInt) != allIds.end()) {
            return asInt;
        }

        for (int tid : allIds) {
            const QString tag = timeline->getTrackTagById(tid);
            const QString name = timeline->getTrackProperty(tid, QStringLiteral("kdenlive:track_name")).toString();
            if (tag.compare(wanted, Qt::CaseInsensitive) == 0 ||
                (!name.isEmpty() && name.compare(wanted, Qt::CaseInsensitive) == 0)) {
                return tid;
            }
        }
    }

    // Fallback: primeira trilha de vídeo (ex.: V1) ou áudio (ex.: A1) ordenada por posição
    const QList<int> ordered = timeline->getTracksIds(preferAudio);
    if (!ordered.isEmpty()) {
        for (int tid : ordered) {
            if (!timeline->isSubtitleTrack(tid) && !timeline->trackIsLocked(tid)) {
                return tid;
            }
        }
        return ordered.first();
    }

    return *allIds.begin();
}

QJsonObject JsonSerialize::serializeClip(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, double fps)
{
    QJsonObject obj;
    if (!timeline || !timeline->isClip(clipId)) {
        return obj;
    }

    const int trackId = timeline->getClipTrackId(clipId);
    const QString trackTag = trackId >= 0 ? timeline->getTrackTagById(trackId) : QString();
    const QString trackNameProp =
        trackId >= 0 ? timeline->getTrackProperty(trackId, QStringLiteral("kdenlive:track_name")).toString() : QString();
    const QString trackName = trackNameProp.isEmpty() ? trackTag : trackNameProp;
    const bool isAudio = trackId >= 0 ? timeline->isAudioTrack(trackId) : false;

    const int position = timeline->getClipPosition(clipId);
    const std::pair<int, int> inOut = timeline->getClipInOut(clipId);
    const int playtime = timeline->getClipPlaytime(clipId);
    const QString binId = timeline->getClipBinId(clipId);
    const QString clipName = timeline->getClipName(clipId);
    const double speed = timeline->getClipSpeed(clipId);
    const int splitPartner = timeline->getClipSplitPartner(clipId);

    QString sourcePath;
    QString titleXmlData;
    int clipTypeInt = 0;
    if (auto itemModel = pCore->projectItemModel()) {
        if (auto binClip = itemModel->getClipByBinID(binId)) {
            sourcePath = binClip->clipUrl();
            clipTypeInt = int(binClip->clipType());
            if (binClip->clipType() == ClipType::Text) {
                titleXmlData = binClip->getProducerProperty(QStringLiteral("xmldata"));
            }
        }
    }

    std::shared_ptr<Mlt::Producer> prod = timeline->getClipProducer(clipId);
    if (titleXmlData.isEmpty()) {
        titleXmlData = readMltStringProperty(prod, "xmldata");
    }
    const QString titleText = extractTitleTextsFromXml(titleXmlData);

    QString meditorUid = readMltStringProperty(prod, "meditor:uid");
    if (meditorUid.isEmpty()) {
        meditorUid = readMltStringProperty(prod, "kdenlive:meditor_uid");
    }
    if (meditorUid.isEmpty()) {
        meditorUid = QStringLiteral("clip:%1").arg(clipId);
    }

    const QString keepIndexStr = readMltStringProperty(prod, "meditor:keep_index");
    const QString reason = readMltStringProperty(prod, "meditor:reason");
    QString kind = readMltStringProperty(prod, "meditor:kind");
    if (kind.isEmpty()) {
        if (!titleXmlData.isEmpty() || clipTypeInt == int(ClipType::Text)) {
            kind = QStringLiteral("title");
        } else {
            kind = isAudio ? QStringLiteral("audio") : QStringLiteral("video");
        }
    }
    if (sourcePath.isEmpty()) {
        sourcePath = readMltStringProperty(prod, "resource");
    }

    QJsonObject props;
    props.insert(QStringLiteral("meditor:uid"), meditorUid);
    props.insert(QStringLiteral("kdenlive:clipname"), clipName);
    props.insert(QStringLiteral("meditor:kind"), kind);
    if (!sourcePath.isEmpty()) {
        props.insert(QStringLiteral("resource"), sourcePath);
    }
    if (!titleXmlData.isEmpty()) {
        props.insert(QStringLiteral("xmldata"), titleXmlData);
    }
    if (!titleText.isEmpty()) {
        props.insert(QStringLiteral("meditor:title_text"), titleText);
    }
    if (!reason.isEmpty()) {
        props.insert(QStringLiteral("meditor:reason"), reason);
    }
    if (!keepIndexStr.isEmpty()) {
        bool okKeep = false;
        const int keepIdx = keepIndexStr.toInt(&okKeep);
        if (okKeep) {
            props.insert(QStringLiteral("meditor:keep_index"), keepIdx);
            obj.insert(QStringLiteral("keepIndex"), keepIdx);
        }
    }

    obj.insert(QStringLiteral("clipId"), clipId);
    obj.insert(QStringLiteral("id"), clipId);
    obj.insert(QStringLiteral("binId"), binId);
    obj.insert(QStringLiteral("trackId"), trackId);
    obj.insert(QStringLiteral("track"), trackTag.isEmpty() ? QString::number(trackId) : trackTag);
    obj.insert(QStringLiteral("trackName"), trackName);
    obj.insert(QStringLiteral("isAudio"), isAudio);
    obj.insert(QStringLiteral("audio"), isAudio);
    obj.insert(QStringLiteral("position"), position);
    obj.insert(QStringLiteral("pos"), position);
    obj.insert(QStringLiteral("in"), inOut.first);
    obj.insert(QStringLiteral("inFrame"), inOut.first);
    obj.insert(QStringLiteral("out"), inOut.second);
    obj.insert(QStringLiteral("outFrame"), inOut.second);
    obj.insert(QStringLiteral("playtime"), playtime);
    obj.insert(QStringLiteral("duration"), playtime);
    obj.insert(QStringLiteral("size"), playtime);
    obj.insert(QStringLiteral("name"), clipName);
    obj.insert(QStringLiteral("speed"), speed);
    obj.insert(QStringLiteral("splitPartner"), splitPartner);
    obj.insert(QStringLiteral("meditor_uid"), meditorUid);
    obj.insert(QStringLiteral("uid"), meditorUid);
    obj.insert(QStringLiteral("kind"), kind);
    obj.insert(QStringLiteral("clipType"), clipTypeInt);
    if (!titleText.isEmpty()) {
        obj.insert(QStringLiteral("titleText"), titleText);
        obj.insert(QStringLiteral("text"), titleText);
    }
    if (!titleXmlData.isEmpty()) {
        obj.insert(QStringLiteral("xmldata"), titleXmlData);
    }
    if (!reason.isEmpty()) {
        obj.insert(QStringLiteral("reason"), reason);
    }
    if (!sourcePath.isEmpty()) {
        obj.insert(QStringLiteral("sourcePath"), sourcePath);
        obj.insert(QStringLiteral("resource"), sourcePath);
    }
    if (fps > 0.0) {
        obj.insert(QStringLiteral("positionSeconds"), double(position) / fps);
        obj.insert(QStringLiteral("inSeconds"), double(inOut.first) / fps);
        obj.insert(QStringLiteral("outSeconds"), double(inOut.second) / fps);
        obj.insert(QStringLiteral("durationSeconds"), double(playtime) / fps);
    }
    obj.insert(QStringLiteral("effects"), QJsonArray());
    obj.insert(QStringLiteral("properties"), props);
    return obj;
}

QJsonObject JsonSerialize::serializeTrack(const std::shared_ptr<TimelineItemModel> &timeline, int trackId, double fps,
                                          QJsonArray *outFlatClips)
{
    QJsonObject trackObj;
    if (!timeline) {
        return trackObj;
    }

    const QString tag = timeline->getTrackTagById(trackId);
    const QString customName = timeline->getTrackProperty(trackId, QStringLiteral("kdenlive:track_name")).toString();
    const bool isAudio = timeline->isAudioTrack(trackId);
    const bool isSubtitle = timeline->isSubtitleTrack(trackId);
    const bool isLocked = timeline->trackIsLocked(trackId);
    const int hideFlags = timeline->getTrackProperty(trackId, QStringLiteral("hide")).toInt();
    const int position = timeline->getTrackPosition(trackId);
    const int mltIndex = timeline->getTrackMltIndex(trackId);
    const int clipsCount = timeline->getTrackClipsCount(trackId);

    // Coleta os clips desta trilha via getItemsInRange(trackId, 0, -1, false)
    const std::unordered_set<int> itemIds = timeline->getItemsInRange(trackId, 0, -1, false);
    std::vector<int> clipIds;
    clipIds.reserve(itemIds.size());
    for (int id : itemIds) {
        if (timeline->isClip(id) && timeline->getClipTrackId(id) == trackId) {
            clipIds.push_back(id);
        }
    }
    std::sort(clipIds.begin(), clipIds.end(), [&timeline](int a, int b) {
        const int posA = timeline->getClipPosition(a);
        const int posB = timeline->getClipPosition(b);
        return posA != posB ? (posA < posB) : (a < b);
    });

    QJsonArray trackClips;
    for (int cid : clipIds) {
        const QJsonObject clipJson = serializeClip(timeline, cid, fps);
        trackClips.append(clipJson);
        if (outFlatClips) {
            outFlatClips->append(clipJson);
        }
    }

    trackObj.insert(QStringLiteral("trackId"), trackId);
    trackObj.insert(QStringLiteral("id"), trackId);
    trackObj.insert(QStringLiteral("tag"), tag);
    trackObj.insert(QStringLiteral("name"), customName.isEmpty() ? tag : customName);
    trackObj.insert(QStringLiteral("trackName"), customName);
    trackObj.insert(QStringLiteral("position"), position);
    trackObj.insert(QStringLiteral("mltIndex"), mltIndex);
    trackObj.insert(QStringLiteral("audio"), isAudio);
    trackObj.insert(QStringLiteral("isAudio"), isAudio);
    trackObj.insert(QStringLiteral("subtitle"), isSubtitle);
    trackObj.insert(QStringLiteral("isSubtitle"), isSubtitle);
    trackObj.insert(QStringLiteral("locked"), isLocked);
    trackObj.insert(QStringLiteral("hidden"), (hideFlags & 1) != 0);
    trackObj.insert(QStringLiteral("muted"), (hideFlags & 2) != 0);
    trackObj.insert(QStringLiteral("clipsCount"), clipsCount);
    trackObj.insert(QStringLiteral("clips"), trackClips);
    return trackObj;
}

QJsonArray JsonSerialize::serializeGuides(const std::shared_ptr<TimelineItemModel> &timeline, KdenliveDoc *doc,
                                          double fps)
{
    QJsonArray guidesArr;
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
        return guidesArr;
    }

    const QList<CommentedTime> markers = guideModel->getAllMarkers();
    const double safeFps = fps > 0.0 ? fps : 30.0;
    for (int i = 0; i < markers.size(); ++i) {
        const CommentedTime &m = markers.at(i);
        const int frame = m.time().frames(safeFps);
        const double seconds = m.time().seconds();
        QJsonObject mObj;
        mObj.insert(QStringLiteral("id"), i);
        mObj.insert(QStringLiteral("frame"), frame);
        mObj.insert(QStringLiteral("pos"), frame);
        mObj.insert(QStringLiteral("position"), frame);
        mObj.insert(QStringLiteral("seconds"), seconds);
        mObj.insert(QStringLiteral("comment"), m.comment());
        mObj.insert(QStringLiteral("text"), m.comment());
        mObj.insert(QStringLiteral("type"), m.markerType());
        guidesArr.append(mObj);
    }
    return guidesArr;
}

QJsonArray JsonSerialize::serializeSubtitles(const std::shared_ptr<TimelineItemModel> &timeline, double fps)
{
    QJsonArray subsArr;
    if (!timeline) {
        return subsArr;
    }

    std::shared_ptr<SubtitleModel> subModel = timeline->getSubtitleModel();
    if (!subModel) {
        return subsArr;
    }

    const double safeFps = fps > 0.0 ? fps : 30.0;
    const auto allSubs = subModel->getAllSubtitles();
    for (const auto &entry : allSubs) {
        const int layer = entry.first.first;
        const GenTime &startTime = entry.first.second;
        const SubtitleEvent &ev = entry.second;
        const GenTime endTime = ev.endTime();
        const int subId = subModel->getIdForStartPos(layer, startTime);
        const int startFrame = startTime.frames(safeFps);
        const int endFrame = endTime.frames(safeFps);

        QJsonObject sObj;
        sObj.insert(QStringLiteral("id"), subId);
        sObj.insert(QStringLiteral("layer"), layer);
        sObj.insert(QStringLiteral("startFrame"), startFrame);
        sObj.insert(QStringLiteral("endFrame"), endFrame);
        sObj.insert(QStringLiteral("in"), startFrame);
        sObj.insert(QStringLiteral("out"), endFrame);
        sObj.insert(QStringLiteral("durationFrames"), std::max(0, endFrame - startFrame));
        sObj.insert(QStringLiteral("startSeconds"), startTime.seconds());
        sObj.insert(QStringLiteral("endSeconds"), endTime.seconds());
        sObj.insert(QStringLiteral("style"), ev.styleName());
        sObj.insert(QStringLiteral("text"), ev.text());
        subsArr.append(sObj);
    }
    return subsArr;
}

QJsonArray JsonSerialize::serializeBin()
{
    QJsonArray binArr;
    auto itemModel = pCore->projectItemModel();
    if (!itemModel) {
        return binArr;
    }

    std::vector<QString> clipIds = itemModel->getAllClipIds();
    std::sort(clipIds.begin(), clipIds.end());
    for (const QString &binId : clipIds) {
        std::shared_ptr<ProjectClip> clip = itemModel->getClipByBinID(binId);
        if (!clip) {
            continue;
        }
        const bool isReady = (clip->clipStatus() == FileStatus::StatusReady);
        QJsonObject cObj;
        cObj.insert(QStringLiteral("binId"), binId);
        cObj.insert(QStringLiteral("id"), binId);
        cObj.insert(QStringLiteral("name"), clip->clipName());
        cObj.insert(QStringLiteral("sourcePath"), clip->clipUrl());
        cObj.insert(QStringLiteral("url"), clip->clipUrl());
        cObj.insert(QStringLiteral("ready"), isReady);
        cObj.insert(QStringLiteral("duration"), isReady ? int(clip->frameDuration()) : 0);
        cObj.insert(QStringLiteral("playtime"), isReady ? clip->getFramePlaytime() : 0);
        cObj.insert(QStringLiteral("type"), int(clip->clipType()));
        cObj.insert(QStringLiteral("hasAudioAndVideo"), isReady ? clip->hasAudioAndVideo() : false);
        if (clip->clipType() == ClipType::Text) {
            const QString xmlData = clip->getProducerProperty(QStringLiteral("xmldata"));
            const QString tText = extractTitleTextsFromXml(xmlData);
            cObj.insert(QStringLiteral("kind"), QStringLiteral("title"));
            if (!tText.isEmpty()) {
                cObj.insert(QStringLiteral("titleText"), tText);
                cObj.insert(QStringLiteral("text"), tText);
            }
            if (!xmlData.isEmpty()) {
                cObj.insert(QStringLiteral("xmldata"), xmlData);
            }
        }
        binArr.append(cObj);
    }
    return binArr;
}

QJsonObject JsonSerialize::serializeTimeline(int rev, int sinceRev)
{
    QJsonObject root;

    KdenliveDoc *doc = pCore->currentDoc();
    if (!doc && pCore->projectManager()) {
        doc = pCore->projectManager()->current();
    }

    std::shared_ptr<TimelineItemModel> timeline;
    if (pCore->projectManager()) {
        timeline = pCore->projectManager()->getTimeline();
    }
    if (!timeline && doc) {
        timeline = doc->getTimeline(doc->uuid(), true);
    }

    const int currentUndoIdx = pCore->undoIndex();
    const int effectiveRev = rev > 0 ? rev : currentUndoIdx;

    if (sinceRev > 0 && effectiveRev > 0 && effectiveRev <= sinceRev) {
        root.insert(QStringLiteral("unchanged"), true);
        root.insert(QStringLiteral("rev"), effectiveRev);
        root.insert(QStringLiteral("undoIndex"), currentUndoIdx);
        return root;
    }

    const double fps = doc ? doc->timecode().fps() : pCore->getCurrentFps();
    const QSize frameSize = pCore->getCurrentFrameSize();
    const int width = doc ? doc->width() : frameSize.width();
    const int height = doc ? doc->height() : frameSize.height();
    const int duration = timeline ? timeline->duration() : pCore->projectDuration();
    const int position = pCore->getMonitorPosition();

    root.insert(QStringLiteral("unchanged"), false);
    root.insert(QStringLiteral("rev"), effectiveRev);
    root.insert(QStringLiteral("undoIndex"), currentUndoIdx);
    root.insert(QStringLiteral("uuid"),
                doc ? doc->uuid().toString(QUuid::WithoutBraces)
                    : pCore->currentTimelineId().toString(QUuid::WithoutBraces));
    root.insert(QStringLiteral("projectPath"), doc ? doc->url().toLocalFile() : QString());
    root.insert(QStringLiteral("fps"), fps);
    root.insert(QStringLiteral("width"), width);
    root.insert(QStringLiteral("height"), height);
    root.insert(QStringLiteral("duration"), duration);
    root.insert(QStringLiteral("position"), position);
    root.insert(QStringLiteral("playhead"), position);

    QJsonArray tracksArr;
    QJsonArray flatClipsArr;

    if (timeline) {
        const std::unordered_set<int> allTracksSet = timeline->getAllTracksIds();
        std::vector<int> sortedTracks(allTracksSet.begin(), allTracksSet.end());
        std::sort(sortedTracks.begin(), sortedTracks.end(), [&timeline](int a, int b) {
            return timeline->getTrackPosition(a) < timeline->getTrackPosition(b);
        });

        for (int tid : sortedTracks) {
            tracksArr.append(serializeTrack(timeline, tid, fps, &flatClipsArr));
        }
    }

    const QJsonArray guidesArr = serializeGuides(timeline, doc, fps);
    const QJsonArray subtitlesArr = serializeSubtitles(timeline, fps);
    const QJsonArray binArr = serializeBin();

    root.insert(QStringLiteral("tracks"), tracksArr);
    root.insert(QStringLiteral("clips"), flatClipsArr);
    root.insert(QStringLiteral("guides"), guidesArr);
    root.insert(QStringLiteral("markers"), guidesArr);
    root.insert(QStringLiteral("subtitles"), subtitlesArr);
    if (timeline && timeline->getSubtitleModel()) {
        auto subModel = timeline->getSubtitleModel();
        subModel->ensureEditorIAPresets();
        QJsonArray stylesArr;
        for (const auto &stEntry : subModel->getAllSubtitleStyles()) {
            QJsonObject stObj;
            stObj.insert(QStringLiteral("name"), stEntry.first);
            stObj.insert(QStringLiteral("fontName"), stEntry.second.fontName());
            stObj.insert(QStringLiteral("fontSize"), stEntry.second.fontSize());
            stObj.insert(QStringLiteral("bold"), stEntry.second.bold());
            stObj.insert(QStringLiteral("borderStyle"), stEntry.second.borderStyle());
            stObj.insert(QStringLiteral("outline"), stEntry.second.outline());
            stObj.insert(QStringLiteral("alignment"), stEntry.second.alignment());
            stObj.insert(QStringLiteral("marginV"), stEntry.second.marginV());
            stylesArr.append(stObj);
        }
        root.insert(QStringLiteral("subtitleStyles"), stylesArr);
    }
    root.insert(QStringLiteral("bin"), binArr);

    return root;
}

} // namespace MEditor
