/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "bin/model/markerlistmodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "mainwindow.h"
#include "rchelpers.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include <QJsonArray>

namespace {
const QString kInt = QStringLiteral("int");

/** {scope: "guides"} → active timeline guides; {scope: "clip", binId} → that bin clip's markers. */
std::shared_ptr<MarkerListModel> markerModel(const QJsonObject &p, RcReply &err)
{
    auto scope = rcString(p, "scope", err, QStringLiteral("guides"));
    if (!scope) return nullptr;
    if (*scope == QLatin1String("guides")) {
        auto tl = rcTimeline(err);
        return tl ? tl->getGuideModel() : nullptr;
    }
    if (*scope == QLatin1String("clip")) {
        auto binId = rcString(p, "binId", err);
        if (!binId) return nullptr;
        auto clip = pCore->projectItemModel()->getClipByBinID(*binId);
        if (!clip) {
            err = rcFail(QStringLiteral("not_found"), QStringLiteral("no bin clip with id %1").arg(*binId));
            return nullptr;
        }
        return clip->getMarkerModel();
    }
    err = rcFail(QStringLiteral("bad_params"), QStringLiteral("scope must be 'guides' or 'clip'"));
    return nullptr;
}

GenTime frameTime(int frames)
{
    return GenTime(frames, pCore->getCurrentFps());
}

int toFrames(const GenTime &t)
{
    return t.frames(pCore->getCurrentFps());
}

RcReply markerList(const QJsonObject &p)
{
    RcReply err;
    auto model = markerModel(p, err);
    if (!model) return err;
    QJsonArray out;
    for (const CommentedTime &m : model->getAllMarkers()) {
        out.append(QJsonObject{{QStringLiteral("position"), toFrames(m.time())},
                               {QStringLiteral("comment"), m.comment()},
                               {QStringLiteral("category"), m.markerType()},
                               {QStringLiteral("duration"), m.hasRange() ? toFrames(m.duration()) : 0}});
    }
    return rcOk(out);
}

RcReply markerAdd(const QJsonObject &p)
{
    RcReply err;
    auto model = markerModel(p, err);
    auto pos = model ? rcInt(p, "position", err) : std::nullopt;
    auto comment = pos ? rcString(p, "comment", err, QString()) : std::nullopt;
    auto category = comment ? rcInt(p, "category", err, 0) : std::nullopt;
    auto duration = category ? rcInt(p, "duration", err, 0) : std::nullopt;
    if (!duration) return err;
    if (*pos < 0 || *duration < 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("position and duration must be >= 0"));
    const bool done = *duration > 0 ? model->addRangeMarker(frameTime(*pos), frameTime(*duration), *comment, *category)
                                    : model->addMarker(frameTime(*pos), *comment, *category);
    if (!done) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused to add the marker"));
    return rcOk(QJsonObject());
}

RcReply markerEdit(const QJsonObject &p)
{
    RcReply err;
    auto model = markerModel(p, err);
    auto pos = model ? rcInt(p, "position", err) : std::nullopt;
    if (!pos) return err;
    bool exists = false;
    const CommentedTime old = model->getMarker(frameTime(*pos), &exists);
    if (!exists) return rcFail(QStringLiteral("not_found"), QStringLiteral("no marker at frame %1").arg(*pos));
    auto newPos = rcInt(p, "newPosition", err, *pos);
    auto comment = newPos ? rcString(p, "comment", err, old.comment()) : std::nullopt;
    auto category = comment ? rcInt(p, "category", err, old.markerType()) : std::nullopt;
    auto duration = category ? rcInt(p, "duration", err, old.hasRange() ? toFrames(old.duration()) : 0) : std::nullopt;
    if (!duration) return err;
    if (!model->editMarker(frameTime(*pos), frameTime(*newPos), *comment, *category, frameTime(*duration))) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused to edit the marker"));
    }
    return rcOk(QJsonObject());
}

RcReply markerRemove(const QJsonObject &p)
{
    RcReply err;
    auto model = markerModel(p, err);
    auto pos = model ? rcInt(p, "position", err) : std::nullopt;
    if (!pos) return err;
    bool exists = false;
    model->getMarker(frameTime(*pos), &exists);
    if (!exists) return rcFail(QStringLiteral("not_found"), QStringLiteral("no marker at frame %1").arg(*pos));
    if (!model->removeMarker(frameTime(*pos))) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused to remove the marker"));
    return rcOk(QJsonObject());
}

RcReply markerCategories(const QJsonObject &)
{
    QJsonArray out;
    for (auto it = pCore->markerTypes.constBegin(); it != pCore->markerTypes.constEnd(); ++it) {
        out.append(QJsonObject{
            {QStringLiteral("index"), it.key()}, {QStringLiteral("name"), it.value().displayName}, {QStringLiteral("color"), it.value().color.name()}});
    }
    return rcOk(out);
}

TimelineController *controller()
{
    auto *widget = pCore->window()->getCurrentTimeline();
    return widget ? widget->controller() : nullptr;
}

RcReply zoneGet(const QJsonObject &)
{
    auto *c = controller();
    if (!c) return rcFail(QStringLiteral("no_timeline"), QStringLiteral("no open timeline"));
    return rcOk(QJsonObject{{QStringLiteral("in"), c->zoneIn()}, {QStringLiteral("out"), c->zoneOut()}});
}

RcReply zoneSet(const QJsonObject &p)
{
    RcReply err;
    auto in = rcInt(p, "in", err);
    auto out = in ? rcInt(p, "out", err) : std::nullopt;
    if (!out) return err;
    if (*in < 0 || *out <= *in) return rcFail(QStringLiteral("bad_params"), QStringLiteral("need 0 <= in < out (out is exclusive)"));
    auto *c = controller();
    if (!c) return rcFail(QStringLiteral("no_timeline"), QStringLiteral("no open timeline"));
    c->setZone(QPoint(*in, *out), true);
    return rcOk(QJsonObject());
}

const RcParam kScope{QStringLiteral("scope"), QStringLiteral("string"), false, QStringLiteral("'guides' (default) or 'clip'")};
const RcParam kBinId{QStringLiteral("binId"), QStringLiteral("string"), false, QStringLiteral("required for scope 'clip'")};
const RcParam kPos{QStringLiteral("position"), kInt, true, QStringLiteral("frame (clip markers: frame in the source clip)")};
} // namespace

void registerMarkerCommands(RcRegistry &r)
{
    r.insert(QStringLiteral("marker.list"),
             {QStringLiteral("marker.list"), QStringLiteral("Timeline guides or a bin clip's markers"), {kScope, kBinId}, false, true, false, markerList});
    r.insert(QStringLiteral("marker.add"), {QStringLiteral("marker.add"),
                                            QStringLiteral("Add a guide/marker; duration > 0 makes a range marker"),
                                            {kScope,
                                             kBinId,
                                             kPos,
                                             {QStringLiteral("comment"), QStringLiteral("string"), false, QString()},
                                             {QStringLiteral("category"), kInt, false, QStringLiteral("see marker.categories")},
                                             {QStringLiteral("duration"), kInt, false, QString()}},
                                            true,
                                            true,
                                            false,
                                            markerAdd});
    r.insert(QStringLiteral("marker.edit"), {QStringLiteral("marker.edit"),
                                             QStringLiteral("Edit the marker at position (omitted fields keep their value)"),
                                             {kScope,
                                              kBinId,
                                              kPos,
                                              {QStringLiteral("newPosition"), kInt, false, QString()},
                                              {QStringLiteral("comment"), QStringLiteral("string"), false, QString()},
                                              {QStringLiteral("category"), kInt, false, QString()},
                                              {QStringLiteral("duration"), kInt, false, QString()}},
                                             true,
                                             true,
                                             false,
                                             markerEdit});
    r.insert(QStringLiteral("marker.remove"),
             {QStringLiteral("marker.remove"), QStringLiteral("Remove the marker at position"), {kScope, kBinId, kPos}, true, true, false, markerRemove});
    r.insert(QStringLiteral("marker.categories"),
             {QStringLiteral("marker.categories"), QStringLiteral("Marker categories (index, name, color)"), {}, false, true, false, markerCategories});
    r.insert(QStringLiteral("zone.get"), {QStringLiteral("zone.get"), QStringLiteral("Timeline in/out zone (out exclusive)"), {}, false, true, true, zoneGet});
    r.insert(QStringLiteral("zone.set"), {QStringLiteral("zone.set"),
                                          QStringLiteral("Set the timeline in/out zone (out exclusive)"),
                                          {{QStringLiteral("in"), kInt, true, QString()}, {QStringLiteral("out"), kInt, true, QString()}},
                                          true,
                                          true,
                                          true,
                                          zoneSet});
}
