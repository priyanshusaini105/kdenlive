/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "core.h"
#include "profiles/profilemodel.hpp"
#include "rchelpers.h"
#include "remotecontrol.h"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"

#include <QJsonArray>
#include <algorithm>

namespace {
const QString kInt = QStringLiteral("int");

RcReply projectInfo(const QJsonObject &)
{
    auto &profile = pCore->getCurrentProfile();
    RcReply err;
    auto tl = rcTimeline(err);
    return rcOk(QJsonObject{{QStringLiteral("fps"), profile->fps()},
                            {QStringLiteral("fpsNum"), profile->frame_rate_num()},
                            {QStringLiteral("fpsDen"), profile->frame_rate_den()},
                            {QStringLiteral("width"), profile->width()},
                            {QStringLiteral("height"), profile->height()},
                            {QStringLiteral("profile"), profile->path()},
                            {QStringLiteral("duration"), tl ? tl->duration() : 0}});
}

QJsonObject trackJson(const std::shared_ptr<TimelineItemModel> &tl, int tid)
{
    const bool audio = tl->isAudioTrack(tid);
    const int hide = tl->getTrackProperty(tid, QStringLiteral("hide")).toInt();
    return {{QStringLiteral("id"), tid},
            {QStringLiteral("name"), tl->getTrackFullName(tid)},
            {QStringLiteral("audio"), audio},
            {QStringLiteral("locked"), tl->trackIsLocked(tid)},
            {QStringLiteral("hidden"), !audio && (hide & 1)},
            {QStringLiteral("muted"), audio && (hide & 2)},
            {QStringLiteral("position"), tl->getTrackPosition(tid)}};
}

RcReply timelineGet(const QJsonObject &)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    QList<int> trackIds = tl->getTracksIds(false);
    trackIds << tl->getTracksIds(true);

    QJsonArray tracks;
    QList<QJsonObject> clips;
    QList<QJsonObject> compositions;
    for (int tid : std::as_const(trackIds)) {
        tracks.append(trackJson(tl, tid));
        for (int id : tl->getItemsInRange(tid, 0, -1, true)) {
            if (tl->isClip(id)) {
                const int in = tl->getClipIn(id);
                const int playtime = tl->getClipPlaytime(id);
                clips << QJsonObject{{QStringLiteral("id"), id},
                                     {QStringLiteral("trackId"), tid},
                                     {QStringLiteral("binId"), tl->getClipBinId(id)},
                                     {QStringLiteral("name"), tl->getClipName(id)},
                                     {QStringLiteral("position"), tl->getClipPosition(id)},
                                     {QStringLiteral("in"), in},
                                     {QStringLiteral("out"), in + playtime - 1},
                                     {QStringLiteral("duration"), playtime},
                                     {QStringLiteral("speed"), tl->getClipSpeed(id)}};
            } else if (tl->isComposition(id)) {
                auto params = tl->getCompositionParameterModel(id);
                compositions << QJsonObject{{QStringLiteral("id"), id},
                                            {QStringLiteral("trackId"), tid},
                                            {QStringLiteral("aTrack"), pCore->getCompositionATrack(id).first},
                                            {QStringLiteral("assetId"), params ? params->getAssetId() : QString()},
                                            {QStringLiteral("position"), tl->getCompositionPosition(id)},
                                            {QStringLiteral("duration"), tl->getCompositionPlaytime(id)}};
            }
        }
    }
    auto byTrackThenPos = [&trackIds](const QJsonObject &a, const QJsonObject &b) {
        const auto ta = trackIds.indexOf(a.value(QStringLiteral("trackId")).toInt());
        const auto tb = trackIds.indexOf(b.value(QStringLiteral("trackId")).toInt());
        return ta != tb ? ta < tb : a.value(QStringLiteral("position")).toInt() < b.value(QStringLiteral("position")).toInt();
    };
    std::sort(clips.begin(), clips.end(), byTrackThenPos);
    std::sort(compositions.begin(), compositions.end(), byTrackThenPos);

    QJsonArray mixes;
    for (const auto &o : std::as_const(clips)) {
        const int cid = o.value(QStringLiteral("id")).toInt();
        auto track = RemoteControl::track(tl, o.value(QStringLiteral("trackId")).toInt());
        if (tl->getMixInOut(cid).first > -1) {
            auto model = track->mixModel(cid);
            mixes.append(QJsonObject{{QStringLiteral("clipId"), cid},
                                     {QStringLiteral("previousClipId"), track->getMixInfo(cid).first.firstClipId},
                                     {QStringLiteral("trackId"), o.value(QStringLiteral("trackId"))},
                                     {QStringLiteral("assetId"), model ? model->getAssetId() : QString()},
                                     {QStringLiteral("duration"), tl->getMixDuration(cid)}});
        }
    }
    QJsonArray selection;
    for (int id : tl->getCurrentSelection()) {
        selection.append(id);
    }
    QJsonArray clipArray;
    for (const auto &o : std::as_const(clips)) {
        clipArray.append(o);
    }
    QJsonArray compositionArray;
    for (const auto &o : std::as_const(compositions)) {
        compositionArray.append(o);
    }
    return rcOk(QJsonObject{{QStringLiteral("fps"), pCore->getCurrentFps()},
                            {QStringLiteral("duration"), tl->duration()},
                            {QStringLiteral("playhead"), pCore->window() ? QJsonValue(pCore->getMonitorPosition()) : QJsonValue()},
                            {QStringLiteral("tracks"), tracks},
                            {QStringLiteral("clips"), clipArray},
                            {QStringLiteral("compositions"), compositionArray},
                            {QStringLiteral("mixes"), mixes},
                            {QStringLiteral("selection"), selection}});
}

RcReply clipCut(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto cid = rcClip(tl, p, err);
    if (!cid) return err;
    auto pos = rcInt(p, "position", err);
    if (!pos) return err;
    const int start = tl->getClipPosition(*cid);
    const int end = start + tl->getClipPlaytime(*cid);
    if (*pos <= start || *pos >= end) {
        return rcFail(QStringLiteral("bad_params"),
                      QStringLiteral("position %1 is not strictly inside clip %2 (%3-%4)").arg(*pos).arg(*cid).arg(start).arg(end));
    }
    const int tid = tl->getClipTrackId(*cid);
    if (tl->trackIsLocked(tid)) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("track %1 is locked").arg(tid));
    }
    if (!TimelineFunctions::requestClipCut(tl, *cid, *pos)) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the cut"));
    }
    return rcOk(QJsonObject{{QStringLiteral("leftId"), *cid}, {QStringLiteral("rightId"), tl->getClipByStartPosition(tid, *pos)}});
}
} // namespace

void registerTimelineCommands(RcRegistry &r)
{
    r.insert(QStringLiteral("project.info"),
             {QStringLiteral("project.info"), QStringLiteral("Project profile (fps, size) and timeline duration"), {}, false, true, false, projectInfo});
    r.insert(QStringLiteral("timeline.get"), {QStringLiteral("timeline.get"),
                                              QStringLiteral("Tracks (top to bottom), clips, compositions, playhead and selection of the active timeline"),
                                              {},
                                              false,
                                              true,
                                              false,
                                              timelineGet});
    r.insert(QStringLiteral("clip.cut"),
             {QStringLiteral("clip.cut"),
              QStringLiteral("Razor-cut a clip at an absolute timeline frame strictly inside it"),
              {{QStringLiteral("clipId"), kInt, true, QString()}, {QStringLiteral("position"), kInt, true, QStringLiteral("timeline frame")}},
              true,
              true,
              false,
              clipCut});
}
