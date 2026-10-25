/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "rccommand.h"

#include <QObject>
#include <QTimer>
#include <memory>

class TimelineItemModel;
class TrackModel;

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
    /** @brief While an undo group is open, it closes itself after this many ms without a call (default 10000). */
    void setIdleTimeoutMs(int ms);
    bool groupOpen() const;
    void beginGroup(const QString &label);
    void endGroup();
    /** @brief Track model access for command handlers (TimelineModel befriends this class). */
    static std::shared_ptr<TrackModel> track(const std::shared_ptr<TimelineItemModel> &tl, int tid);

public Q_SLOTS:
    Q_SCRIPTABLE QString call(const QString &command, const QString &paramsJson);
    Q_SCRIPTABLE QString listCommands();

private:
    RcRegistry m_registry;
    QTimer m_idle;
    bool m_groupOpen = false;
};
