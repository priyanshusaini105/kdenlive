/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "rctitle.h"

#include <QColor>
#include <QDomDocument>
#include <QJsonArray>

namespace {
/** "#RRGGBB" / "#RRGGBBAA" → "r,g,b,a" (the titler's color format). */
std::optional<QString> titleColor(const QJsonValue &v, const QString &def)
{
    QString hex = v.isString() ? v.toString() : def;
    if (hex.startsWith(QLatin1Char('#'))) hex.remove(0, 1);
    if (hex.size() != 6 && hex.size() != 8) return std::nullopt;
    const QColor c(QLatin1Char('#') + hex.left(6));
    bool ok = true;
    const int alpha = hex.size() == 8 ? hex.right(2).toInt(&ok, 16) : 255;
    if (!c.isValid() || !ok) return std::nullopt;
    return QStringLiteral("%1,%2,%3,%4").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

QString rectString(const QJsonValue &v, int width, int height)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 4) return QStringLiteral("0,0,%1,%2").arg(width).arg(height);
    return QStringLiteral("%1,%2,%3,%4").arg(a.at(0).toInt()).arg(a.at(1).toInt()).arg(a.at(2).toInt()).arg(a.at(3).toInt());
}

QDomElement positioned(QDomDocument &doc, const QString &type, int z, int x, int y)
{
    QDomElement item = doc.createElement(QStringLiteral("item"));
    item.setAttribute(QStringLiteral("type"), type);
    item.setAttribute(QStringLiteral("z-index"), z);
    QDomElement position = doc.createElement(QStringLiteral("position"));
    position.setAttribute(QStringLiteral("x"), x);
    position.setAttribute(QStringLiteral("y"), y);
    QDomElement transform = doc.createElement(QStringLiteral("transform"));
    transform.appendChild(doc.createTextNode(QStringLiteral("1,0,0,0,1,0,0,0,1")));
    position.appendChild(transform);
    item.appendChild(position);
    return item;
}
} // namespace

QString rcTitleXml(const QJsonObject &spec, int width, int height, int duration, QString *error)
{
    const QJsonArray items = spec.value(QStringLiteral("items")).toArray();
    if (items.isEmpty()) {
        *error = QStringLiteral("spec.items must be a non-empty array");
        return {};
    }
    QDomDocument doc;
    QDomElement root = doc.createElement(QStringLiteral("kdenlivetitle"));
    root.setAttribute(QStringLiteral("width"), width);
    root.setAttribute(QStringLiteral("height"), height);
    root.setAttribute(QStringLiteral("out"), duration);
    root.setAttribute(QStringLiteral("LC_NUMERIC"), QStringLiteral("C"));
    doc.appendChild(root);
    int z = 0;
    for (const auto &value : items) {
        const QJsonObject it = value.toObject();
        const QString type = it.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text")) {
            const QString align = it.value(QStringLiteral("align")).toString(QStringLiteral("left"));
            if (align != QLatin1String("left") && align != QLatin1String("center") && align != QLatin1String("right")) {
                *error = QStringLiteral("align must be left, center or right");
                return {};
            }
            auto color = titleColor(it.value(QStringLiteral("color")), QStringLiteral("#ffffffff"));
            auto outlineColor = titleColor(it.value(QStringLiteral("outlineColor")), QStringLiteral("#000000ff"));
            if (!color || !outlineColor) {
                *error = QStringLiteral("colors must be #RRGGBB or #RRGGBBAA");
                return {};
            }
            // Centered/right text without a box width spans the whole frame, so "center" means centered on screen.
            const bool boxed = align != QLatin1String("left") || it.contains(QStringLiteral("width"));
            const int boxWidth = it.value(QStringLiteral("width")).toInt(width);
            QDomElement item =
                positioned(doc, QStringLiteral("QGraphicsTextItem"), z++,
                           boxed && !it.contains(QStringLiteral("width")) ? 0 : it.value(QStringLiteral("x")).toInt(), it.value(QStringLiteral("y")).toInt());
            QDomElement content = doc.createElement(QStringLiteral("content"));
            content.setAttribute(QStringLiteral("font"), it.value(QStringLiteral("font")).toString(QStringLiteral("Sans")));
            content.setAttribute(QStringLiteral("font-pixel-size"), it.value(QStringLiteral("size")).toInt(64));
            content.setAttribute(QStringLiteral("font-weight"), it.value(QStringLiteral("weight")).toInt(400));
            content.setAttribute(QStringLiteral("font-italic"), it.value(QStringLiteral("italic")).toBool() ? 1 : 0);
            content.setAttribute(QStringLiteral("font-underline"), 0);
            content.setAttribute(QStringLiteral("font-color"), *color);
            content.setAttribute(QStringLiteral("font-outline"), it.value(QStringLiteral("outline")).toDouble(0));
            content.setAttribute(QStringLiteral("font-outline-color"), *outlineColor);
            if (boxed) {
                content.setAttribute(QStringLiteral("alignment"), align == QLatin1String("center")  ? int(Qt::AlignHCenter)
                                                                  : align == QLatin1String("right") ? int(Qt::AlignRight)
                                                                                                    : int(Qt::AlignLeft));
                content.setAttribute(QStringLiteral("box-width"), boxWidth);
                content.setAttribute(QStringLiteral("box-height"), it.value(QStringLiteral("height")).toInt(it.value(QStringLiteral("size")).toInt(64) * 2));
            }
            content.appendChild(doc.createTextNode(it.value(QStringLiteral("text")).toString()));
            item.appendChild(content);
            root.appendChild(item);
        } else if (type == QLatin1String("rect")) {
            auto fill = titleColor(it.value(QStringLiteral("fill")), QStringLiteral("#000000ff"));
            auto stroke = titleColor(it.value(QStringLiteral("stroke")), QStringLiteral("#00000000"));
            if (!fill || !stroke) {
                *error = QStringLiteral("colors must be #RRGGBB or #RRGGBBAA");
                return {};
            }
            QDomElement item =
                positioned(doc, QStringLiteral("QGraphicsRectItem"), z++, it.value(QStringLiteral("x")).toInt(), it.value(QStringLiteral("y")).toInt());
            QDomElement content = doc.createElement(QStringLiteral("content"));
            content.setAttribute(QStringLiteral("rect"),
                                 QStringLiteral("0,0,%1,%2").arg(it.value(QStringLiteral("w")).toInt(width)).arg(it.value(QStringLiteral("h")).toInt(height)));
            content.setAttribute(QStringLiteral("brushcolor"), *fill);
            content.setAttribute(QStringLiteral("pencolor"), *stroke);
            content.setAttribute(QStringLiteral("penwidth"), it.value(QStringLiteral("strokeWidth")).toInt(0));
            item.appendChild(content);
            root.appendChild(item);
        } else {
            *error = QStringLiteral("item type must be 'text' or 'rect'");
            return {};
        }
    }
    const QJsonObject scroll = spec.value(QStringLiteral("scroll")).toObject();
    QDomElement start = doc.createElement(QStringLiteral("startviewport"));
    start.setAttribute(QStringLiteral("rect"), rectString(scroll.value(QStringLiteral("from")), width, height));
    root.appendChild(start);
    QDomElement end = doc.createElement(QStringLiteral("endviewport"));
    end.setAttribute(QStringLiteral("rect"), rectString(scroll.value(QStringLiteral("to")), width, height));
    root.appendChild(end);
    auto background = titleColor(spec.value(QStringLiteral("background")), QStringLiteral("#00000000"));
    if (!background) {
        *error = QStringLiteral("background must be #RRGGBB or #RRGGBBAA");
        return {};
    }
    QDomElement bg = doc.createElement(QStringLiteral("background"));
    bg.setAttribute(QStringLiteral("color"), *background);
    root.appendChild(bg);
    error->clear();
    return doc.toString();
}
