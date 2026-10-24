/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "rccommand.h"

#include <QObject>

/** @class RemoteControl
    @brief JSON command dispatcher exported on D-Bus at /RemoteControl, so external tools (an MCP server) can drive the editor.
    Calls arrive on the GUI thread. Every reply is {"ok":true,"result":…} or {"ok":false,"error":{"code","message"}}.
 */
class RemoteControl : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdenlive.RemoteControl")

public:
    explicit RemoteControl(QObject *parent = nullptr);

public Q_SLOTS:
    Q_SCRIPTABLE QString call(const QString &command, const QString &paramsJson);
    Q_SCRIPTABLE QString listCommands();

private:
    RcRegistry m_registry;
};
