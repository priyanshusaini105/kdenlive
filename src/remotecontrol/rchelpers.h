/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "rccommand.h"

#include <memory>
#include <optional>

class TimelineItemModel;

/* All helpers return std::nullopt / nullptr and fill `err` on failure.
 * Kdenlive is built without exceptions: validate before touching any model getter. */
std::shared_ptr<TimelineItemModel> rcTimeline(RcReply &err);
std::optional<int> rcInt(const QJsonObject &p, const char *key, RcReply &err, std::optional<int> def = {});
std::optional<double> rcDouble(const QJsonObject &p, const char *key, RcReply &err, std::optional<double> def = {});
std::optional<QString> rcString(const QJsonObject &p, const char *key, RcReply &err, std::optional<QString> def = {});
std::optional<int> rcClip(const std::shared_ptr<TimelineItemModel> &tl, const QJsonObject &p, RcReply &err, const char *key = "clipId");
std::optional<int> rcTrack(const std::shared_ptr<TimelineItemModel> &tl, const QJsonObject &p, RcReply &err, const char *key = "trackId");
