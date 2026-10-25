/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "core.h"
#include "rchelpers.h"
#include "timeline2/model/timelineitemmodel.hpp"

namespace {
const QString kInt = QStringLiteral("int");
const QString kBool = QStringLiteral("bool");
const RcParam kTrackId{QStringLiteral("trackId"), kInt, true, QString()};

#define RC_TRACK_OR_RETURN(tl, tid)                                                                                                                            \
    RcReply err;                                                                                                                                               \
    auto tl = rcTimeline(err);                                                                                                                                 \
    if (!tl) return err;                                                                                                                                       \
    auto tid = rcTrack(tl, p, err);                                                                                                                            \
    if (!tid) return err;

/** Set the MLT "hide" property with an undo entry (Kdenlive's GUI path for this opens no dialog but lives in the QML controller). */
RcReply setHide(const std::shared_ptr<TimelineItemModel> &tl, int tid, const QString &value, const QString &label)
{
    const QString old = tl->getTrackProperty(tid, QStringLiteral("hide")).toString();
    std::weak_ptr<TimelineItemModel> weak = tl;
    Fun redo = [weak, tid, value]() {
        if (auto t = weak.lock()) t->setTrackProperty(tid, QStringLiteral("hide"), value);
        return true;
    };
    Fun undo = [weak, tid, old]() {
        if (auto t = weak.lock()) t->setTrackProperty(tid, QStringLiteral("hide"), old);
        return true;
    };
    redo();
    pCore->pushUndo(undo, redo, label);
    return rcOk(QJsonObject());
}

RcReply trackAdd(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto audio = rcBool(p, "audio", err);
    // audio tracks live below video ones (Kdenlive's mirror-track logic relies on it): default audio to the top of the audio block
    auto pos = audio ? rcInt(p, "position", err, *audio ? int(tl->getTracksIds(true).size()) : -1) : std::nullopt;
    auto name = pos ? rcString(p, "name", err, QString()) : std::nullopt;
    if (!name) return err;
    int id = -1;
    if (!tl->requestTrackInsertion(*pos, id, *name, *audio)) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused to add the track"));
    return rcOk(QJsonObject{{QStringLiteral("trackId"), id}});
}

RcReply trackDelete(const QJsonObject &p)
{
    RC_TRACK_OR_RETURN(tl, tid)
    if (!tl->requestTrackDeletion(*tid)) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused to delete the track"));
    return rcOk(QJsonObject());
}

RcReply trackRename(const QJsonObject &p)
{
    RC_TRACK_OR_RETURN(tl, tid)
    auto name = rcString(p, "name", err);
    if (!name) return err;
    tl->setTrackName(*tid, *name);
    return rcOk(QJsonObject());
}

RcReply trackLock(const QJsonObject &p)
{
    RC_TRACK_OR_RETURN(tl, tid)
    auto locked = rcBool(p, "locked", err);
    if (!locked) return err;
    tl->setTrackLockedState(*tid, *locked);
    return rcOk(QJsonObject());
}

RcReply trackHide(const QJsonObject &p)
{
    RC_TRACK_OR_RETURN(tl, tid)
    auto hidden = rcBool(p, "hidden", err);
    if (!hidden) return err;
    if (tl->isAudioTrack(*tid)) return rcFail(QStringLiteral("bad_params"), QStringLiteral("track %1 is an audio track; use track.mute").arg(*tid));
    return setHide(tl, *tid, *hidden ? QStringLiteral("3") : QStringLiteral("2"), *hidden ? QStringLiteral("Hide track") : QStringLiteral("Show track"));
}

RcReply trackMute(const QJsonObject &p)
{
    RC_TRACK_OR_RETURN(tl, tid)
    auto muted = rcBool(p, "muted", err);
    if (!muted) return err;
    if (!tl->isAudioTrack(*tid)) return rcFail(QStringLiteral("bad_params"), QStringLiteral("track %1 is a video track; use track.hide").arg(*tid));
    return setHide(tl, *tid, *muted ? QStringLiteral("3") : QStringLiteral("1"), *muted ? QStringLiteral("Mute track") : QStringLiteral("Unmute track"));
}

RcReply trackMove(const QJsonObject &p)
{
    RC_TRACK_OR_RETURN(tl, tid)
    auto up = rcBool(p, "up", err);
    if (!up) return err;
    if (!tl->requestTrackMove(tl, *tid, *up)) return rcFail(QStringLiteral("failed"), QStringLiteral("cannot move track %1 further").arg(*tid));
    return rcOk(QJsonObject());
}

void add(RcRegistry &r, const QString &name, const QString &desc, const QList<RcParam> &params, std::function<RcReply(const QJsonObject &)> fn)
{
    r.insert(name, RcCommand{name, desc, params, true, true, false, std::move(fn)});
}
} // namespace

void registerTrackCommands(RcRegistry &r)
{
    add(r, QStringLiteral("track.add"), QStringLiteral("Add a track"),
        {{QStringLiteral("audio"), kBool, true, QString()},
         {QStringLiteral("position"), kInt, false, QStringLiteral("index from the bottom; default: top of the audio block / top of the timeline")},
         {QStringLiteral("name"), QStringLiteral("string"), false, QString()}},
        trackAdd);
    add(r, QStringLiteral("track.delete"), QStringLiteral("Delete a track and its clips"), {kTrackId}, trackDelete);
    add(r, QStringLiteral("track.rename"), QStringLiteral("Rename a track"), {kTrackId, {QStringLiteral("name"), QStringLiteral("string"), true, QString()}},
        trackRename);
    add(r, QStringLiteral("track.lock"), QStringLiteral("Lock or unlock a track"), {kTrackId, {QStringLiteral("locked"), kBool, true, QString()}}, trackLock);
    add(r, QStringLiteral("track.hide"), QStringLiteral("Hide or show a video track"), {kTrackId, {QStringLiteral("hidden"), kBool, true, QString()}},
        trackHide);
    add(r, QStringLiteral("track.mute"), QStringLiteral("Mute or unmute an audio track"), {kTrackId, {QStringLiteral("muted"), kBool, true, QString()}},
        trackMute);
    add(r, QStringLiteral("track.move"), QStringLiteral("Move a track up or down"), {kTrackId, {QStringLiteral("up"), kBool, true, QString()}}, trackMove);
}
