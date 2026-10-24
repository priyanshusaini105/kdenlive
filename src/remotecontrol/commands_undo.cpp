/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "core.h"
#include "doc/docundostack.hpp"
#include "rchelpers.h"
#include "remotecontrol.h"

void registerUndoCommands(RcRegistry &r, RemoteControl *rc)
{
    r.insert(QStringLiteral("undo.begin"), {QStringLiteral("undo.begin"),
                                            QStringLiteral("Start grouping following edits into one undo step 'AI: <label>' (auto-closes after idle)"),
                                            {{QStringLiteral("label"), QStringLiteral("string"), true, QString()}},
                                            false,
                                            true,
                                            false,
                                            [rc](const QJsonObject &p) {
                                                RcReply err;
                                                auto label = rcString(p, "label", err);
                                                if (!label) return err;
                                                if (rc->groupOpen())
                                                    return rcFail(QStringLiteral("group_open"), QStringLiteral("an undo group is already open"));
                                                rc->beginGroup(*label);
                                                return rcOk(QJsonObject());
                                            }});
    r.insert(QStringLiteral("undo.end"),
             {QStringLiteral("undo.end"), QStringLiteral("Close the open undo group"), {}, false, true, false, [rc](const QJsonObject &) {
                  if (!rc->groupOpen()) return rcFail(QStringLiteral("no_group"), QStringLiteral("no undo group is open"));
                  rc->endGroup();
                  return rcOk(QJsonObject());
              }});
    auto step = [rc](bool undo) {
        return [rc, undo](const QJsonObject &) {
            rc->endGroup();
            auto stack = pCore->undoStack();
            undo ? stack->undo() : stack->redo();
            return rcOk(QJsonObject{{QStringLiteral("index"), stack->index()}});
        };
    };
    r.insert(QStringLiteral("undo"),
             {QStringLiteral("undo"), QStringLiteral("Undo the last step (closes an open group first)"), {}, true, false, false, step(true)});
    r.insert(QStringLiteral("redo"), {QStringLiteral("redo"), QStringLiteral("Redo the last undone step"), {}, true, false, false, step(false)});
}
