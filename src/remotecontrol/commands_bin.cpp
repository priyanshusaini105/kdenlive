/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "bin/bin.h"
#include "bin/clipcreator.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "rchelpers.h"

#include <QColor>
#include <QFileInfo>
#include <QJsonArray>
#include <QMetaEnum>

namespace {
std::shared_ptr<ProjectClip> rcBinClip(const QJsonObject &p, RcReply &err)
{
    auto binId = rcString(p, "binId", err);
    if (!binId) return nullptr;
    auto clip = pCore->projectItemModel()->getClipByBinID(*binId);
    if (!clip) err = rcFail(QStringLiteral("not_found"), QStringLiteral("no bin clip with id %1").arg(*binId));
    return clip;
}

std::optional<QString> rcFolder(const QJsonObject &p, const char *key, RcReply &err)
{
    auto id = rcString(p, key, err, QStringLiteral("-1"));
    if (id && *id != QLatin1String("-1") && !pCore->projectItemModel()->getFolderByBinId(*id)) {
        err = rcFail(QStringLiteral("not_found"), QStringLiteral("no bin folder with id %1").arg(*id));
        return std::nullopt;
    }
    return id;
}

QString parentId(const std::shared_ptr<AbstractProjectItem> &item)
{
    auto parent = item->parent();
    return parent && parent != pCore->projectItemModel()->getRootFolder() ? parent->clipId() : QStringLiteral("-1");
}

RcReply binList(const QJsonObject &)
{
    auto model = pCore->projectItemModel();
    const QMetaEnum types = QMetaEnum::fromType<ClipType::ProducerType>();
    QJsonArray clips;
    for (const QString &id : model->getAllClipIds()) {
        auto clip = model->getClipByBinID(id);
        if (!clip) continue;
        clips.append(QJsonObject{{QStringLiteral("binId"), id},
                                 {QStringLiteral("name"), clip->clipName()},
                                 {QStringLiteral("type"), QString::fromLatin1(types.valueToKey(clip->clipType()))},
                                 {QStringLiteral("folderId"), parentId(clip)},
                                 {QStringLiteral("duration"), clip->statusReady() ? int(clip->frameDuration()) : 0},
                                 {QStringLiteral("url"), clip->clipUrl()},
                                 {QStringLiteral("ready"), clip->statusReady()},
                                 {QStringLiteral("hasProxy"), clip->hasProxy()}});
    }
    QJsonArray folders;
    for (const auto &folder : model->getFolders()) {
        if (folder == model->getRootFolder()) continue;
        folders.append(
            QJsonObject{{QStringLiteral("id"), folder->clipId()}, {QStringLiteral("name"), folder->name()}, {QStringLiteral("parentId"), parentId(folder)}});
    }
    return rcOk(QJsonObject{{QStringLiteral("clips"), clips}, {QStringLiteral("folders"), folders}});
}

RcReply binImport(const QJsonObject &p)
{
    RcReply err;
    auto folder = rcFolder(p, "folderId", err);
    if (!folder) return err;
    const QJsonArray paths = p.value(QStringLiteral("paths")).toArray();
    if (paths.isEmpty()) return rcFail(QStringLiteral("bad_params"), QStringLiteral("'paths' must be a non-empty array of file paths"));
    for (const auto &path : paths) {
        if (!QFileInfo(path.toString()).isFile()) return rcFail(QStringLiteral("not_found"), QStringLiteral("no file %1").arg(path.toString()));
    }
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    QJsonArray ids;
    for (const auto &path : paths) {
        const QString id = ClipCreator::createClipFromFile(path.toString(), *folder, pCore->projectItemModel(), undo, redo);
        if (id.isEmpty() || id == QLatin1String("-1")) {
            undo();
            return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive could not import %1").arg(path.toString()));
        }
        ids.append(id);
    }
    pCore->pushUndo(undo, redo, QStringLiteral("Import clips"));
    return rcOk(QJsonObject{{QStringLiteral("binIds"), ids}});
}

RcReply binFolderCreate(const QJsonObject &p)
{
    RcReply err;
    auto name = rcString(p, "name", err);
    auto parent = name ? rcFolder(p, "parentId", err) : std::nullopt;
    if (!parent) return err;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    QString id;
    if (!pCore->projectItemModel()->requestAddFolder(id, *name, *parent, undo, redo)) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused to create the folder"));
    }
    pCore->pushUndo(undo, redo, QStringLiteral("Create folder"));
    return rcOk(QJsonObject{{QStringLiteral("folderId"), id}});
}

RcReply binRename(const QJsonObject &p)
{
    RcReply err;
    auto binId = rcString(p, "binId", err);
    auto name = binId ? rcString(p, "name", err) : std::nullopt;
    if (!name) return err;
    auto item = pCore->projectItemModel()->getItemByBinId(*binId);
    if (!item) return rcFail(QStringLiteral("not_found"), QStringLiteral("no bin item with id %1").arg(*binId));
    if (item->itemType() != AbstractProjectItem::FolderItem && !pCore->bin()) {
        return rcFail(QStringLiteral("no_gui"), QStringLiteral("renaming a clip needs the Kdenlive window (bin)"));
    }
    const bool done =
        item->itemType() == AbstractProjectItem::FolderItem ? pCore->projectItemModel()->requestRenameFolder(item, *name) : item->rename(*name, 0);
    if (!done) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the rename"));
    return rcOk(QJsonObject());
}

RcReply binDelete(const QJsonObject &p)
{
    RcReply err;
    auto binId = rcString(p, "binId", err);
    if (!binId) return err;
    auto item = pCore->projectItemModel()->getItemByBinId(*binId);
    if (!item) return rcFail(QStringLiteral("not_found"), QStringLiteral("no bin item with id %1").arg(*binId));
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    if (!pCore->projectItemModel()->requestBinClipDeletion(item, undo, redo))
        return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the deletion"));
    pCore->pushUndo(undo, redo, QStringLiteral("Delete bin item"));
    return rcOk(QJsonObject());
}

RcReply binColor(const QJsonObject &p)
{
    RcReply err;
    auto color = rcString(p, "color", err);
    auto duration = color ? rcInt(p, "duration", err) : std::nullopt;
    auto name = duration ? rcString(p, "name", err, QStringLiteral("Color")) : std::nullopt;
    auto folder = name ? rcFolder(p, "folderId", err) : std::nullopt;
    if (!folder) return err;
    // #RRGGBB or #RRGGBBAA (QColor reads #AARRGGBB, so parse the alpha separately)
    QString hex = color->startsWith(QLatin1Char('#')) ? color->mid(1) : *color;
    const QString alpha = hex.size() == 8 ? hex.right(2) : QStringLiteral("ff");
    const QColor rgb(QLatin1Char('#') + hex.left(6));
    if (!rgb.isValid() || (hex.size() != 6 && hex.size() != 8))
        return rcFail(QStringLiteral("bad_params"), QStringLiteral("color must be #RRGGBB or #RRGGBBAA"));
    if (*duration <= 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("duration must be > 0"));
    const QString mltColor = QStringLiteral("0x") + hex.left(6) + alpha;
    const QString id = ClipCreator::createColorClip(mltColor, *duration, *name, *folder, pCore->projectItemModel());
    if (id.isEmpty() || id == QLatin1String("-1")) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the color clip"));
    return rcOk(QJsonObject{{QStringLiteral("binId"), id}});
}

RcReply binPropertiesGet(const QJsonObject &p)
{
    RcReply err;
    auto clip = rcBinClip(p, err);
    if (!clip) return err;
    QStringList keys;
    for (const auto &k : p.value(QStringLiteral("keys")).toArray())
        keys << k.toString();
    if (keys.isEmpty()) {
        keys = {QStringLiteral("kdenlive:clipname"),
                QStringLiteral("resource"),
                QStringLiteral("length"),
                QStringLiteral("kdenlive:duration"),
                QStringLiteral("mlt_service"),
                QStringLiteral("meta.media.width"),
                QStringLiteral("meta.media.height"),
                QStringLiteral("meta.media.frame_rate_num"),
                QStringLiteral("meta.media.frame_rate_den"),
                QStringLiteral("kdenlive:proxy")};
    }
    QJsonObject out;
    for (const QString &key : std::as_const(keys))
        out.insert(key, clip->getProducerProperty(key));
    return rcOk(out);
}

RcReply binPropertiesSet(const QJsonObject &p)
{
    RcReply err;
    auto clip = rcBinClip(p, err);
    if (!clip) return err;
    const QJsonObject props = p.value(QStringLiteral("props")).toObject();
    if (props.isEmpty()) return rcFail(QStringLiteral("bad_params"), QStringLiteral("'props' must be a non-empty object"));
    QMap<QString, QString> newProps;
    for (auto it = props.begin(); it != props.end(); ++it)
        newProps.insert(it.key(), it.value().toVariant().toString());
    pCore->bin()->slotEditClipCommand(clip->clipId(), clip->currentProperties(newProps), newProps);
    return rcOk(QJsonObject());
}

RcReply binProxy(const QJsonObject &p)
{
    RcReply err;
    auto enabled = rcBool(p, "enabled", err);
    if (!enabled) return err;
    if (!pCore->currentDoc()->useProxy()) return rcFail(QStringLiteral("failed"), QStringLiteral("proxy clips are disabled in Project Settings"));
    QList<std::shared_ptr<ProjectClip>> clips;
    for (const auto &id : p.value(QStringLiteral("binIds")).toArray()) {
        auto clip = pCore->projectItemModel()->getClipByBinID(id.toString());
        if (!clip) return rcFail(QStringLiteral("not_found"), QStringLiteral("no bin clip with id %1").arg(id.toString()));
        clips << clip;
    }
    if (clips.isEmpty()) return rcFail(QStringLiteral("bad_params"), QStringLiteral("'binIds' must be a non-empty array"));
    pCore->currentDoc()->slotProxyCurrentItem(*enabled, clips);
    return rcOk(QJsonObject());
}

void add(RcRegistry &r, const QString &name, const QString &desc, const QList<RcParam> &params, bool mutating, bool needsGui,
         std::function<RcReply(const QJsonObject &)> fn)
{
    r.insert(name, RcCommand{name, desc, params, mutating, true, needsGui, std::move(fn)});
}

const RcParam kBinId{QStringLiteral("binId"), QStringLiteral("string"), true, QString()};
const RcParam kFolder{QStringLiteral("folderId"), QStringLiteral("string"), false, QStringLiteral("default -1 = root")};
} // namespace

void registerBinCommands(RcRegistry &r)
{
    add(r, QStringLiteral("bin.list"), QStringLiteral("Project bin clips (binId, name, type, duration, ready) and folders"), {}, false, false, binList);
    add(r, QStringLiteral("bin.import"), QStringLiteral("Import media files into the bin (loads asynchronously; check ready)"),
        {{QStringLiteral("paths"), QStringLiteral("array"), true, QString()}, kFolder}, true, false, binImport);
    add(r, QStringLiteral("bin.folder.create"), QStringLiteral("Create a bin folder"),
        {{QStringLiteral("name"), QStringLiteral("string"), true, QString()},
         {QStringLiteral("parentId"), QStringLiteral("string"), false, QStringLiteral("default -1")}},
        true, false, binFolderCreate);
    add(r, QStringLiteral("bin.rename"), QStringLiteral("Rename a bin clip or folder"),
        {kBinId, {QStringLiteral("name"), QStringLiteral("string"), true, QString()}}, true, false, binRename);
    add(r, QStringLiteral("bin.delete"), QStringLiteral("Delete a bin clip or folder (also removes its timeline instances)"), {kBinId}, true, false, binDelete);
    add(r, QStringLiteral("bin.color"), QStringLiteral("Create a color clip"),
        {{QStringLiteral("color"), QStringLiteral("string"), true, QStringLiteral("#RRGGBB or #RRGGBBAA")},
         {QStringLiteral("duration"), QStringLiteral("int"), true, QString()},
         {QStringLiteral("name"), QStringLiteral("string"), false, QString()},
         kFolder},
        true, false, binColor);
    add(r, QStringLiteral("bin.properties.get"), QStringLiteral("Producer properties of a bin clip"),
        {kBinId, {QStringLiteral("keys"), QStringLiteral("array"), false, QStringLiteral("default: common keys")}}, false, false, binPropertiesGet);
    add(r, QStringLiteral("bin.properties.set"), QStringLiteral("Set producer properties of a bin clip (undoable)"),
        {kBinId, {QStringLiteral("props"), QStringLiteral("object"), true, QString()}}, true, true, binPropertiesSet);
    add(r, QStringLiteral("bin.proxy"), QStringLiteral("Create or delete proxies for bin clips"),
        {{QStringLiteral("binIds"), QStringLiteral("array"), true, QString()}, {QStringLiteral("enabled"), QStringLiteral("bool"), true, QString()}}, true,
        true, binProxy);
}
