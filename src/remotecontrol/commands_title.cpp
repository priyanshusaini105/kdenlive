/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "bin/bin.h"
#include "bin/clipcreator.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "profiles/profilemodel.hpp"
#include "rchelpers.h"
#include "rctitle.h"

namespace {
std::optional<QString> specXml(const QJsonObject &p, int duration, RcReply &err)
{
    if (!p.value(QStringLiteral("spec")).isObject()) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("missing object 'spec'"));
        return std::nullopt;
    }
    auto &profile = pCore->getCurrentProfile();
    QString error;
    const QString xml = rcTitleXml(p.value(QStringLiteral("spec")).toObject(), profile->width(), profile->height(), duration, &error);
    if (!error.isEmpty()) {
        err = rcFail(QStringLiteral("bad_params"), error);
        return std::nullopt;
    }
    return xml;
}

std::shared_ptr<ProjectClip> titleClip(const QJsonObject &p, RcReply &err)
{
    auto binId = rcString(p, "binId", err);
    if (!binId) return nullptr;
    auto clip = pCore->projectItemModel()->getClipByBinID(*binId);
    if (!clip) {
        err = rcFail(QStringLiteral("not_found"), QStringLiteral("no bin clip with id %1").arg(*binId));
        return nullptr;
    }
    if (clip->clipType() != ClipType::Text && clip->clipType() != ClipType::TextTemplate) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("bin clip %1 is not a title").arg(*binId));
        return nullptr;
    }
    return clip;
}

RcReply titleCreate(const QJsonObject &p)
{
    RcReply err;
    auto name = rcString(p, "name", err, QStringLiteral("Title"));
    auto duration = name ? rcInt(p, "duration", err) : std::nullopt;
    if (!duration) return err;
    if (*duration <= 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("duration must be > 0"));
    auto xml = specXml(p, *duration, err);
    if (!xml) return err;
    const QString id = ClipCreator::createTitleClip({{QStringLiteral("xmldata"), *xml}}, *duration, *name, QStringLiteral("-1"), pCore->projectItemModel());
    if (id.isEmpty() || id == QLatin1String("-1")) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the title clip"));
    return rcOk(QJsonObject{{QStringLiteral("binId"), id}});
}

RcReply titleUpdate(const QJsonObject &p)
{
    RcReply err;
    auto clip = titleClip(p, err);
    if (!clip) return err;
    auto xml = specXml(p, int(clip->frameDuration()), err);
    if (!xml) return err;
    // Same property swap the title dialog does (bin.cpp slotEditClip)
    QMap<QString, QString> newProps{{QStringLiteral("xmldata"), *xml}, {QStringLiteral("force_reload"), QStringLiteral("1")}};
    QMap<QString, QString> oldProps = clip->currentProperties(newProps);
    oldProps.insert(QStringLiteral("force_reload"), QStringLiteral("1"));
    pCore->bin()->slotEditClipCommand(clip->clipId(), oldProps, newProps);
    return rcOk(QJsonObject());
}

RcReply titleXml(const QJsonObject &p)
{
    RcReply err;
    auto clip = titleClip(p, err);
    if (!clip) return err;
    return rcOk(QJsonObject{{QStringLiteral("xml"), clip->getProducerProperty(QStringLiteral("xmldata"))}});
}

const RcParam kSpec{
    QStringLiteral("spec"), QStringLiteral("object"), true,
    QStringLiteral("{background?, items:[{type:text,text,x,y,font,size,weight,italic,color,align,width?,outline,outlineColor}|{type:rect,x,y,w,h,fill,stroke,"
                   "strokeWidth}], scroll?:{from:[x,y,w,h],to:[x,y,w,h]}} — profile pixels, colors #RRGGBB[AA]")};
} // namespace

void registerTitleCommands(RcRegistry &r)
{
    r.insert(
        QStringLiteral("title.create"),
        {QStringLiteral("title.create"),
         QStringLiteral("Create a title clip in the bin from a structured spec"),
         {{QStringLiteral("name"), QStringLiteral("string"), false, QString()}, {QStringLiteral("duration"), QStringLiteral("int"), true, QString()}, kSpec},
         true,
         true,
         false,
         titleCreate});
    r.insert(QStringLiteral("title.update"), {QStringLiteral("title.update"),
                                              QStringLiteral("Replace a title clip's content"),
                                              {{QStringLiteral("binId"), QStringLiteral("string"), true, QString()}, kSpec},
                                              true,
                                              true,
                                              true,
                                              titleUpdate});
    r.insert(QStringLiteral("title.xml"), {QStringLiteral("title.xml"),
                                           QStringLiteral("A title clip's kdenlivetitle XML"),
                                           {{QStringLiteral("binId"), QStringLiteral("string"), true, QString()}},
                                           false,
                                           true,
                                           false,
                                           titleXml});
}
