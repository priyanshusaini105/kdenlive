/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "effects/effectsrepository.hpp"
#include "rchelpers.h"
#include "transitions/transitionsrepository.hpp"

#include <QJsonArray>
#include <QMetaEnum>

namespace {
const QStringList kAnimatedTypes{QStringLiteral("animated"),         QStringLiteral("animatedrect"),   QStringLiteral("keyframe"),
                                 QStringLiteral("geometry"),         QStringLiteral("simplekeyframe"), QStringLiteral("animatedfakerect"),
                                 QStringLiteral("animatedfakepoint")};

/** "effect" or "transition"; anything else → bad_params. */
std::optional<bool> isEffectKind(const QJsonObject &p, RcReply &err)
{
    auto kind = rcString(p, "kind", err, QStringLiteral("effect"));
    if (!kind) return std::nullopt;
    if (*kind == QLatin1String("effect")) return true;
    if (*kind == QLatin1String("transition")) return false;
    err = rcFail(QStringLiteral("bad_params"), QStringLiteral("kind must be 'effect' or 'transition'"));
    return std::nullopt;
}

template <typename Repo> RcReply listFrom(const Repo &repo, bool effects, const QString &filter, const QJsonValue &audioFilter)
{
    const QMetaEnum types = QMetaEnum::fromType<AssetListType::AssetType>();
    QJsonArray out;
    for (const auto &[id, name] : repo->getNames()) {
        const auto type = repo->getType(id);
        if (type == AssetListType::Hidden) continue;
        if (!filter.isEmpty() && !id.contains(filter, Qt::CaseInsensitive) && !name.contains(filter, Qt::CaseInsensitive)) continue;
        const bool audio = effects ? EffectsRepository::get()->isAudioEffect(id) : TransitionsRepository::get()->isAudio(id);
        if (audioFilter.isBool() && audioFilter.toBool() != audio) continue;
        QJsonObject item{{QStringLiteral("id"), id},
                         {QStringLiteral("name"), name},
                         {QStringLiteral("type"), QString::fromLatin1(types.valueToKey(type))},
                         {QStringLiteral("audio"), audio},
                         {QStringLiteral("description"), repo->getDescription(id)}};
        if (!effects) item.insert(QStringLiteral("composition"), TransitionsRepository::get()->isComposition(id));
        out.append(item);
    }
    return rcOk(out);
}

RcReply assetList(const QJsonObject &p)
{
    RcReply err;
    auto effects = isEffectKind(p, err);
    auto filter = effects ? rcString(p, "filter", err, QString()) : std::nullopt;
    if (!filter) return err;
    const QJsonValue audio = p.value(QStringLiteral("audio"));
    return *effects ? listFrom(EffectsRepository::get(), true, *filter, audio) : listFrom(TransitionsRepository::get(), false, *filter, audio);
}

RcReply assetSchema(const QJsonObject &p)
{
    RcReply err;
    auto effects = isEffectKind(p, err);
    auto id = effects ? rcString(p, "id", err) : std::nullopt;
    if (!id) return err;
    const bool exists = *effects ? EffectsRepository::get()->exists(*id) : TransitionsRepository::get()->exists(*id);
    if (!exists)
        return rcFail(QStringLiteral("not_found"), QStringLiteral("no %1 '%2'").arg(*effects ? QStringLiteral("effect") : QStringLiteral("transition"), *id));
    const QDomElement xml = *effects ? EffectsRepository::get()->getXml(*id) : TransitionsRepository::get()->getXml(*id);
    QJsonArray params;
    const QDomNodeList nodes = xml.elementsByTagName(QStringLiteral("parameter"));
    for (int i = 0; i < nodes.count(); ++i) {
        const QDomElement e = nodes.item(i).toElement();
        const QString type = e.attribute(QStringLiteral("type"));
        QJsonObject param{{QStringLiteral("name"), e.attribute(QStringLiteral("name"))},       {QStringLiteral("type"), type},
                          {QStringLiteral("default"), e.attribute(QStringLiteral("default"))}, {QStringLiteral("min"), e.attribute(QStringLiteral("min"))},
                          {QStringLiteral("max"), e.attribute(QStringLiteral("max"))},         {QStringLiteral("animated"), kAnimatedTypes.contains(type)}};
        const QString nameElem = e.firstChildElement(QStringLiteral("name")).text();
        if (!nameElem.isEmpty()) param.insert(QStringLiteral("label"), nameElem);
        if (e.hasAttribute(QStringLiteral("paramlist"))) {
            param.insert(QStringLiteral("values"), QJsonArray::fromStringList(e.attribute(QStringLiteral("paramlist")).split(QLatin1Char(';'))));
        }
        params.append(param);
    }
    return rcOk(QJsonObject{{QStringLiteral("id"), *id},
                            {QStringLiteral("name"), *effects ? EffectsRepository::get()->getName(*id) : TransitionsRepository::get()->getName(*id)},
                            {QStringLiteral("params"), params}});
}

const RcParam kKind{QStringLiteral("kind"), QStringLiteral("string"), false, QStringLiteral("'effect' (default) or 'transition'")};
} // namespace

void registerAssetCommands(RcRegistry &r)
{
    r.insert(QStringLiteral("asset.list"), {QStringLiteral("asset.list"),
                                            QStringLiteral("Available effects or transitions (id, name, type, audio)"),
                                            {kKind,
                                             {QStringLiteral("filter"), QStringLiteral("string"), false, QStringLiteral("substring of id or name")},
                                             {QStringLiteral("audio"), QStringLiteral("bool"), false, QStringLiteral("only audio (true) or video (false)")}},
                                            false,
                                            true,
                                            false,
                                            assetList});
    r.insert(QStringLiteral("asset.schema"), {QStringLiteral("asset.schema"),
                                              QStringLiteral("Parameters of an effect/transition: name, type, default, range, values, animated"),
                                              {kKind, {QStringLiteral("id"), QStringLiteral("string"), true, QString()}},
                                              false,
                                              true,
                                              false,
                                              assetSchema});
}
