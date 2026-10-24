/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "remotecontrol.h"
#include "core.h"
#include "doc/docundostack.hpp"

#include <QJsonArray>
#include <QJsonDocument>

RemoteControl::RemoteControl(QObject *parent)
    : QObject(parent)
{
    registerTimelineCommands(m_registry);
    registerUndoCommands(m_registry, this);
    registerEffectCommands(m_registry);
    registerActionCommands(m_registry);
    m_idle.setSingleShot(true);
    m_idle.setInterval(10000);
    connect(&m_idle, &QTimer::timeout, this, &RemoteControl::endGroup);
}

void RemoteControl::setIdleTimeoutMs(int ms)
{
    m_idle.setInterval(ms);
}

bool RemoteControl::groupOpen() const
{
    return m_groupOpen;
}

void RemoteControl::beginGroup(const QString &label)
{
    pCore->undoStack()->beginMacro(QStringLiteral("AI: ") + label);
    m_groupOpen = true;
    m_idle.start();
}

void RemoteControl::endGroup()
{
    m_idle.stop();
    if (m_groupOpen) {
        m_groupOpen = false;
        auto stack = pCore->undoStack();
        stack->endMacro();
        // A group in which every command failed would leave a no-op entry that swallows the user's next Ctrl+Z.
        auto *macro = const_cast<QUndoCommand *>(stack->command(stack->index() - 1));
        if (macro && macro->childCount() == 0) {
            macro->setObsolete(true);
            stack->undo(); // obsolete commands are dropped from the stack when undone
        }
    }
}

static QString toJson(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

static QString replyJson(const RcReply &r)
{
    if (r.ok()) {
        return toJson({{QStringLiteral("ok"), true}, {QStringLiteral("result"), r.result}});
    }
    return toJson(
        {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), r.code}, {QStringLiteral("message"), r.message}}}});
}

QString RemoteControl::call(const QString &command, const QString &paramsJson)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(paramsJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return replyJson(rcFail(QStringLiteral("bad_json"), QStringLiteral("params must be a JSON object")));
    }
    if (m_groupOpen) {
        // Any AI activity keeps the group alive; silence closes it so user edits stay separate.
        m_idle.start();
    }
    auto it = m_registry.constFind(command);
    if (it == m_registry.constEnd()) {
        return replyJson(rcFail(QStringLiteral("unknown_command"), QStringLiteral("unknown command '%1'; see listCommands").arg(command)));
    }
    if (it->needsGui && !pCore->window()) {
        return replyJson(rcFail(QStringLiteral("no_gui"), QStringLiteral("'%1' needs the Kdenlive window").arg(command)));
    }
    return replyJson(it->run(doc.object()));
}

QString RemoteControl::listCommands()
{
    QJsonArray list;
    for (const RcCommand &c : std::as_const(m_registry)) {
        QJsonArray params;
        for (const RcParam &p : c.params) {
            params.append(QJsonObject{{QStringLiteral("name"), p.name},
                                      {QStringLiteral("type"), p.type},
                                      {QStringLiteral("required"), p.required},
                                      {QStringLiteral("description"), p.description}});
        }
        list.append(QJsonObject{{QStringLiteral("name"), c.name},
                                {QStringLiteral("description"), c.description},
                                {QStringLiteral("params"), params},
                                {QStringLiteral("mutating"), c.mutating},
                                {QStringLiteral("undoable"), c.undoable},
                                {QStringLiteral("needsGui"), c.needsGui}});
    }
    return QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact));
}
