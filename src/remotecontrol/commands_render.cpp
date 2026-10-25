/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "core.h"
#include "kdenlivesettings.h"
#include "mainwindow.h"
#include "monitor/monitor.h"
#include "rchelpers.h"
#include "render/renderrequest.h"
#include "render/renderserver.h"
#include "renderpresets/renderpresetmodel.hpp"
#include "renderpresets/renderpresetrepository.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>

namespace {
/** jobId → running kdenlive_render processes (several for multi-pass/multi-export). */
QMap<int, QList<QPointer<QProcess>>> &renders()
{
    static QMap<int, QList<QPointer<QProcess>>> table;
    return table;
}

RcReply renderPresets(const QJsonObject &)
{
    QJsonArray out;
    for (const QString &name : RenderPresetRepository::get()->getAllPresets()) {
        auto &preset = RenderPresetRepository::get()->getPreset(name);
        out.append(
            QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("category"), preset->groupId()}, {QStringLiteral("extension"), preset->extension()}});
    }
    return rcOk(out);
}

RcReply renderStart(const QJsonObject &p)
{
    RcReply err;
    auto preset = rcString(p, "preset", err);
    auto output = preset ? rcString(p, "output", err) : std::nullopt;
    if (!output) return err;
    if (!RenderPresetRepository::get()->presetExists(*preset)) {
        return rcFail(QStringLiteral("not_found"), QStringLiteral("no render preset '%1'; see render.presets").arg(*preset));
    }
    if (!QFileInfo(QFileInfo(*output).absolutePath()).isDir()) {
        return rcFail(QStringLiteral("not_found"), QStringLiteral("no folder %1").arg(QFileInfo(*output).absolutePath()));
    }
    int in = -1, out = -1;
    const QJsonValue range = p.value(QStringLiteral("range"));
    if (range.isObject()) {
        in = range.toObject().value(QStringLiteral("in")).toInt(-1);
        out = range.toObject().value(QStringLiteral("out")).toInt(-1);
        if (in < 0 || out <= in) return rcFail(QStringLiteral("bad_params"), QStringLiteral("range {in, out} needs 0 <= in < out (out exclusive)"));
        out -= 1;
    } else if (range.toString() == QLatin1String("zone")) {
        Monitor *m = pCore->getMonitor(Kdenlive::ProjectMonitor);
        in = m->getZoneStart();
        out = m->getZoneEnd() - 1;
    } else if (!range.isUndefined() && range.toString() != QLatin1String("all")) {
        return rcFail(QStringLiteral("bad_params"), QStringLiteral("range must be 'all', 'zone' or {in, out}"));
    }
    if (!pCore->window()) return rcFail(QStringLiteral("no_gui"), QStringLiteral("rendering needs the Kdenlive window"));
    if (KdenliveSettings::kdenliverendererpath().isEmpty() || !QFileInfo::exists(KdenliveSettings::kdenliverendererpath())) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("kdenlive_render not found; check Settings > Environment"));
    }
    RenderRequest request;
    request.setOutputFile(*output);
    request.loadPresetParams(*preset);
    request.setBounds(in, out);
    request.setDelayedRendering(false);
    request.setProxyRendering(false);
    request.setEmbedSubtitles(false);
    request.setTwoPass(false);
    request.setAudioFilePerTrack(false);
    request.setOverlayData(QString());
    const std::vector<RenderRequest::RenderJob> jobs = request.process();
    if (!request.errorMessages().isEmpty() || jobs.empty()) {
        return rcFail(QStringLiteral("failed"), request.errorMessages().join(QLatin1Char('\n')));
    }
    const int id = rcJobStart(QStringLiteral("render"));
    auto *server = pCore->window()->findChild<RenderServer *>();
    QSet<QString> outputs;
    for (const auto &job : jobs) {
        outputs.insert(job.outputPath);
        auto *process = new QProcess(pCore.get());
        QObject::connect(process, &QProcess::finished, process, [id, process](int exitCode, QProcess::ExitStatus status) {
            auto &list = renders()[id];
            const bool allDone = std::all_of(list.begin(), list.end(), [](const QPointer<QProcess> &pr) { return !pr || pr->state() == QProcess::NotRunning; });
            const QJsonObject current = rcJobStatus(id);
            if (current.value(QStringLiteral("state")).toString() == QLatin1String("running")) {
                if (status != QProcess::NormalExit || exitCode != 0) {
                    rcJobFinish(id, QStringLiteral("kdenlive_render exited with code %1").arg(exitCode));
                } else if (allDone) {
                    rcJobFinish(id);
                }
            }
            process->deleteLater();
        });
        renders()[id] << process;
        process->start(KdenliveSettings::kdenliverendererpath(), RenderRequest::argsByJob(job, true));
    }
    if (server) {
        QObject::connect(server, &RenderServer::setRenderingProgress, pCore.get(), [id, outputs](const QString &url, int progress, int) {
            if (outputs.contains(url)) rcJobUpdate(id, progress);
        });
    }
    return rcOk(QJsonObject{{QStringLiteral("jobId"), id}});
}

RcReply renderStatus(const QJsonObject &p)
{
    RcReply err;
    auto id = rcInt(p, "jobId", err);
    if (!id) return err;
    const QJsonObject status = rcJobStatus(*id);
    if (status.isEmpty() || !renders().contains(*id)) return rcFail(QStringLiteral("not_found"), QStringLiteral("no render job %1").arg(*id));
    return rcOk(status);
}

RcReply renderAbort(const QJsonObject &p)
{
    RcReply err;
    auto id = rcInt(p, "jobId", err);
    if (!id) return err;
    if (!renders().contains(*id)) return rcFail(QStringLiteral("not_found"), QStringLiteral("no render job %1").arg(*id));
    rcJobFinish(*id, QStringLiteral("aborted"));
    for (const auto &process : renders().value(*id)) {
        if (process) process->kill();
    }
    return rcOk(QJsonObject());
}
} // namespace

void registerRenderCommands(RcRegistry &r)
{
    const RcParam jobId{QStringLiteral("jobId"), QStringLiteral("int"), true, QString()};
    r.insert(QStringLiteral("render.presets"),
             {QStringLiteral("render.presets"), QStringLiteral("Render presets (name, category, extension)"), {}, false, true, false, renderPresets});
    r.insert(QStringLiteral("render.start"),
             {QStringLiteral("render.start"),
              QStringLiteral("Render the project to a file in the background → {jobId}; overwrites output"),
              {{QStringLiteral("preset"), QStringLiteral("string"), true, QStringLiteral("see render.presets")},
               {QStringLiteral("output"), QStringLiteral("string"), true, QStringLiteral("output file path")},
               {QStringLiteral("range"), QStringLiteral("string|object"), false, QStringLiteral("'all' (default), 'zone' or {in, out} (out exclusive)")}},
              false,
              true,
              false,
              renderStart});
    r.insert(QStringLiteral("render.status"),
             {QStringLiteral("render.status"), QStringLiteral("Render job state and progress 0-100"), {jobId}, false, true, false, renderStatus});
    r.insert(QStringLiteral("render.abort"), {QStringLiteral("render.abort"), QStringLiteral("Stop a render job"), {jobId}, false, true, false, renderAbort});
}
