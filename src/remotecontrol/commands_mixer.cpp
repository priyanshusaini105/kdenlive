/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "audiomixer/mixermanager.hpp"
#include "core.h"
#include "rchelpers.h"
#include "remotecontrol.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"

#include <QJsonArray>
#include <mlt++/MltFilter.h>
#include <mlt++/MltTractor.h>

namespace {
/** The mixer's hidden filter (volume or panner) on a track/master tractor, created like MixerWidget does when absent. */
std::shared_ptr<Mlt::Filter> mixerFilter(Mlt::Service &service, const char *name, bool create)
{
    for (int i = 0; i < service.filter_count(); ++i) {
        std::shared_ptr<Mlt::Filter> fl(service.filter(i));
        if (fl->get_int("internal_added") == 237 && qstrcmp(fl->get("mlt_service"), name) == 0) return fl;
    }
    if (!create) return nullptr;
    auto fl = std::make_shared<Mlt::Filter>(*service.profile(), name);
    if (!fl->is_valid()) return nullptr;
    fl->set("internal_added", 237);
    fl->set("disable", 1);
    if (qstrcmp(name, "panner") == 0) fl->set("start", 0.5);
    service.attach(*fl);
    return fl;
}

/** trackId -1 = master; otherwise an audio track. */
Mlt::Service *mixerService(const std::shared_ptr<TimelineItemModel> &tl, int tid, RcReply &err)
{
    if (tid == -1) return tl->tractor();
    if (!tl->isTrack(tid)) {
        err = rcFail(QStringLiteral("not_found"), QStringLiteral("no track with id %1").arg(tid));
        return nullptr;
    }
    if (!tl->isAudioTrack(tid)) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("track %1 is a video track; the mixer is for audio tracks and master (-1)").arg(tid));
        return nullptr;
    }
    return RemoteControl::track(tl, tid)->getTrackService();
}

QJsonObject strip(const std::shared_ptr<TimelineItemModel> &tl, int tid, Mlt::Service &service)
{
    auto level = mixerFilter(service, "volume", false);
    auto pan = mixerFilter(service, "panner", false);
    const int hide = tid == -1 ? service.get_int("hide") : tl->getTrackProperty(tid, QStringLiteral("hide")).toInt();
    return {{QStringLiteral("trackId"), tid},
            {QStringLiteral("volumeDb"), level && !level->get_int("disable") ? level->get_double("level") : 0.},
            {QStringLiteral("pan"), pan && !pan->get_int("disable") ? pan->get_double("start") : 0.5},
            {QStringLiteral("muted"), bool(hide & 2)}};
}

RcReply mixerGet(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    QList<int> tracks;
    if (p.contains(QStringLiteral("trackId"))) {
        auto tid = rcInt(p, "trackId", err);
        if (!tid) return err;
        tracks << *tid;
    } else {
        tracks = tl->getTracksIds(true);
        tracks << -1;
    }
    QJsonArray out;
    for (int tid : std::as_const(tracks)) {
        Mlt::Service *service = mixerService(tl, tid, err);
        if (!service) return err;
        out.append(strip(tl, tid, *service));
    }
    return rcOk(out);
}

RcReply mixerSet(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto tid = rcInt(p, "trackId", err);
    if (!tid) return err;
    Mlt::Service *service = mixerService(tl, *tid, err);
    if (!service) return err;
    std::optional<double> db, pan;
    if (p.contains(QStringLiteral("volumeDb"))) {
        db = rcDouble(p, "volumeDb", err);
        if (!db) return err;
        if (*db < -100 || *db > 24) return rcFail(QStringLiteral("bad_params"), QStringLiteral("volumeDb must be -100..24"));
    }
    if (p.contains(QStringLiteral("pan"))) {
        pan = rcDouble(p, "pan", err);
        if (!pan) return err;
        if (*pan < 0 || *pan > 1) return rcFail(QStringLiteral("bad_params"), QStringLiteral("pan must be 0 (left) .. 1 (right); 0.5 is center"));
    }
    if (!db && !pan) return rcFail(QStringLiteral("bad_params"), QStringLiteral("pass volumeDb and/or pan"));
    if (db) {
        auto level = mixerFilter(*service, "volume", true);
        level->set("level", *db);
        level->set("disable", qFuzzyIsNull(*db) ? 1 : 0);
    }
    if (pan) {
        auto panner = mixerFilter(*service, "panner", true);
        panner->set("start", *pan);
        panner->set("disable", qFuzzyCompare(*pan, 0.5) ? 1 : 0);
    }
    pCore->setDocumentModified();
    return rcOk(QJsonObject());
}

RcReply mixerSolo(const QJsonObject &p)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto tid = rcTrack(tl, p, err);
    if (!tid) return err;
    if (!tl->isAudioTrack(*tid)) return rcFail(QStringLiteral("bad_params"), QStringLiteral("solo applies to audio tracks"));
    pCore->mixer()->slotSwitchSoloMode(*tid);
    return rcOk(QJsonObject());
}
} // namespace

void registerMixerCommands(RcRegistry &r)
{
    const RcParam trackId{QStringLiteral("trackId"), QStringLiteral("int"), true, QStringLiteral("audio track, or -1 for master")};
    r.insert(QStringLiteral("mixer.get"), {QStringLiteral("mixer.get"),
                                           QStringLiteral("Mixer strips (volume dB, pan 0..1, muted) for audio tracks and master (-1)"),
                                           {{QStringLiteral("trackId"), QStringLiteral("int"), false, QStringLiteral("default: all")}},
                                           false,
                                           true,
                                           false,
                                           mixerGet});
    r.insert(QStringLiteral("mixer.set"), {QStringLiteral("mixer.set"),
                                           QStringLiteral("Set track/master volume (dB) and pan (0..1); not undoable, the mixer panel refreshes on reopen"),
                                           {trackId,
                                            {QStringLiteral("volumeDb"), QStringLiteral("double"), false, QString()},
                                            {QStringLiteral("pan"), QStringLiteral("double"), false, QString()}},
                                           true,
                                           false,
                                           false,
                                           mixerSet});
    r.insert(QStringLiteral("mixer.solo"),
             {QStringLiteral("mixer.solo"), QStringLiteral("Toggle solo on an audio track"), {trackId}, true, false, true, mixerSolo});
}
