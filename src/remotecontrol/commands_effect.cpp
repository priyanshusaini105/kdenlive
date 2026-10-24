/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "assets/model/assetcommand.hpp"
#include "assets/model/assetparametermodel.hpp"
#include "core.h"
#include "effects/effectsrepository.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "rchelpers.h"

#include <QJsonArray>

namespace {
/** Effects shown in the effect stack, skipping hidden built-in ones (visible index → item). */
QList<std::shared_ptr<EffectItemModel>> visibleEffects(const std::shared_ptr<EffectStackModel> &stack)
{
    QList<std::shared_ptr<EffectItemModel>> out;
    for (int row = 0; row < stack->rowCount(); ++row) {
        auto fx = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(row));
        if (fx && !fx->hideFromStack()) {
            out << fx;
        }
    }
    return out;
}

std::shared_ptr<EffectStackModel> stackFrom(const QJsonObject &p, RcReply &err)
{
    if (!p.value(QStringLiteral("target")).isObject()) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("missing object 'target'"));
        return nullptr;
    }
    return rcEffectStack(p.value(QStringLiteral("target")).toObject(), err);
}

std::shared_ptr<EffectItemModel> effectAt(const std::shared_ptr<EffectStackModel> &stack, const QJsonObject &p, RcReply &err)
{
    auto index = rcInt(p, "index", err);
    if (!index) return nullptr;
    const auto effects = visibleEffects(stack);
    if (*index < 0 || *index >= effects.size()) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("effect index %1 out of range (0-%2)").arg(*index).arg(effects.size() - 1));
        return nullptr;
    }
    return effects.at(*index);
}

QString valueString(const QJsonValue &v)
{
    return v.isString() ? v.toString() : QString::number(v.toDouble());
}

RcReply effectAdd(const QJsonObject &p)
{
    RcReply err;
    auto stack = stackFrom(p, err);
    if (!stack) return err;
    auto effectId = rcString(p, "effectId", err);
    if (!effectId) return err;
    if (!EffectsRepository::get()->exists(*effectId)) {
        return rcFail(QStringLiteral("not_found"), QStringLiteral("no effect '%1'; see asset.list").arg(*effectId));
    }
    stringMap params;
    const QJsonObject initial = p.value(QStringLiteral("params")).toObject();
    for (auto it = initial.begin(); it != initial.end(); ++it) {
        params.insert(it.key(), valueString(it.value()));
    }
    if (!stack->appendEffect(*effectId, false, params)) {
        return rcFail(QStringLiteral("failed"),
                      QStringLiteral("Kdenlive refused to add '%1' (unique effect already present or wrong media type?)").arg(*effectId));
    }
    return rcOk(QJsonObject{{QStringLiteral("index"), int(visibleEffects(stack).size()) - 1}});
}

QJsonArray paramsJson(const std::shared_ptr<EffectItemModel> &fx)
{
    QHash<QString, QString> types;
    const QDomNodeList xmlParams = EffectsRepository::get()->getXml(fx->getAssetId()).elementsByTagName(QStringLiteral("parameter"));
    for (int i = 0; i < xmlParams.count(); ++i) {
        const QDomElement e = xmlParams.item(i).toElement();
        types.insert(e.attribute(QStringLiteral("name")), e.attribute(QStringLiteral("type")));
    }
    AssetParameterModel *m = fx.get();
    QJsonArray out;
    for (int row = 0; row < m->rowCount(); ++row) {
        const QModelIndex ix = m->index(row, 0);
        const QString name = m->data(ix, AssetParameterModel::NameRole).toString();
        QJsonObject param{{QStringLiteral("name"), name},
                          {QStringLiteral("type"), types.value(name)},
                          {QStringLiteral("value"), m->data(ix, AssetParameterModel::ValueRole).toString()},
                          {QStringLiteral("default"), m->data(ix, AssetParameterModel::DefaultRole).toString()},
                          {QStringLiteral("min"), m->data(ix, AssetParameterModel::MinRole).toString()},
                          {QStringLiteral("max"), m->data(ix, AssetParameterModel::MaxRole).toString()}};
        const QStringList values = m->data(ix, AssetParameterModel::ListValuesRole).toStringList();
        if (values.size() > 1) {
            param.insert(QStringLiteral("values"), QJsonArray::fromStringList(values));
        }
        out.append(param);
    }
    return out;
}

RcReply effectGet(const QJsonObject &p)
{
    RcReply err;
    auto stack = stackFrom(p, err);
    if (!stack) return err;
    QJsonArray out;
    const auto effects = visibleEffects(stack);
    for (int i = 0; i < effects.size(); ++i) {
        const auto &fx = effects.at(i);
        out.append(QJsonObject{{QStringLiteral("index"), i},
                               {QStringLiteral("effectId"), fx->getAssetId()},
                               {QStringLiteral("name"), EffectsRepository::get()->getName(fx->getAssetId())},
                               {QStringLiteral("enabled"), fx->isAssetEnabled()},
                               {QStringLiteral("params"), paramsJson(fx)}});
    }
    return rcOk(out);
}

RcReply effectSet(const QJsonObject &p)
{
    RcReply err;
    auto stack = stackFrom(p, err);
    if (!stack) return err;
    auto fx = effectAt(stack, p, err);
    if (!fx) return err;
    const QJsonObject params = p.value(QStringLiteral("params")).toObject();
    if (params.isEmpty()) {
        return rcFail(QStringLiteral("bad_params"), QStringLiteral("'params' must be a non-empty object of name: value"));
    }
    std::shared_ptr<AssetParameterModel> model = fx;
    QList<QModelIndex> indexes;
    QStringList values;
    for (auto it = params.begin(); it != params.end(); ++it) {
        const QModelIndex ix = model->getParamIndexFromName(it.key());
        if (!ix.isValid()) {
            return rcFail(QStringLiteral("bad_params"), QStringLiteral("effect '%1' has no parameter '%2'").arg(fx->getAssetId(), it.key()));
        }
        indexes << ix;
        values << valueString(it.value());
    }
    if (indexes.size() == 1) {
        pCore->pushUndo(new AssetCommand(model, indexes.first(), values.first()));
    } else {
        pCore->pushUndo(new AssetMultiCommand(model, indexes, values));
    }
    return rcOk(QJsonObject());
}

const QList<RcParam> kTarget{{QStringLiteral("target"), QStringLiteral("object"), true, QStringLiteral("{type: clip|track|master|bin, id}")}};
} // namespace

void registerEffectCommands(RcRegistry &r)
{
    r.insert(QStringLiteral("effect.add"),
             {QStringLiteral("effect.add"), QStringLiteral("Append an effect to a clip/track/master/bin clip stack"),
              kTarget + QList<RcParam>{{QStringLiteral("effectId"), QStringLiteral("string"), true, QString()},
                                       {QStringLiteral("params"), QStringLiteral("object"), false, QStringLiteral("initial name: value")}},
              true, true, false, effectAdd});
    r.insert(QStringLiteral("effect.get"),
             {QStringLiteral("effect.get"), QStringLiteral("Visible effects of a stack with their parameters"), kTarget, false, true, false, effectGet});
    r.insert(QStringLiteral("effect.set"),
             {QStringLiteral("effect.set"), QStringLiteral("Set effect parameters (values may be MLT animation strings like '0=0;50~=1')"),
              kTarget + QList<RcParam>{{QStringLiteral("index"), QStringLiteral("int"), true, QStringLiteral("visible stack index")},
                                       {QStringLiteral("params"), QStringLiteral("object"), true, QStringLiteral("name: value")}},
              true, true, false, effectSet});
}
