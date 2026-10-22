/*
    SPDX-FileCopyrightText: 2026 MEditor Project
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <memory>

class TimelineItemModel;
class KdenliveDoc;

namespace MEditor {

class JsonSerialize
{
public:
    /**
     * Serializa o estado completo da timeline ativa para `timeline.get`.
     *
     * Campos incluídos:
     * - rev, undoIndex, uuid, projectPath, fps, width, height, duration, position
     * - tracks (trackId, tag, name, position, mltIndex, audio, subtitle, locked, clips)
     * - clips (clipId, id, binId, trackId, track, position, pos, in, out, playtime, duration,
     *          name, speed, isAudio, sourcePath, meditor_uid, uid, keepIndex, reason, kind, properties)
     * - guides / markers (frame, pos, seconds, comment, text, type)
     * - subtitles (id, layer, startFrame, endFrame, in, out, startSeconds, endSeconds, text)
     * - bin (binId, name, sourcePath, duration, type, hasAudioAndVideo)
     *
     * Se `sinceRev > 0` e `rev > 0 && rev <= sinceRev`, retorna `{ "unchanged": true, "rev": rev }`.
     */
    static QJsonObject serializeTimeline(int rev = 0, int sinceRev = 0);

    /**
     * Serializa um único clip da timeline dado seu `clipId`.
     */
    static QJsonObject serializeClip(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, double fps);

    /**
     * Serializa uma trilha (e opcionalmente seus clips) dado seu `trackId`.
     */
    static QJsonObject serializeTrack(const std::shared_ptr<TimelineItemModel> &timeline, int trackId, double fps,
                                      QJsonArray *outFlatClips = nullptr);

    /**
     * Serializa guias/marcadores do modelo ativo.
     */
    static QJsonArray serializeGuides(const std::shared_ptr<TimelineItemModel> &timeline, KdenliveDoc *doc, double fps);

    /**
     * Serializa legendas do `SubtitleModel` ativo.
     */
    static QJsonArray serializeSubtitles(const std::shared_ptr<TimelineItemModel> &timeline, double fps);

    /**
     * Serializa os clips do Bin (`ProjectItemModel`).
     */
    static QJsonArray serializeBin();

    /**
     * Resolve um identificador de trilha (ex.: int `trackId`, ou nome/tag `"V1"`, `"A1"`, `"V2"`)
     * para o `trackId` interno do `TimelineItemModel`. Retorna -1 se não encontrar.
     */
    static int resolveTrackId(const std::shared_ptr<TimelineItemModel> &timeline, const QJsonValue &trackVal,
                              bool preferAudio = false);
};

inline QJsonObject serializeTimeline(int rev = 0, int sinceRev = 0)
{
    return JsonSerialize::serializeTimeline(rev, sinceRev);
}

} // namespace MEditor

using MEditorJsonSerialize = MEditor::JsonSerialize;
