/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "core.h"
#include "monitor/monitor.h"
#include "rchelpers.h"

namespace {
/** "project" (default) or "clip" monitor. */
Monitor *monitorFrom(const QJsonObject &p, RcReply &err)
{
    auto which = rcString(p, "monitor", err, QStringLiteral("project"));
    if (!which) return nullptr;
    if (*which != QLatin1String("project") && *which != QLatin1String("clip")) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("monitor must be 'project' or 'clip'"));
        return nullptr;
    }
    Monitor *m = pCore->getMonitor(*which == QLatin1String("clip") ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor);
    if (!m) err = rcFail(QStringLiteral("no_gui"), QStringLiteral("monitor unavailable"));
    return m;
}

RcReply monitorSeek(const QJsonObject &p)
{
    RcReply err;
    Monitor *m = monitorFrom(p, err);
    auto pos = m ? rcInt(p, "position", err) : std::nullopt;
    if (!pos) return err;
    if (*pos < 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("position must be >= 0"));
    m->requestSeek(*pos);
    return rcOk(QJsonObject());
}

RcReply monitorPlay(const QJsonObject &p)
{
    RcReply err;
    Monitor *m = monitorFrom(p, err);
    auto play = m ? rcBool(p, "play", err, true) : std::nullopt;
    if (!play) return err;
    m->switchPlay(*play);
    return rcOk(QJsonObject());
}

RcReply monitorPosition(const QJsonObject &p)
{
    RcReply err;
    Monitor *m = monitorFrom(p, err);
    if (!m) return err;
    return rcOk(QJsonObject{{QStringLiteral("position"), m->position()}, {QStringLiteral("playing"), m->isPlaying()}});
}

const RcParam kMonitor{QStringLiteral("monitor"), QStringLiteral("string"), false, QStringLiteral("'project' (default) or 'clip'")};
} // namespace

void registerMonitorCommands(RcRegistry &r)
{
    r.insert(QStringLiteral("monitor.seek"), {QStringLiteral("monitor.seek"),
                                              QStringLiteral("Move the playhead to a frame"),
                                              {{QStringLiteral("position"), QStringLiteral("int"), true, QString()}, kMonitor},
                                              false,
                                              true,
                                              true,
                                              monitorSeek});
    r.insert(QStringLiteral("monitor.play"), {QStringLiteral("monitor.play"),
                                              QStringLiteral("Play (true) or pause (false)"),
                                              {{QStringLiteral("play"), QStringLiteral("bool"), false, QStringLiteral("default true")}, kMonitor},
                                              false,
                                              true,
                                              true,
                                              monitorPlay});
    r.insert(QStringLiteral("monitor.position"),
             {QStringLiteral("monitor.position"), QStringLiteral("Playhead frame and play state"), {kMonitor}, false, true, true, monitorPosition});
}
