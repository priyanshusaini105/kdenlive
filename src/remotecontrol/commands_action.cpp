/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "core.h"
#include "mainwindow.h"
#include "rchelpers.h"

#include <KActionCollection>
#include <QAction>
#include <QApplication>
#include <QJsonArray>
#include <QTimer>

namespace {
RcReply actionList(const QJsonObject &p)
{
    RcReply err;
    auto filter = rcString(p, "filter", err, QString());
    if (!filter) return err;
    QJsonArray out;
    for (QAction *a : pCore->window()->actionCollection()->actions()) {
        const QString name = a->objectName();
        const QString text = a->text().remove(QLatin1Char('&'));
        if (!filter->isEmpty() && !name.contains(*filter, Qt::CaseInsensitive) && !text.contains(*filter, Qt::CaseInsensitive)) {
            continue;
        }
        out.append(QJsonObject{{QStringLiteral("name"), name},
                               {QStringLiteral("text"), text},
                               {QStringLiteral("shortcut"), a->shortcut().toString()},
                               {QStringLiteral("enabled"), a->isEnabled()},
                               {QStringLiteral("checkable"), a->isCheckable()},
                               {QStringLiteral("checked"), a->isChecked()}});
    }
    return rcOk(out);
}

RcReply actionTrigger(const QJsonObject &p)
{
    RcReply err;
    auto name = rcString(p, "name", err);
    if (!name) return err;
    QAction *a = pCore->window()->actionCollection()->action(*name);
    if (!a) {
        return rcFail(QStringLiteral("not_found"), QStringLiteral("no action '%1'; see action.list").arg(*name));
    }
    if (!a->isEnabled()) {
        return rcFail(QStringLiteral("failed"), QStringLiteral("action '%1' is disabled right now").arg(*name));
    }
    // Deferred: the D-Bus reply returns first, so an action that opens a dialog doesn't block the caller.
    QTimer::singleShot(0, a, [a]() {
        if (!QApplication::activeModalWidget()) a->trigger();
    });
    return rcOk(QJsonObject());
}
} // namespace

void registerActionCommands(RcRegistry &r)
{
    r.insert(QStringLiteral("action.list"), {QStringLiteral("action.list"),
                                             QStringLiteral("Registered actions (menus, shortcuts); text ending in '…' opens a dialog"),
                                             {{QStringLiteral("filter"), QStringLiteral("string"), false, QStringLiteral("substring of name or text")}},
                                             false,
                                             true,
                                             true,
                                             actionList});
    r.insert(QStringLiteral("action.trigger"), {QStringLiteral("action.trigger"),
                                                QStringLiteral("Trigger an action by name (asynchronously; a dialog it opens is for the user)"),
                                                {{QStringLiteral("name"), QStringLiteral("string"), true, QString()}},
                                                true,
                                                true,
                                                true,
                                                actionTrigger});
}
