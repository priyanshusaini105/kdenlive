/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "bin/clipcreator.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "profiles/profilemodel.hpp"
#include "profiles/profilerepository.hpp"
#include "project/projectmanager.h"
#include "rchelpers.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>
#include <QUrl>

namespace {
KdenliveDoc *currentDoc(RcReply &err)
{
    KdenliveDoc *doc = pCore->currentDoc();
    if (!doc) err = rcFail(QStringLiteral("no_timeline"), QStringLiteral("no open project"));
    return doc;
}

/** Runs `work` after the D-Bus reply, as a job. `work` returns an error message or an empty string. */
RcReply deferred(const QString &kind, std::function<QString()> work)
{
    if (!pCore->window()) return rcFail(QStringLiteral("no_gui"), QStringLiteral("'%1' needs the Kdenlive window").arg(kind));
    const int id = rcJobStart(kind);
    QTimer::singleShot(0, pCore.get(), [id, work]() { rcJobFinish(id, work()); });
    return rcOk(QJsonObject{{QStringLiteral("jobId"), id}});
}

RcReply refuseIfModified(KdenliveDoc *doc)
{
    return rcFail(QStringLiteral("document_modified"),
                  QStringLiteral("the project has unsaved changes; call project.save or project.discard first%1")
                      .arg(doc->url().isValid() ? QString() : QStringLiteral(" (it is untitled: project.save needs a path)")));
}

RcReply projectSave(const QJsonObject &p)
{
    RcReply err;
    KdenliveDoc *doc = currentDoc(err);
    auto path = doc ? rcString(p, "path", err, QString()) : std::nullopt;
    if (!path) return err;
    QString target = *path;
    if (target.isEmpty()) {
        if (!doc->url().isValid()) return rcFail(QStringLiteral("bad_params"), QStringLiteral("untitled project: pass a path ending in .kdenlive"));
        target = doc->url().toLocalFile();
    }
    if (!target.endsWith(QLatin1String(".kdenlive"))) return rcFail(QStringLiteral("bad_params"), QStringLiteral("path must end in .kdenlive"));
    if (!QFileInfo(QFileInfo(target).absolutePath()).isDir())
        return rcFail(QStringLiteral("not_found"), QStringLiteral("no folder %1").arg(QFileInfo(target).absolutePath()));
    return deferred(QStringLiteral("save"),
                    [target]() { return pCore->projectManager()->saveFileAs(target, true, false) ? QString() : QStringLiteral("save failed"); });
}

RcReply projectDiscard(const QJsonObject &)
{
    RcReply err;
    KdenliveDoc *doc = currentDoc(err);
    if (!doc) return err;
    doc->setModified(false);
    return rcOk(QJsonObject());
}

RcReply projectNew(const QJsonObject &p)
{
    RcReply err;
    KdenliveDoc *doc = currentDoc(err);
    auto profile = doc ? rcString(p, "profile", err, QString()) : std::nullopt;
    if (!profile) return err;
    if (!profile->isEmpty() && !ProfileRepository::get()->profileExists(*profile)) {
        return rcFail(QStringLiteral("not_found"), QStringLiteral("no profile '%1'; see project.profiles").arg(*profile));
    }
    if (doc->isModified()) return refuseIfModified(doc);
    const QString chosen = *profile;
    return deferred(QStringLiteral("new"), [chosen]() {
        chosen.isEmpty() ? pCore->projectManager()->newFile(false) : pCore->projectManager()->newFile(chosen, false);
        return QString();
    });
}

RcReply projectOpen(const QJsonObject &p)
{
    RcReply err;
    KdenliveDoc *doc = currentDoc(err);
    auto path = doc ? rcString(p, "path", err) : std::nullopt;
    if (!path) return err;
    if (!QFileInfo(*path).isFile()) return rcFail(QStringLiteral("not_found"), QStringLiteral("no file %1").arg(*path));
    if (doc->isModified()) return refuseIfModified(doc);
    const QUrl url = QUrl::fromLocalFile(*path);
    return deferred(QStringLiteral("open"), [url]() {
        pCore->projectManager()->openFile(url);
        return QString();
    });
}

RcReply projectProfiles(const QJsonObject &)
{
    QJsonArray out;
    for (const auto &[description, path] : ProfileRepository::get()->getAllProfiles()) {
        auto &profile = ProfileRepository::get()->getProfile(path);
        out.append(QJsonObject{{QStringLiteral("path"), path},
                               {QStringLiteral("description"), description},
                               {QStringLiteral("fps"), profile->fps()},
                               {QStringLiteral("width"), profile->width()},
                               {QStringLiteral("height"), profile->height()}});
    }
    return rcOk(out);
}

RcReply projectSetProfile(const QJsonObject &p)
{
    RcReply err;
    KdenliveDoc *doc = currentDoc(err);
    auto profile = doc ? rcString(p, "profile", err) : std::nullopt;
    if (!profile) return err;
    if (!ProfileRepository::get()->profileExists(*profile)) {
        return rcFail(QStringLiteral("not_found"), QStringLiteral("no profile '%1'; see project.profiles").arg(*profile));
    }
    // private slot: invoke through the meta-object, the same way Kdenlive's own signal connections reach it
    if (!QMetaObject::invokeMethod(doc, "slotSwitchProfile", Q_ARG(QString, *profile), Q_ARG(bool, true))) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("could not switch profile"));
    }
    return rcOk(QJsonObject());
}

RcReply projectGetProperty(const QJsonObject &p)
{
    RcReply err;
    KdenliveDoc *doc = currentDoc(err);
    auto name = doc ? rcString(p, "name", err) : std::nullopt;
    if (!name) return err;
    return rcOk(QJsonObject{{QStringLiteral("value"), doc->getDocumentProperty(*name)}});
}

RcReply projectSetProperty(const QJsonObject &p)
{
    RcReply err;
    KdenliveDoc *doc = currentDoc(err);
    auto name = doc ? rcString(p, "name", err) : std::nullopt;
    auto value = name ? rcString(p, "value", err) : std::nullopt;
    if (!value) return err;
    doc->setDocumentProperty(*name, *value);
    doc->setModified(true);
    return rcOk(QJsonObject());
}

RcReply sequenceList(const QJsonObject &)
{
    RcReply err;
    KdenliveDoc *doc = currentDoc(err);
    if (!doc) return err;
    const QMap<QUuid, QString> clips = pCore->projectItemModel()->getAllSequenceClips();
    QJsonArray out;
    for (const QUuid &uuid : doc->getTimelinesUuids()) {
        const QString binId = clips.value(uuid);
        auto clip = binId.isEmpty() ? nullptr : pCore->projectItemModel()->getClipByBinID(binId);
        out.append(QJsonObject{{QStringLiteral("uuid"), uuid.toString()},
                               {QStringLiteral("binId"), binId},
                               {QStringLiteral("name"), clip ? clip->clipName() : QString()},
                               {QStringLiteral("active"), uuid == doc->activeUuid}});
    }
    return rcOk(out);
}

RcReply sequenceOpen(const QJsonObject &p)
{
    RcReply err;
    KdenliveDoc *doc = currentDoc(err);
    auto uuidText = doc ? rcString(p, "uuid", err) : std::nullopt;
    if (!uuidText) return err;
    const QUuid uuid(*uuidText);
    const QString binId = pCore->projectItemModel()->getAllSequenceClips().value(uuid);
    if (uuid.isNull() || binId.isEmpty()) return rcFail(QStringLiteral("not_found"), QStringLiteral("no sequence %1; see sequence.list").arg(*uuidText));
    if (!pCore->projectManager()->openTimeline(binId, -1, uuid))
        return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive could not open the sequence"));
    return rcOk(QJsonObject());
}

RcReply sequenceCreate(const QJsonObject &p)
{
    RcReply err;
    auto name = rcString(p, "name", err);
    auto video = name ? rcInt(p, "videoTracks", err, 2) : std::nullopt;
    auto audio = video ? rcInt(p, "audioTracks", err, 2) : std::nullopt;
    if (!audio) return err;
    if (*video < 0 || *audio < 0 || *video + *audio == 0) return rcFail(QStringLiteral("bad_params"), QStringLiteral("need at least one track"));
    const QString binId = ClipCreator::createPlaylistClip(*name, {*audio, *video}, QStringLiteral("-1"), pCore->projectItemModel());
    if (binId.isEmpty() || binId == QLatin1String("-1")) return rcFail(QStringLiteral("failed"), QStringLiteral("Kdenlive refused the sequence"));
    const QUuid uuid = pCore->projectItemModel()->getAllSequenceClips().key(binId);
    return rcOk(QJsonObject{{QStringLiteral("uuid"), uuid.toString()}, {QStringLiteral("binId"), binId}});
}

RcReply jobStatus(const QJsonObject &p)
{
    RcReply err;
    auto id = rcInt(p, "jobId", err);
    if (!id) return err;
    const QJsonObject status = rcJobStatus(*id);
    if (status.isEmpty()) return rcFail(QStringLiteral("not_found"), QStringLiteral("no job %1").arg(*id));
    return rcOk(status);
}

void add(RcRegistry &r, const QString &name, const QString &desc, const QList<RcParam> &params, bool mutating, bool undoable, bool needsGui,
         std::function<RcReply(const QJsonObject &)> fn)
{
    r.insert(name, RcCommand{name, desc, params, mutating, undoable, needsGui, std::move(fn)});
}

const QString kString = QStringLiteral("string");
} // namespace

void registerProjectCommands(RcRegistry &r)
{
    add(r, QStringLiteral("project.save"), QStringLiteral("Save the project (path required when untitled) → {jobId}"),
        {{QStringLiteral("path"), kString, false, QStringLiteral(".kdenlive file")}}, false, true, false, projectSave);
    add(r, QStringLiteral("project.discard"), QStringLiteral("Mark the project unmodified so new/open won't prompt (unsaved changes are lost on new/open)"), {},
        false, true, false, projectDiscard);
    add(r, QStringLiteral("project.new"), QStringLiteral("New empty project → {jobId}; refuses with document_modified if unsaved"),
        {{QStringLiteral("profile"), kString, false, QStringLiteral("see project.profiles")}}, true, false, false, projectNew);
    add(r, QStringLiteral("project.open"),
        QStringLiteral("Open a .kdenlive file → {jobId}; refuses with document_modified if unsaved. Kdenlive may still show backup/missing-clip prompts to the "
                       "user"),
        {{QStringLiteral("path"), kString, true, QString()}}, true, false, false, projectOpen);
    add(r, QStringLiteral("project.profiles"), QStringLiteral("Available project profiles"), {}, false, true, false, projectProfiles);
    add(r, QStringLiteral("project.setProfile"), QStringLiteral("Switch the project profile (not undoable)"),
        {{QStringLiteral("profile"), kString, true, QString()}}, true, false, false, projectSetProfile);
    add(r, QStringLiteral("project.getProperty"), QStringLiteral("Read a document property"), {{QStringLiteral("name"), kString, true, QString()}}, false, true,
        false, projectGetProperty);
    add(r, QStringLiteral("project.setProperty"), QStringLiteral("Write a document property"),
        {{QStringLiteral("name"), kString, true, QString()}, {QStringLiteral("value"), kString, true, QString()}}, true, false, false, projectSetProperty);
    add(r, QStringLiteral("sequence.list"), QStringLiteral("Sequences (timelines) in the project"), {}, false, true, false, sequenceList);
    add(r, QStringLiteral("sequence.open"), QStringLiteral("Open/switch to a sequence"), {{QStringLiteral("uuid"), kString, true, QString()}}, false, true,
        true, sequenceOpen);
    add(r, QStringLiteral("sequence.create"), QStringLiteral("Create a sequence"),
        {{QStringLiteral("name"), kString, true, QString()},
         {QStringLiteral("videoTracks"), QStringLiteral("int"), false, QStringLiteral("default 2")},
         {QStringLiteral("audioTracks"), QStringLiteral("int"), false, QStringLiteral("default 2")}},
        true, true, true, sequenceCreate);
    add(r, QStringLiteral("job.status"), QStringLiteral("State of a job from project.save/new/open or render.start"),
        {{QStringLiteral("jobId"), QStringLiteral("int"), true, QString()}}, false, true, false, jobStatus);
}
