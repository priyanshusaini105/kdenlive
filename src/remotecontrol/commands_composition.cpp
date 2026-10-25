/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "assets/model/assetparametermodel.hpp"
#include "core.h"
#include "rchelpers.h"
#include "remotecontrol.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"
#include "transitions/transitionsrepository.hpp"

#include <mlt++/MltProperties.h>

namespace {
const QString kInt = QStringLiteral("int");

std::optional<int> rcComposition(const std::shared_ptr<TimelineItemModel> &tl, const QJsonObject &p, RcReply &err)
{
    auto id = rcInt(p, "compositionId", err);
    if (!id) return std::nullopt;
    if (!tl->isComposition(*id)) {
        err = rcFail(QStringLiteral("not_found"), QStringLiteral("no composition with id %1").arg(*id));
        return std::nullopt;
    }
    return id;
}

#define RC_COMPO_OR_RETURN(tl, cid)                                                                                                                            \
    RcReply err;                                                                                                                                               \
    auto tl = rcTimeline(err);                                                                                                                                 \
    if (!tl) return err;                                                                                                                                       \
    auto cid = rcComposition(tl, p, err);                                                                                                                      \
    if (!cid) return err;

RcReply compositionAdd(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto id = rcString(p, "transitionId", err);
    auto tid = id ? rcTrack(tl, p, err) : std::nullopt;
    auto pos = tid ? rcInt(p, "position", err) : std::nullopt;
    auto duration = pos ? rcInt(p, "duration", err) : std::nullopt;
    auto aTrack = duration ? rcInt(p, "aTrack", err, -1) : std::nullopt;
    if (!aTrack) return err;
    if (!TransitionsRepository::get()->exists(*id) || !TransitionsRepository::get()->isComposition(*id)) {
        return rcFail(QStringLiteral("not_found"), QStringLiteral("no composition '%1'; see asset.list kind=transition (composition=true)").arg(*id));
    }
    if (*pos < 0 || *duration <= 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("need position >= 0 and duration > 0"));
    if (tl->trackIsLocked(*tid)) return rcFail(QStringLiteral("failed"), QStringLiteral("track %1 is locked").arg(*tid));
    if (*aTrack >= tl->getTrackPosition(*tid)) {
        // getTrackIndexFromPosition only Q_ASSERTs its bounds: an out-of-range index would walk past the track list
        return rcFail(QStringLiteral("bad_params"), QStringLiteral("aTrack must be the MLT index of a lower track (0-%1)").arg(tl->getTrackPosition(*tid) - 1));
    }
    int cid = -1;
    bool done;
    if (*aTrack >= 0) {
        // aTrack is the MLT index of the track composited onto (see timeline.get tracks[].position)
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        done = tl->requestCompositionInsertion(*id, *tid, *aTrack, *pos, *duration, nullptr, cid, undo, redo, true);
        if (done) pCore->pushUndo(undo, redo, QStringLiteral("Insert composition"));
    } else {
        done = tl->requestCompositionInsertion(*id, *tid, *pos, *duration, nullptr, cid, true);
    }
    if (!done) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the composition (overlap or no track below?)"));
    return rcOk(QJsonObject{{QStringLiteral("compositionId"), cid}});
}

RcReply compositionDelete(const QJsonObject &p)
{
    RC_COMPO_OR_RETURN(tl, cid)
    if (!tl->requestItemDeletion(*cid, true)) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused to delete the composition"));
    return rcOk(QJsonObject());
}

RcReply compositionMove(const QJsonObject &p)
{
    RC_COMPO_OR_RETURN(tl, cid)
    auto tid = rcTrack(tl, p, err);
    auto pos = tid ? rcInt(p, "position", err) : std::nullopt;
    if (!pos) return err;
    if (!tl->requestCompositionMove(*cid, *tid, *pos, true, true)) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the move"));
    return rcOk(QJsonObject());
}

RcReply compositionResize(const QJsonObject &p)
{
    RC_COMPO_OR_RETURN(tl, cid)
    auto duration = rcInt(p, "duration", err);
    auto fromEnd = duration ? rcBool(p, "fromEnd", err, true) : std::nullopt;
    if (!fromEnd) return err;
    if (*duration <= 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("duration must be > 0"));
    const int size = tl->requestItemResize(*cid, *duration, *fromEnd, true);
    if (size < 0) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the resize"));
    return rcOk(QJsonObject{{QStringLiteral("duration"), size}});
}

QJsonObject assetJson(const std::shared_ptr<AssetParameterModel> &m)
{
    return {{QStringLiteral("assetId"), m->getAssetId()},
            {QStringLiteral("params"), rcAssetParams(m.get(), TransitionsRepository::get()->getXml(m->getAssetId()))}};
}

RcReply compositionGet(const QJsonObject &p)
{
    RC_COMPO_OR_RETURN(tl, cid)
    return rcOk(assetJson(tl->getCompositionParameterModel(*cid)));
}

RcReply compositionSet(const QJsonObject &p)
{
    RC_COMPO_OR_RETURN(tl, cid)
    return rcSetAssetParams(tl->getCompositionParameterModel(*cid), p.value(QStringLiteral("params")).toObject());
}

/** A clip that starts with a same-track mix (mixes are keyed by their right-hand clip). */
std::shared_ptr<AssetParameterModel> mixAt(const std::shared_ptr<TimelineItemModel> &tl, int cid, RcReply &err)
{
    auto track = RemoteControl::track(tl, tl->getClipTrackId(cid));
    auto model = tl->getMixInOut(cid).first > -1 ? track->mixModel(cid) : nullptr;
    if (!model) err = rcFail(QStringLiteral("not_found"), QStringLiteral("clip %1 does not start with a mix").arg(cid));
    return model;
}

RcReply mixAdd(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto cid = rcClip(tl, p, err);
    auto id = cid ? rcString(p, "transitionId", err, QStringLiteral("luma")) : std::nullopt;
    if (!id) return err;
    if (!TransitionsRepository::get()->exists(*id)) return rcFail(QStringLiteral("not_found"), QStringLiteral("no transition '%1'").arg(*id));
    // delta -1: mix with the clip ending where this one starts
    if (!tl->mixClip(*cid, *id, -1)) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("no adjacent clip before %1, or not enough extra frames for the mix").arg(*cid));
    }
    return rcOk(QJsonObject());
}

RcReply mixRemove(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto cid = rcClip(tl, p, err);
    if (!cid || !mixAt(tl, *cid, err)) return err;
    if (!tl->removeMix(*cid)) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused to remove the mix"));
    return rcOk(QJsonObject());
}

RcReply mixGet(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto cid = rcClip(tl, p, err);
    auto model = cid ? mixAt(tl, *cid, err) : nullptr;
    if (!model) return err;
    return rcOk(assetJson(model));
}

RcReply mixSet(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto cid = rcClip(tl, p, err);
    auto model = cid ? mixAt(tl, *cid, err) : nullptr;
    if (!model) return err;
    return rcSetAssetParams(model, p.value(QStringLiteral("params")).toObject());
}

const RcParam kCompo{QStringLiteral("compositionId"), kInt, true, QString()};
const RcParam kClip{QStringLiteral("clipId"), kInt, true, QStringLiteral("the clip on the right of the mix")};
const RcParam kParams{QStringLiteral("params"), QStringLiteral("object"), true, QStringLiteral("name: value")};

void add(RcRegistry &r, const QString &name, const QString &desc, const QList<RcParam> &params, bool mutating, std::function<RcReply(const QJsonObject &)> fn)
{
    r.insert(name, RcCommand{name, desc, params, mutating, true, false, std::move(fn)});
}
} // namespace

void registerCompositionCommands(RcRegistry &r)
{
    add(r, QStringLiteral("composition.add"), QStringLiteral("Add a composition on a track (composites onto the track below, or aTrack)"),
        {{QStringLiteral("transitionId"), QStringLiteral("string"), true, QStringLiteral("e.g. composite, qtblend, wipe")},
         {QStringLiteral("trackId"), kInt, true, QString()},
         {QStringLiteral("position"), kInt, true, QString()},
         {QStringLiteral("duration"), kInt, true, QString()},
         {QStringLiteral("aTrack"), kInt, false, QStringLiteral("MLT index of the lower track")}},
        true, compositionAdd);
    add(r, QStringLiteral("composition.delete"), QStringLiteral("Delete a composition"), {kCompo}, true, compositionDelete);
    add(r, QStringLiteral("composition.move"), QStringLiteral("Move a composition"),
        {kCompo, {QStringLiteral("trackId"), kInt, true, QString()}, {QStringLiteral("position"), kInt, true, QString()}}, true, compositionMove);
    add(r, QStringLiteral("composition.resize"), QStringLiteral("Resize a composition"),
        {kCompo, {QStringLiteral("duration"), kInt, true, QString()}, {QStringLiteral("fromEnd"), QStringLiteral("bool"), false, QString()}}, true,
        compositionResize);
    add(r, QStringLiteral("composition.get"), QStringLiteral("Composition asset and parameters"), {kCompo}, false, compositionGet);
    add(r, QStringLiteral("composition.set"), QStringLiteral("Set composition parameters"), {kCompo, kParams}, true, compositionSet);
    add(r, QStringLiteral("mix.add"), QStringLiteral("Same-track crossfade between a clip and the clip ending at its start"),
        {{QStringLiteral("clipId"), kInt, true, QStringLiteral("right-hand clip")},
         {QStringLiteral("transitionId"), QStringLiteral("string"), false, QStringLiteral("default luma (dissolve)")}},
        true, mixAdd);
    add(r, QStringLiteral("mix.remove"), QStringLiteral("Remove the mix at a clip's start"), {kClip}, true, mixRemove);
    add(r, QStringLiteral("mix.get"), QStringLiteral("Mix asset and parameters"), {kClip}, false, mixGet);
    add(r, QStringLiteral("mix.set"), QStringLiteral("Set mix parameters"), {kClip, kParams}, true, mixSet);
}
