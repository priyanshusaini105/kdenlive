/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "assets/keyframes/model/keyframemodel.hpp"
#include "assets/keyframes/model/keyframemodellist.hpp"
#include "assets/model/assetcommand.hpp"
#include "assets/model/assetparametermodel.hpp"
#include "core.h"
#include "doc/docundostack.hpp"
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
    return rcAssetParams(fx.get(), EffectsRepository::get()->getXml(fx->getAssetId()));
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
    return rcSetAssetParams(fx, p.value(QStringLiteral("params")).toObject());
}

RcReply effectRemove(const QJsonObject &p)
{
    RcReply err;
    auto stack = stackFrom(p, err);
    if (!stack) return err;
    auto fx = effectAt(stack, p, err);
    if (!fx) return err;
    stack->removeEffect(fx);
    return rcOk(QJsonObject());
}

RcReply effectMove(const QJsonObject &p)
{
    RcReply err;
    auto stack = stackFrom(p, err);
    if (!stack) return err;
    auto from = rcInt(p, "from", err);
    auto to = from ? rcInt(p, "to", err) : std::nullopt;
    if (!to) return err;
    const auto effects = visibleEffects(stack);
    if (*from < 0 || *from >= effects.size() || *to < 0 || *to >= effects.size()) {
        return rcFail(QStringLiteral("bad_params"), QStringLiteral("indexes must be within 0-%1").arg(effects.size() - 1));
    }
    if (*from == *to) return rcOk(QJsonObject());
    // moveEffect inserts before destRow, so moving down needs the row after the target (GUI does row + 2 for one step down).
    const int targetRow = effects.at(*to)->row();
    stack->moveEffect(*to > *from ? targetRow + 1 : targetRow, effects.at(*from));
    if (visibleEffects(stack).indexOf(effects.at(*from)) != *to) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the move (built-in effects stay first)"));
    }
    return rcOk(QJsonObject());
}

RcReply effectEnable(const QJsonObject &p)
{
    RcReply err;
    auto stack = stackFrom(p, err);
    if (!stack) return err;
    auto fx = effectAt(stack, p, err);
    auto enabled = fx ? rcBool(p, "enabled", err) : std::nullopt;
    if (!enabled) return err;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    std::static_pointer_cast<AbstractEffectItem>(fx)->markEnabled(*enabled, undo, redo, false);
    redo();
    pCore->pushUndo(undo, redo, *enabled ? QStringLiteral("Enable effect") : QStringLiteral("Disable effect"));
    return rcOk(QJsonObject());
}

const QList<std::pair<QString, KeyframeType::KeyframeEnum>> kInterp{{QStringLiteral("linear"), KeyframeType::Linear},
                                                                    {QStringLiteral("discrete"), KeyframeType::Discrete},
                                                                    {QStringLiteral("smooth"), KeyframeType::Curve},
                                                                    {QStringLiteral("smooth_natural"), KeyframeType::CurveSmooth},
                                                                    {QStringLiteral("bounce_in"), KeyframeType::BounceIn},
                                                                    {QStringLiteral("bounce_out"), KeyframeType::BounceOut},
                                                                    {QStringLiteral("cubic_in"), KeyframeType::CubicIn},
                                                                    {QStringLiteral("cubic_out"), KeyframeType::CubicOut},
                                                                    {QStringLiteral("exponential_in"), KeyframeType::ExponentialIn},
                                                                    {QStringLiteral("exponential_out"), KeyframeType::ExponentialOut},
                                                                    {QStringLiteral("circular_in"), KeyframeType::CircularIn},
                                                                    {QStringLiteral("circular_out"), KeyframeType::CircularOut},
                                                                    {QStringLiteral("elastic_in"), KeyframeType::ElasticIn},
                                                                    {QStringLiteral("elastic_out"), KeyframeType::ElasticOut}};

QString interpName(KeyframeType::KeyframeEnum type)
{
    for (const auto &[name, value] : kInterp) {
        if (value == type) return name;
    }
    return QStringLiteral("linear");
}

/** An animated parameter: {target, index, param}. Keyframe positions are shared by all animated parameters of an effect. */
struct RcKeys
{
    std::shared_ptr<KeyframeModelList> list;
    QPersistentModelIndex ix;
    KeyframeModel *km = nullptr;
};

std::optional<RcKeys> keyModel(const QJsonObject &p, RcReply &err)
{
    auto stack = stackFrom(p, err);
    if (!stack) return std::nullopt;
    auto fx = effectAt(stack, p, err);
    auto param = fx ? rcString(p, "param", err) : std::nullopt;
    if (!param) return std::nullopt;
    std::shared_ptr<AssetParameterModel> model = fx;
    const QModelIndex ix = model->getParamIndexFromName(*param);
    if (!ix.isValid()) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("effect '%1' has no parameter '%2'").arg(fx->getAssetId(), *param));
        return std::nullopt;
    }
    RcKeys keys{model->getKeyframeModel(), QPersistentModelIndex(ix)};
    keys.km = keys.list ? keys.list->getKeyModel(keys.ix) : nullptr;
    if (!keys.km) {
        err = rcFail(QStringLiteral("bad_params"), QStringLiteral("parameter '%1' is not animated; use effect.set").arg(*param));
        return std::nullopt;
    }
    return keys;
}

GenTime frameTime(int frame)
{
    return GenTime(frame, pCore->getCurrentFps());
}

QVariant keyValue(const QJsonValue &v)
{
    if (v.isDouble()) return v.toDouble();
    bool isNumber = false;
    const double d = v.toString().toDouble(&isNumber);
    return isNumber ? QVariant(d) : QVariant(v.toString());
}

RcReply keyframeList(const QJsonObject &p)
{
    RcReply err;
    auto keys = keyModel(p, err);
    if (!keys) return err;
    KeyframeModel *km = keys->km;
    QJsonArray out;
    for (int row = 0; row < km->rowCount(); ++row) {
        const QModelIndex i = km->index(row, 0);
        const int frame = km->data(i, KeyframeModel::FrameRole).toInt();
        out.append(QJsonObject{{QStringLiteral("frame"), frame},
                               {QStringLiteral("value"), km->getInterpolatedValue(frame).toString()},
                               {QStringLiteral("interp"), interpName(KeyframeType::KeyframeEnum(km->data(i, KeyframeModel::TypeRole).toInt()))}});
    }
    return rcOk(out);
}

/** Runs several undo-pushing Kdenlive calls as one undo step. */
template <typename F> bool asOneStep(const QString &label, F &&f)
{
    auto stack = pCore->undoStack();
    stack->beginMacro(label);
    const bool done = f();
    stack->endMacro();
    return done;
}

RcReply keyframeSet(const QJsonObject &p)
{
    RcReply err;
    auto keys = keyModel(p, err);
    auto frame = keys ? rcInt(p, "frame", err) : std::nullopt;
    auto interp = frame ? rcString(p, "interp", err, QString()) : std::nullopt;
    if (!interp) return err;
    if (!p.contains(QStringLiteral("value"))) return rcFail(QStringLiteral("bad_params"), QStringLiteral("missing 'value'"));
    if (*frame < 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("frame must be >= 0 (item-local)"));
    std::optional<KeyframeType::KeyframeEnum> type;
    for (const auto &[name, value] : kInterp) {
        if (name == *interp) type = value;
    }
    if (!interp->isEmpty() && !type) {
        return rcFail(QStringLiteral("bad_params"), QStringLiteral("unknown interp '%1'; use linear, discrete, smooth, cubic_in, …").arg(*interp));
    }
    const GenTime pos = frameTime(*frame);
    const QVariant value = keyValue(p.value(QStringLiteral("value")));
    const bool done = asOneStep(QStringLiteral("Set keyframe"), [&]() {
        if (!keys->km->hasKeyframe(pos) && !keys->list->addKeyframe(pos, type.value_or(KeyframeType::Linear))) return false;
        keys->list->updateKeyframe(pos, value, -1, keys->ix);
        if (type) {
            Fun undo = []() { return true; };
            Fun redo = []() { return true; };
            if (!keys->list->updateKeyframeType(pos, int(*type), keys->ix, undo, redo)) return false;
            pCore->pushUndo(undo, redo, QStringLiteral("Keyframe type"));
        }
        return true;
    });
    if (!done) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the keyframe (outside the clip?)"));
    return rcOk(QJsonObject());
}

RcReply keyframeRemove(const QJsonObject &p)
{
    RcReply err;
    auto keys = keyModel(p, err);
    auto frame = keys ? rcInt(p, "frame", err) : std::nullopt;
    if (!frame) return err;
    if (!keys->km->hasKeyframe(*frame)) return rcFail(QStringLiteral("not_found"), QStringLiteral("no keyframe at frame %1").arg(*frame));
    if (!keys->list->removeKeyframe(frameTime(*frame))) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused (last keyframe?)"));
    return rcOk(QJsonObject());
}

RcReply keyframeMove(const QJsonObject &p)
{
    RcReply err;
    auto keys = keyModel(p, err);
    auto frame = keys ? rcInt(p, "frame", err) : std::nullopt;
    auto newFrame = frame ? rcInt(p, "newFrame", err) : std::nullopt;
    if (!newFrame) return err;
    if (!keys->km->hasKeyframe(*frame)) return rcFail(QStringLiteral("not_found"), QStringLiteral("no keyframe at frame %1").arg(*frame));
    if (*newFrame < 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("newFrame must be >= 0"));
    if (!keys->list->moveKeyframe(frameTime(*frame), frameTime(*newFrame), true)) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the move (occupied?)"));
    }
    return rcOk(QJsonObject());
}

const QList<RcParam> kTarget{{QStringLiteral("target"), QStringLiteral("object"), true, QStringLiteral("{type: clip|track|master|bin, id}")}};
} // namespace

QJsonArray rcAssetParams(AssetParameterModel *m, const QDomElement &xml)
{
    QHash<QString, QString> types;
    const QDomNodeList xmlParams = xml.elementsByTagName(QStringLiteral("parameter"));
    for (int i = 0; i < xmlParams.count(); ++i) {
        const QDomElement e = xmlParams.item(i).toElement();
        types.insert(e.attribute(QStringLiteral("name")), e.attribute(QStringLiteral("type")));
    }
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

RcReply rcSetAssetParams(const std::shared_ptr<AssetParameterModel> &model, const QJsonObject &params)
{
    if (params.isEmpty()) {
        return rcFail(QStringLiteral("bad_params"), QStringLiteral("'params' must be a non-empty object of name: value"));
    }
    QList<QModelIndex> indexes;
    QStringList values;
    for (auto it = params.begin(); it != params.end(); ++it) {
        const QModelIndex ix = model->getParamIndexFromName(it.key());
        if (!ix.isValid()) {
            return rcFail(QStringLiteral("bad_params"), QStringLiteral("'%1' has no parameter '%2'").arg(model->getAssetId(), it.key()));
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
    const RcParam index{QStringLiteral("index"), QStringLiteral("int"), true, QStringLiteral("visible stack index")};
    r.insert(QStringLiteral("effect.remove"),
             {QStringLiteral("effect.remove"), QStringLiteral("Remove an effect"), kTarget + QList<RcParam>{index}, true, true, false, effectRemove});
    r.insert(QStringLiteral("effect.move"), {QStringLiteral("effect.move"), QStringLiteral("Reorder an effect in the stack"),
                                             kTarget + QList<RcParam>{{QStringLiteral("from"), QStringLiteral("int"), true, QString()},
                                                                      {QStringLiteral("to"), QStringLiteral("int"), true, QString()}},
                                             true, true, false, effectMove});
    r.insert(QStringLiteral("effect.enable"),
             {QStringLiteral("effect.enable"), QStringLiteral("Enable or bypass an effect"),
              kTarget + QList<RcParam>{index, {QStringLiteral("enabled"), QStringLiteral("bool"), true, QString()}}, true, true, false, effectEnable});
    const QList<RcParam> kf =
        kTarget + QList<RcParam>{index, {QStringLiteral("param"), QStringLiteral("string"), true, QStringLiteral("animated parameter name")}};
    const RcParam frame{QStringLiteral("frame"), QStringLiteral("int"), true, QStringLiteral("item-local frame (0 = clip start)")};
    r.insert(QStringLiteral("keyframe.list"),
             {QStringLiteral("keyframe.list"), QStringLiteral("Keyframes of an animated parameter"), kf, false, true, false, keyframeList});
    r.insert(QStringLiteral("keyframe.set"),
             {QStringLiteral("keyframe.set"),
              QStringLiteral("Add or update a keyframe (positions are shared by the effect's animated parameters); interp: linear, discrete, smooth, "
                             "smooth_natural, {bounce,cubic,exponential,circular,elastic}_{in,out}"),
              kf + QList<RcParam>{frame,
                                  {QStringLiteral("value"), QStringLiteral("string"), true, QStringLiteral("number or 'x y w h opacity' for rects")},
                                  {QStringLiteral("interp"), QStringLiteral("string"), false, QString()}},
              true, true, false, keyframeSet});
    r.insert(QStringLiteral("keyframe.remove"),
             {QStringLiteral("keyframe.remove"), QStringLiteral("Remove a keyframe (from all animated parameters of the effect)"), kf + QList<RcParam>{frame},
              true, true, false, keyframeRemove});
    r.insert(QStringLiteral("keyframe.move"),
             {QStringLiteral("keyframe.move"), QStringLiteral("Move a keyframe to another item-local frame"),
              kf + QList<RcParam>{frame, {QStringLiteral("newFrame"), QStringLiteral("int"), true, QString()}}, true, true, false, keyframeMove});
}
