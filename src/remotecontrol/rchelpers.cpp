/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "rchelpers.h"
#include "core.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <cmath>

RcReply rcOk(const QJsonValue &v)
{
    return RcReply{v, QString(), QString()};
}

RcReply rcFail(const QString &code, const QString &message)
{
    return RcReply{QJsonValue(), code, message};
}

std::shared_ptr<TimelineItemModel> rcTimeline(RcReply &err)
{
    std::shared_ptr<TimelineItemModel> tl = pCore->projectManager() ? pCore->projectManager()->getTimeline() : nullptr;
    if (!tl) {
        err = rcFail(QStringLiteral("no_timeline"), QStringLiteral("no active timeline"));
    }
    return tl;
}

static QString keyName(const char *key)
{
    return QString::fromLatin1(key);
}

std::optional<int> rcInt(const QJsonObject &p, const char *key, RcReply &err, std::optional<int> def)
{
    const QJsonValue v = p.value(keyName(key));
    if (v.isUndefined() || v.isNull()) {
        if (def) return def;
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("missing integer '%1'").arg(keyName(key)));
        return std::nullopt;
    }
    const double d = v.toDouble(std::nan(""));
    if (!v.isDouble() || std::floor(d) != d) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("'%1' must be an integer").arg(keyName(key)));
        return std::nullopt;
    }
    return int(d);
}

std::optional<double> rcDouble(const QJsonObject &p, const char *key, RcReply &err, std::optional<double> def)
{
    const QJsonValue v = p.value(keyName(key));
    if (v.isUndefined() || v.isNull()) {
        if (def) return def;
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("missing number '%1'").arg(keyName(key)));
        return std::nullopt;
    }
    if (!v.isDouble()) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("'%1' must be a number").arg(keyName(key)));
        return std::nullopt;
    }
    return v.toDouble();
}

std::optional<QString> rcString(const QJsonObject &p, const char *key, RcReply &err, std::optional<QString> def)
{
    const QJsonValue v = p.value(keyName(key));
    if (v.isUndefined() || v.isNull()) {
        if (def) return def;
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("missing string '%1'").arg(keyName(key)));
        return std::nullopt;
    }
    if (!v.isString()) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("'%1' must be a string").arg(keyName(key)));
        return std::nullopt;
    }
    return v.toString();
}

std::optional<int> rcClip(const std::shared_ptr<TimelineItemModel> &tl, const QJsonObject &p, RcReply &err, const char *key)
{
    auto id = rcInt(p, key, err);
    if (!id) return std::nullopt;
    if (!tl->isClip(*id)) {
        err = rcFail(QStringLiteral("not_found"), QStringLiteral("no clip with id %1").arg(*id));
        return std::nullopt;
    }
    return id;
}

std::optional<int> rcTrack(const std::shared_ptr<TimelineItemModel> &tl, const QJsonObject &p, RcReply &err, const char *key)
{
    auto id = rcInt(p, key, err);
    if (!id) return std::nullopt;
    if (!tl->isTrack(*id)) {
        err = rcFail(QStringLiteral("not_found"), QStringLiteral("no track with id %1").arg(*id));
        return std::nullopt;
    }
    return id;
}
