/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QJsonObject>
#include <QString>

/** @brief Build kdenlivetitle XML (the titler's format) from a JSON spec, without the title dialog.
    spec: {background?: "#RRGGBBAA", items: [{type: "text", text, x, y, font, size, weight, italic, color, align, width?, outline, outlineColor}
           | {type: "rect", x, y, w, h, fill, stroke?, strokeWidth}], scroll?: {from: [x,y,w,h], to: [x,y,w,h]}}
    On error returns an empty string and sets *error. */
QString rcTitleXml(const QJsonObject &spec, int width, int height, int duration, QString *error);
