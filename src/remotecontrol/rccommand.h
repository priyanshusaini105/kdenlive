/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <functional>

/** @brief Result of a remote-control command: either a JSON result or an error code + message. */
struct RcReply
{
    QJsonValue result;
    QString code;
    QString message;
    bool ok() const { return code.isEmpty(); }
};

RcReply rcOk(const QJsonValue &v = QJsonValue());
RcReply rcFail(const QString &code, const QString &message);

struct RcParam
{
    QString name;
    QString type; // int|double|string|bool|object|array
    bool required;
    QString description;
};

struct RcCommand
{
    QString name;
    QString description;
    QList<RcParam> params;
    bool mutating;
    bool undoable;
    bool needsGui;
    std::function<RcReply(const QJsonObject &params)> run;
};

using RcRegistry = QMap<QString, RcCommand>;

class RemoteControl;

void registerTimelineCommands(RcRegistry &r);
void registerUndoCommands(RcRegistry &r, RemoteControl *rc);
void registerEffectCommands(RcRegistry &r);
void registerActionCommands(RcRegistry &r);
void registerClipCommands(RcRegistry &r);
void registerTrackCommands(RcRegistry &r);
void registerMarkerCommands(RcRegistry &r);
void registerMonitorCommands(RcRegistry &r);
void registerAssetCommands(RcRegistry &r);
void registerCompositionCommands(RcRegistry &r);
void registerBinCommands(RcRegistry &r);
void registerTitleCommands(RcRegistry &r);
void registerSubtitleCommands(RcRegistry &r);
void registerMixerCommands(RcRegistry &r);
