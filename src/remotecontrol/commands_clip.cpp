/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "rchelpers.h"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QJsonArray>
#include <unordered_set>

namespace {
using Tl = std::shared_ptr<TimelineItemModel>;
const QString kInt = QStringLiteral("int");
const QString kBool = QStringLiteral("bool");

/** Ids of existing timeline items (clips or compositions); any unknown id → not_found. */
std::optional<std::unordered_set<int>> rcItems(const Tl &tl, const QJsonObject &p, const char *key, RcReply &err)
{
    const QJsonValue v = p.value(QString::fromLatin1(key));
    if (!v.isArray() || v.toArray().isEmpty()) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("'%1' must be a non-empty array of ids").arg(QString::fromLatin1(key)));
        return std::nullopt;
    }
    std::unordered_set<int> ids;
    for (const auto &item : v.toArray()) {
        const int id = item.toInt(-1);
        if (!tl->isItem(id)) {
            err = rcFail(QStringLiteral("not_found"), QStringLiteral("no timeline item with id %1").arg(item.toVariant().toString()));
            return std::nullopt;
        }
        ids.insert(id);
    }
    return ids;
}

std::optional<int> rcPosition(const QJsonObject &p, const char *key, RcReply &err)
{
    auto pos = rcInt(p, key, err);
    if (pos && *pos < 0) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("'%1' must be >= 0").arg(QString::fromLatin1(key)));
        return std::nullopt;
    }
    return pos;
}

RcReply lockedError(int tid)
{
    return rcFail(QStringLiteral("failed"), QStringLiteral("track %1 is locked").arg(tid));
}

RcReply refused(const QString &what)
{
    return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused to %1 (overlap, locked track or invalid range)").arg(what));
}

/** Common prologue: timeline + clip id, refusing clips on locked tracks. */
#define RC_CLIP_OR_RETURN(tl, cid)                                                                                                                             \
    RcReply err;                                                                                                                                               \
    auto tl = rcTimeline(err);                                                                                                                                 \
    if (!tl) return err;                                                                                                                                       \
    auto cid = rcClip(tl, p, err);                                                                                                                             \
    if (!cid) return err;                                                                                                                                      \
    if (tl->trackIsLocked(tl->getClipTrackId(*cid))) return lockedError(tl->getClipTrackId(*cid));

RcReply clipInsert(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto binId = rcString(p, "binId", err);
    auto tid = binId ? rcTrack(tl, p, err) : std::nullopt;
    auto pos = tid ? rcPosition(p, "position", err) : std::nullopt;
    if (!pos) return err;
    auto clip = pCore->projectItemModel()->getClipByBinID(*binId);
    if (!clip) return rcFail(QStringLiteral("not_found"), QStringLiteral("no bin clip with id %1").arg(*binId));
    if (!clip->statusReady()) return rcFail(QStringLiteral("failed"), QStringLiteral("clip still loading; retry shortly"));
    if (tl->trackIsLocked(*tid)) return lockedError(*tid);
    int id = -1;
    if (!tl->requestClipInsertion(*binId, *tid, *pos, id, true, true, false)) return refused(QStringLiteral("insert the clip"));
    return rcOk(QJsonObject{{QStringLiteral("clipId"), id}});
}

RcReply clipMove(const QJsonObject &p)
{
    RC_CLIP_OR_RETURN(tl, cid)
    auto tid = rcTrack(tl, p, err);
    auto pos = tid ? rcPosition(p, "position", err) : std::nullopt;
    if (!pos) return err;
    if (tl->trackIsLocked(*tid)) return lockedError(*tid);
    if (!tl->requestClipMove(*cid, *tid, *pos, true, true, true)) return refused(QStringLiteral("move the clip"));
    return rcOk(QJsonObject());
}

RcReply clipResize(const QJsonObject &p)
{
    RC_CLIP_OR_RETURN(tl, cid)
    auto duration = rcInt(p, "duration", err);
    auto fromEnd = duration ? rcBool(p, "fromEnd", err, true) : std::nullopt;
    if (!fromEnd) return err;
    if (*duration <= 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("duration must be > 0"));
    const int size = tl->requestItemResize(*cid, *duration, *fromEnd, true);
    if (size < 0) return refused(QStringLiteral("resize the clip"));
    return rcOk(QJsonObject{{QStringLiteral("duration"), size}});
}

RcReply clipRippleResize(const QJsonObject &p)
{
    RC_CLIP_OR_RETURN(tl, cid)
    auto duration = rcInt(p, "duration", err);
    auto fromEnd = duration ? rcBool(p, "fromEnd", err, true) : std::nullopt;
    if (!fromEnd) return err;
    if (*duration <= 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("duration must be > 0"));
    const int size = tl->requestItemRippleResize(tl, *cid, *duration, *fromEnd, true);
    if (size < 0) return refused(QStringLiteral("ripple-resize the clip"));
    return rcOk(QJsonObject{{QStringLiteral("duration"), size}});
}

RcReply clipSlip(const QJsonObject &p)
{
    RC_CLIP_OR_RETURN(tl, cid)
    auto offset = rcInt(p, "offset", err);
    if (!offset) return err;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    if (!tl->requestClipSlip(*cid, *offset, true, undo, redo)) return refused(QStringLiteral("slip the clip"));
    pCore->pushUndo(undo, redo, QStringLiteral("Slip clip"));
    return rcOk(QJsonObject());
}

RcReply clipDelete(const QJsonObject &p)
{
    RC_CLIP_OR_RETURN(tl, cid)
    if (!tl->requestItemDeletion(*cid, true)) return refused(QStringLiteral("delete the clip"));
    return rcOk(QJsonObject());
}

RcReply clipSpeed(const QJsonObject &p)
{
    RC_CLIP_OR_RETURN(tl, cid)
    auto speed = rcDouble(p, "speed", err);
    auto pitch = speed ? rcBool(p, "pitchCompensate", err, false) : std::nullopt;
    auto change = pitch ? rcBool(p, "changeDuration", err, true) : std::nullopt;
    if (!change) return err;
    if (*speed == 0.) return rcFail(QStringLiteral("bad_params"), QStringLiteral("speed must be non-zero (1.0 = normal, negative = reverse)"));
    if (!tl->requestClipTimeWarp(*cid, *speed, *pitch, *change)) return refused(QStringLiteral("change speed"));
    return rcOk(QJsonObject());
}

RcReply clipEnable(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto ids = rcItems(tl, p, "clipIds", err);
    auto enabled = ids ? rcBool(p, "enabled", err) : std::nullopt;
    if (!enabled) return err;
    if (!TimelineFunctions::setClipsEnabled(tl, *ids, *enabled)) return refused(QStringLiteral("change enabled state"));
    return rcOk(QJsonObject());
}

RcReply clipSplit(const QJsonObject &p, bool audio)
{
    RC_CLIP_OR_RETURN(tl, cid)
    auto target = rcInt(p, "targetTrackId", err, -1);
    if (!target) return err;
    if (*target >= 0 && !tl->isTrack(*target)) return rcFail(QStringLiteral("not_found"), QStringLiteral("no track with id %1").arg(*target));
    const bool done = audio ? TimelineFunctions::requestSplitAudio(tl, *cid, *target >= 0 ? QList<int>{*target} : QList<int>())
                            : TimelineFunctions::requestSplitVideo(tl, *cid, *target);
    if (!done) return refused(audio ? QStringLiteral("split audio") : QStringLiteral("split video"));
    return rcOk(QJsonObject());
}

RcReply clipCopy(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto ids = rcItems(tl, p, "clipIds", err);
    if (!ids) return err;
    return rcOk(QJsonObject{{QStringLiteral("xml"), TimelineFunctions::copyClips(tl, *ids)}});
}

RcReply clipPaste(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto xml = rcString(p, "xml", err);
    auto tid = xml ? rcTrack(tl, p, err) : std::nullopt;
    auto pos = tid ? rcPosition(p, "position", err) : std::nullopt;
    if (!pos) return err;
    if (tl->trackIsLocked(*tid)) return lockedError(*tid);
    if (!TimelineFunctions::pasteClips(tl, *xml, *tid, *pos)) return refused(QStringLiteral("paste"));
    return rcOk(QJsonObject());
}

/** Track ids from 'trackIds', defaulting to every unlocked track. */
std::optional<QVector<int>> rcTrackList(const Tl &tl, const QJsonObject &p, RcReply &err)
{
    QVector<int> tracks;
    const QJsonValue v = p.value(QStringLiteral("trackIds"));
    if (v.isUndefined() || v.isNull()) {
        QList<int> all = tl->getTracksIds(false);
        all << tl->getTracksIds(true);
        for (int tid : std::as_const(all)) {
            if (!tl->trackIsLocked(tid)) tracks << tid;
        }
        return tracks;
    }
    for (const auto &item : v.toArray()) {
        const int tid = item.toInt(-1);
        if (!tl->isTrack(tid)) {
            err = rcFail(QStringLiteral("not_found"), QStringLiteral("no track with id %1").arg(item.toVariant().toString()));
            return std::nullopt;
        }
        tracks << tid;
    }
    return tracks;
}

std::optional<QPoint> rcZone(const QJsonObject &p, const char *startKey, const char *lengthOrEndKey, bool isLength, RcReply &err)
{
    auto a = rcPosition(p, startKey, err);
    auto b = a ? rcInt(p, lengthOrEndKey, err) : std::nullopt;
    if (!b) return std::nullopt;
    const int end = isLength ? *a + *b : *b;
    if (end <= *a) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("empty or negative range"));
        return std::nullopt;
    }
    return QPoint(*a, end);
}

RcReply editExtractZone(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto zone = rcZone(p, "in", "out", false, err);
    auto tracks = zone ? rcTrackList(tl, p, err) : std::nullopt;
    auto lift = tracks ? rcBool(p, "lift", err, false) : std::nullopt;
    if (!lift) return err;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    if (!TimelineFunctions::extractZoneWithUndo(tl, *tracks, *zone, *lift, -1, {}, undo, redo)) return refused(QStringLiteral("extract the zone"));
    pCore->pushUndo(undo, redo, *lift ? QStringLiteral("Lift zone") : QStringLiteral("Extract zone"));
    return rcOk(QJsonObject());
}

RcReply editSpace(const QJsonObject &p, bool insert)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto zone = rcZone(p, "position", "duration", true, err);
    if (!zone) return err;
    QVector<int> allowed;
    if (p.contains(QStringLiteral("trackId"))) {
        auto tid = rcTrack(tl, p, err);
        if (!tid) return err;
        allowed << *tid;
    }
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    const bool done =
        insert ? TimelineFunctions::requestInsertSpace(tl, *zone, undo, redo, allowed) : TimelineFunctions::removeSpace(tl, *zone, undo, redo, allowed, false);
    if (!done) return refused(insert ? QStringLiteral("insert space") : QStringLiteral("remove space"));
    pCore->pushUndo(undo, redo, insert ? QStringLiteral("Insert space") : QStringLiteral("Remove space"));
    return rcOk(QJsonObject());
}

RcReply editDeleteBlank(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto tid = rcTrack(tl, p, err);
    auto pos = tid ? rcPosition(p, "position", err) : std::nullopt;
    if (!pos) return err;
    if (!TimelineFunctions::requestDeleteBlankAt(tl, *tid, *pos, false)) return refused(QStringLiteral("delete the blank"));
    return rcOk(QJsonObject());
}

RcReply groupCreate(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto ids = rcItems(tl, p, "ids", err);
    if (!ids) return err;
    const int gid = tl->requestClipsGroup(*ids, true);
    if (gid < 0) return refused(QStringLiteral("group"));
    return rcOk(QJsonObject{{QStringLiteral("groupId"), gid}});
}

RcReply groupUngroup(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto id = rcInt(p, "id", err);
    if (!id) return err;
    if (!tl->isItem(*id)) return rcFail(QStringLiteral("not_found"), QStringLiteral("no timeline item with id %1").arg(*id));
    if (!tl->requestClipUngroup(*id, true)) return refused(QStringLiteral("ungroup"));
    return rcOk(QJsonObject());
}

RcReply timelineSelect(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto ids = rcItems(tl, p, "ids", err);
    if (!ids) return err;
    tl->requestSetSelection(*ids);
    return rcOk(QJsonObject());
}

void add(RcRegistry &r, const QString &name, const QString &desc, const QList<RcParam> &params, bool mutating, std::function<RcReply(const QJsonObject &)> fn)
{
    r.insert(name, RcCommand{name, desc, params, mutating, true, false, std::move(fn)});
}
} // namespace

void registerClipCommands(RcRegistry &r)
{
    const RcParam clipId{QStringLiteral("clipId"), kInt, true, QString()};
    const RcParam trackId{QStringLiteral("trackId"), kInt, true, QString()};
    const RcParam position{QStringLiteral("position"), kInt, true, QStringLiteral("timeline frame")};
    add(r, QStringLiteral("clip.insert"), QStringLiteral("Insert a ready bin clip on a track at a frame"),
        {{QStringLiteral("binId"), QStringLiteral("string"), true, QString()}, trackId, position}, true, clipInsert);
    add(r, QStringLiteral("clip.move"), QStringLiteral("Move a clip (and its group) to a track and frame"), {clipId, trackId, position}, true, clipMove);
    add(r, QStringLiteral("clip.resize"), QStringLiteral("Set a clip's duration by moving its end (fromEnd=true) or start"),
        {clipId, {QStringLiteral("duration"), kInt, true, QString()}, {QStringLiteral("fromEnd"), kBool, false, QStringLiteral("default true")}}, true,
        clipResize);
    add(r, QStringLiteral("clip.rippleResize"), QStringLiteral("Resize a clip and shift following clips"),
        {clipId, {QStringLiteral("duration"), kInt, true, QString()}, {QStringLiteral("fromEnd"), kBool, false, QStringLiteral("default true")}}, true,
        clipRippleResize);
    add(r, QStringLiteral("clip.slip"), QStringLiteral("Slip the clip's source in/out by offset frames, keeping its position"),
        {clipId, {QStringLiteral("offset"), kInt, true, QString()}}, true, clipSlip);
    add(r, QStringLiteral("clip.delete"), QStringLiteral("Delete a clip"), {clipId}, true, clipDelete);
    add(r, QStringLiteral("clip.speed"), QStringLiteral("Change clip speed (ratio: 1.0 normal, 2.0 double, negative reverse)"),
        {clipId,
         {QStringLiteral("speed"), QStringLiteral("double"), true, QString()},
         {QStringLiteral("pitchCompensate"), kBool, false, QString()},
         {QStringLiteral("changeDuration"), kBool, false, QStringLiteral("default true")}},
        true, clipSpeed);
    add(r, QStringLiteral("clip.enable"), QStringLiteral("Enable or disable clips"),
        {{QStringLiteral("clipIds"), QStringLiteral("array"), true, QString()}, {QStringLiteral("enabled"), kBool, true, QString()}}, true, clipEnable);
    add(r, QStringLiteral("clip.splitAudio"), QStringLiteral("Split the audio of an AV clip to an audio track"),
        {clipId, {QStringLiteral("targetTrackId"), kInt, false, QStringLiteral("default: automatic")}}, true,
        [](const QJsonObject &p) { return clipSplit(p, true); });
    add(r, QStringLiteral("clip.splitVideo"), QStringLiteral("Split the video of an AV clip to a video track"),
        {clipId, {QStringLiteral("targetTrackId"), kInt, false, QStringLiteral("default: automatic")}}, true,
        [](const QJsonObject &p) { return clipSplit(p, false); });
    add(r, QStringLiteral("clip.copy"), QStringLiteral("Serialise clips to XML for clip.paste"),
        {{QStringLiteral("clipIds"), QStringLiteral("array"), true, QString()}}, false, clipCopy);
    add(r, QStringLiteral("clip.paste"), QStringLiteral("Paste XML from clip.copy at a track and frame"),
        {{QStringLiteral("xml"), QStringLiteral("string"), true, QString()}, trackId, position}, true, clipPaste);
    add(r, QStringLiteral("edit.extractZone"), QStringLiteral("Remove [in, out) from tracks; lift=true leaves a gap, false closes it"),
        {{QStringLiteral("in"), kInt, true, QString()},
         {QStringLiteral("out"), kInt, true, QStringLiteral("exclusive")},
         {QStringLiteral("trackIds"), QStringLiteral("array"), false, QStringLiteral("default: all unlocked tracks")},
         {QStringLiteral("lift"), kBool, false, QString()}},
        true, editExtractZone);
    const QList<RcParam> spaceParams{
        {QStringLiteral("trackId"), kInt, false, QStringLiteral("default: all tracks")}, position, {QStringLiteral("duration"), kInt, true, QString()}};
    add(r, QStringLiteral("edit.insertSpace"), QStringLiteral("Insert blank space, pushing clips right"), spaceParams, true,
        [](const QJsonObject &p) { return editSpace(p, true); });
    add(r, QStringLiteral("edit.removeSpace"), QStringLiteral("Remove blank space, pulling clips left"), spaceParams, true,
        [](const QJsonObject &p) { return editSpace(p, false); });
    add(r, QStringLiteral("edit.deleteBlank"), QStringLiteral("Delete the blank at a frame on a track"), {trackId, position}, true, editDeleteBlank);
    add(r, QStringLiteral("group.create"), QStringLiteral("Group timeline items"), {{QStringLiteral("ids"), QStringLiteral("array"), true, QString()}}, true,
        groupCreate);
    add(r, QStringLiteral("group.ungroup"), QStringLiteral("Ungroup the group containing an item"), {{QStringLiteral("id"), kInt, true, QString()}}, true,
        groupUngroup);
    add(r, QStringLiteral("timeline.select"), QStringLiteral("Set the timeline selection"), {{QStringLiteral("ids"), QStringLiteral("array"), true, QString()}},
        false, timelineSelect);
}
