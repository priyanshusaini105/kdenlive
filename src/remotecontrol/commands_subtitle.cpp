/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "bin/model/subtitlemodel.hpp"
#include "core.h"
#include "doc/docundostack.hpp"
#include "rchelpers.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QFileInfo>
#include <QJsonArray>

namespace {
GenTime frameTime(int frame)
{
    return GenTime(frame, pCore->getCurrentFps());
}

int toFrames(const GenTime &t)
{
    return t.frames(pCore->getCurrentFps());
}

/** The active timeline's subtitle model; created on demand when `create` is set. */
std::shared_ptr<SubtitleModel> subtitles(const std::shared_ptr<TimelineItemModel> &tl, bool create, RcReply &err)
{
    if (!tl->hasSubtitleModel()) {
        if (!create) {
            err = rcFail(QStringLiteral("not_found"), QStringLiteral("this timeline has no subtitles yet"));
            return nullptr;
        }
        return tl->createSubtitleModel();
    }
    return tl->getSubtitleModel();
}

/** Subtitle id → validated against the model. */
std::optional<int> rcSubtitle(const std::shared_ptr<SubtitleModel> &model, const QJsonObject &p, RcReply &err)
{
    auto id = rcInt(p, "id", err);
    if (!id) return std::nullopt;
    if (!model->hasSubtitle(*id)) {
        err = rcFail(QStringLiteral("not_found"), QStringLiteral("no subtitle with id %1").arg(*id));
        return std::nullopt;
    }
    return id;
}

#define RC_SUBS_OR_RETURN(tl, model, create)                                                                                                                   \
    RcReply err;                                                                                                                                               \
    auto tl = rcTimeline(err);                                                                                                                                 \
    if (!tl) return err;                                                                                                                                       \
    auto model = subtitles(tl, create, err);                                                                                                                   \
    if (!model) return err;

RcReply subtitleList(const QJsonObject &)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    QJsonArray out;
    if (!tl->hasSubtitleModel()) return rcOk(out);
    auto model = tl->getSubtitleModel();
    for (const auto &[start, event] : model->getAllSubtitles()) {
        out.append(QJsonObject{{QStringLiteral("id"), model->getIdForStartPos(start.first, start.second)},
                               {QStringLiteral("layer"), start.first},
                               {QStringLiteral("start"), toFrames(start.second)},
                               {QStringLiteral("end"), toFrames(event.endTime())},
                               {QStringLiteral("text"), event.text()},
                               {QStringLiteral("style"), event.styleName()}});
    }
    return rcOk(out);
}

RcReply subtitleAdd(const QJsonObject &p)
{
    RC_SUBS_OR_RETURN(tl, model, true)
    auto start = rcInt(p, "start", err);
    auto end = start ? rcInt(p, "end", err) : std::nullopt;
    auto text = end ? rcString(p, "text", err) : std::nullopt;
    auto layer = text ? rcInt(p, "layer", err, 0) : std::nullopt;
    if (!layer) return err;
    if (*start < 0 || *end <= *start) return rcFail(QStringLiteral("bad_params"), QStringLiteral("need 0 <= start < end"));
    if (*layer < 0 || *layer > model->getMaxLayer())
        return rcFail(QStringLiteral("bad_params"), QStringLiteral("layer out of range (0-%1)").arg(model->getMaxLayer()));
    if (model->isLocked()) return rcFail(QStringLiteral("failed"), QStringLiteral("the subtitle track is locked"));
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    const std::pair<int, GenTime> pos{*layer, frameTime(*start)};
    const SubtitleEvent event(true, frameTime(*end), model->getLayerDefaultStyle(*layer), QString(), 0, 0, 0, QString(), *text);
    if (!model->addSubtitle(pos, event, undo, redo)) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the subtitle (overlap?)"));
    pCore->pushUndo(undo, redo, QStringLiteral("Add subtitle"));
    return rcOk(QJsonObject{{QStringLiteral("id"), model->getIdForStartPos(*layer, frameTime(*start))}});
}

RcReply subtitleEdit(const QJsonObject &p)
{
    RC_SUBS_OR_RETURN(tl, model, false)
    auto id = rcSubtitle(model, p, err);
    if (!id) return err;
    const int layer = model->getLayerForId(*id);
    const int start = toFrames(model->getStartPosForId(*id));
    const int end = model->getSubtitleEnd(*id);
    auto newEnd = rcInt(p, "end", err, end);
    auto newStart = newEnd ? rcInt(p, "start", err, start) : std::nullopt;
    auto text = newStart ? rcString(p, "text", err, model->getText(*id)) : std::nullopt;
    auto style = text ? rcString(p, "style", err, model->getStyleName(*id)) : std::nullopt;
    if (!style) return err;
    if (*newStart < 0 || *newEnd <= *newStart) return rcFail(QStringLiteral("bad_params"), QStringLiteral("need 0 <= start < end"));
    auto stack = pCore->undoStack();
    stack->beginMacro(QStringLiteral("Edit subtitle"));
    if (*text != model->getText(*id)) model->editSubtitle(*id, *text, model->getText(*id));
    if (*style != model->getStyleName(*id)) model->setStyleName(*id, *style);
    if (*newEnd != end) model->resizeSubtitle(layer, start, *newEnd, end, true);
    if (*newStart != start) tl->requestSubtitleMove(*id, layer, *newStart, true, true, true);
    stack->endMacro();
    return rcOk(QJsonObject());
}

RcReply subtitleDelete(const QJsonObject &p)
{
    RC_SUBS_OR_RETURN(tl, model, false)
    auto id = rcSubtitle(model, p, err);
    if (!id) return err;
    model->deleteSubtitle(model->getLayerForId(*id), toFrames(model->getStartPosForId(*id)), model->getSubtitleEnd(*id), model->getText(*id));
    return rcOk(QJsonObject());
}

RcReply subtitleStyles(const QJsonObject &)
{
    RcReply err;
    auto tl = rcTimeline(err);
    if (!tl) return err;
    auto model = subtitles(tl, true, err);
    QJsonArray out;
    for (const auto &[name, style] : model->getAllSubtitleStyles(false)) {
        out.append(name);
    }
    return rcOk(out);
}

RcReply subtitleLayerAdd(const QJsonObject &)
{
    RC_SUBS_OR_RETURN(tl, model, true)
    model->requestCreateLayer();
    return rcOk(QJsonObject{{QStringLiteral("layer"), model->getMaxLayer()}});
}

RcReply subtitleLayerRemove(const QJsonObject &p)
{
    RC_SUBS_OR_RETURN(tl, model, false)
    auto layer = rcInt(p, "layer", err);
    if (!layer) return err;
    if (*layer < 0 || *layer > model->getMaxLayer()) return rcFail(QStringLiteral("not_found"), QStringLiteral("no subtitle layer %1").arg(*layer));
    model->requestDeleteLayer(*layer);
    return rcOk(QJsonObject());
}

RcReply subtitleImport(const QJsonObject &p)
{
    RC_SUBS_OR_RETURN(tl, model, true)
    auto path = rcString(p, "path", err);
    auto offset = path ? rcInt(p, "offset", err, 0) : std::nullopt;
    if (!offset) return err;
    if (!QFileInfo(*path).isFile()) return rcFail(QStringLiteral("not_found"), QStringLiteral("no file %1").arg(*path));
    if (model->isLocked()) return rcFail(QStringLiteral("failed"), QStringLiteral("the subtitle track is locked"));
    const double fps = pCore->getCurrentFps();
    model->importSubtitle(*path, *offset, true, float(fps), float(fps));
    return rcOk(QJsonObject());
}

RcReply subtitleExport(const QJsonObject &p)
{
    RC_SUBS_OR_RETURN(tl, model, false)
    auto path = rcString(p, "path", err);
    if (!path) return err;
    model->subtitleFileFromZone(0, qMax(tl->duration(), model->trackDuration()), *path);
    if (!QFileInfo::exists(*path)) return rcFail(QStringLiteral("failed"), QStringLiteral("could not write %1").arg(*path));
    return rcOk(QJsonObject());
}

void add(RcRegistry &r, const QString &name, const QString &desc, const QList<RcParam> &params, bool mutating, bool undoable,
         std::function<RcReply(const QJsonObject &)> fn)
{
    r.insert(name, RcCommand{name, desc, params, mutating, undoable, false, std::move(fn)});
}

const RcParam kId{QStringLiteral("id"), QStringLiteral("int"), true, QString()};
} // namespace

void registerSubtitleCommands(RcRegistry &r)
{
    add(r, QStringLiteral("subtitle.list"), QStringLiteral("Subtitles of the active timeline (frames)"), {}, false, true, subtitleList);
    add(r, QStringLiteral("subtitle.add"), QStringLiteral("Add a subtitle from start to end (frames)"),
        {{QStringLiteral("start"), QStringLiteral("int"), true, QString()},
         {QStringLiteral("end"), QStringLiteral("int"), true, QString()},
         {QStringLiteral("text"), QStringLiteral("string"), true, QString()},
         {QStringLiteral("layer"), QStringLiteral("int"), false, QStringLiteral("default 0")}},
        true, true, subtitleAdd);
    add(r, QStringLiteral("subtitle.edit"), QStringLiteral("Change a subtitle's text, start, end or style (one undo step)"),
        {kId,
         {QStringLiteral("text"), QStringLiteral("string"), false, QString()},
         {QStringLiteral("start"), QStringLiteral("int"), false, QString()},
         {QStringLiteral("end"), QStringLiteral("int"), false, QString()},
         {QStringLiteral("style"), QStringLiteral("string"), false, QStringLiteral("see subtitle.styles")}},
        true, true, subtitleEdit);
    add(r, QStringLiteral("subtitle.delete"), QStringLiteral("Delete a subtitle"), {kId}, true, true, subtitleDelete);
    add(r, QStringLiteral("subtitle.styles"), QStringLiteral("Names of the project's subtitle styles"), {}, false, true, subtitleStyles);
    add(r, QStringLiteral("subtitle.layer.add"), QStringLiteral("Add a subtitle layer"), {}, true, true, subtitleLayerAdd);
    add(r, QStringLiteral("subtitle.layer.remove"), QStringLiteral("Remove a subtitle layer"),
        {{QStringLiteral("layer"), QStringLiteral("int"), true, QString()}}, true, true, subtitleLayerRemove);
    add(r, QStringLiteral("subtitle.import"), QStringLiteral("Import an .srt/.ass/.vtt/.sbv file (not undoable)"),
        {{QStringLiteral("path"), QStringLiteral("string"), true, QString()},
         {QStringLiteral("offset"), QStringLiteral("int"), false, QStringLiteral("frames")}},
        true, false, subtitleImport);
    add(r, QStringLiteral("subtitle.export"), QStringLiteral("Write the subtitles to a file (format from extension)"),
        {{QStringLiteral("path"), QStringLiteral("string"), true, QString()}}, false, true, subtitleExport);
}
