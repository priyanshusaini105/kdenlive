/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "core.h"
#include "monitor/monitor.h"
#include "monitor/monitorproxy.h"
#include "profiles/profilemodel.hpp"

#include "rchelpers.h"
#include <QBuffer>
#include <QImage>

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

RcReply monitorGrab(const QJsonObject &p)
{
    RcReply err;
    Monitor *m = monitorFrom(p, err);
    auto width = m ? rcInt(p, "width", err, 640) : std::nullopt;
    if (!width) return err;
    if (*width < 16 || *width > 3840) return rcFail(QStringLiteral("bad_params"), QStringLiteral("width must be 16-3840"));
    auto &profile = pCore->getCurrentProfile();
    const int height = qMax(2, int(*width * double(profile->height()) / profile->width()) & ~1);
    const QImage image = m->getControllerProxy()->extractFrame(QString(), *width, height);
    if (image.isNull()) return rcFail(QStringLiteral("failed"), QStringLiteral("no frame available (empty timeline?)"));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return rcOk(QJsonObject{{QStringLiteral("mime"), QStringLiteral("image/png")},
                            {QStringLiteral("width"), image.width()},
                            {QStringLiteral("height"), image.height()},
                            {QStringLiteral("position"), m->position()},
                            {QStringLiteral("data"), QString::fromLatin1(png.toBase64())}});
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
    r.insert(QStringLiteral("monitor.grab"),
             {QStringLiteral("monitor.grab"),
              QStringLiteral("The monitor's current frame as a base64 PNG, so you can look at the shot"),
              {kMonitor, {QStringLiteral("width"), QStringLiteral("int"), false, QStringLiteral("default 640; height follows the profile aspect")}},
              false,
              true,
              true,
              monitorGrab});
}
