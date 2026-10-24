/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "catch.hpp"
#include "test_utils.hpp"

#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "remotecontrol/remotecontrol.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

static QJsonObject rc(RemoteControl &r, const QString &cmd, const QJsonObject &p = {})
{
    return QJsonDocument::fromJson(r.call(cmd, QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact))).toUtf8()).object();
}

static QString errCode(const QJsonObject &reply)
{
    return reply.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString();
}

static QJsonArray clipsOf(RemoteControl &r)
{
    return rc(r, QStringLiteral("timeline.get")).value(QStringLiteral("result")).toObject().value(QStringLiteral("clips")).toArray();
}

TEST_CASE("RC envelope", "[RemoteControl]")
{
    RemoteControl r;
    auto reply = rc(r, QStringLiteral("nope"));
    REQUIRE(reply.value(QStringLiteral("ok")).toBool() == false);
    REQUIRE(errCode(reply) == QStringLiteral("unknown_command"));

    reply = QJsonDocument::fromJson(r.call(QStringLiteral("timeline.get"), QStringLiteral("{bad")).toUtf8()).object();
    REQUIRE(errCode(reply) == QStringLiteral("bad_json"));

    const QJsonArray cmds = QJsonDocument::fromJson(r.listCommands().toUtf8()).array();
    bool found = false;
    for (const auto &c : cmds) {
        const QJsonObject o = c.toObject();
        if (o.value(QStringLiteral("name")).toString() == QStringLiteral("clip.cut")) {
            found = o.value(QStringLiteral("mutating")).toBool() && o.value(QStringLiteral("undoable")).toBool();
        }
    }
    REQUIRE(found);
}

TEST_CASE("RC timeline.get + clip.cut", "[RemoteControl]")
{
    auto binModel = pCore->projectItemModel();
    std::shared_ptr<DocUndoStack> undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    QDateTime documentDate = QDateTime::currentDateTime();
    KdenliveTests::updateTimeline(false, QString(), QString(), documentDate, 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);

    int tid1;
    REQUIRE(timeline->requestTrackInsertion(-1, tid1));
    QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel);
    int cid1;
    REQUIRE(timeline->requestClipInsertion(binId, tid1, 100, cid1));

    RemoteControl r;
    QJsonArray clips = clipsOf(r);
    REQUIRE(clips.size() == 1);
    REQUIRE(clips.at(0).toObject().value(QStringLiteral("position")).toInt() == 100);
    REQUIRE(clips.at(0).toObject().value(QStringLiteral("duration")).toInt() == 20);

    SECTION("cut, undo, edge, stale id, missing params")
    {
        auto reply = rc(r, QStringLiteral("clip.cut"), {{QStringLiteral("clipId"), cid1}, {QStringLiteral("position"), 110}});
        REQUIRE(reply.value(QStringLiteral("ok")).toBool());
        clips = clipsOf(r);
        REQUIRE(clips.size() == 2);
        REQUIRE(clips.at(0).toObject().value(QStringLiteral("position")).toInt() == 100);
        REQUIRE(clips.at(1).toObject().value(QStringLiteral("position")).toInt() == 110);

        undoStack->undo();
        REQUIRE(clipsOf(r).size() == 1);

        reply = rc(r, QStringLiteral("clip.cut"), {{QStringLiteral("clipId"), cid1}, {QStringLiteral("position"), 100}});
        REQUIRE(errCode(reply) == QStringLiteral("bad_params"));

        REQUIRE(timeline->requestItemDeletion(cid1));
        reply = rc(r, QStringLiteral("clip.cut"), {{QStringLiteral("clipId"), cid1}, {QStringLiteral("position"), 110}});
        REQUIRE(errCode(reply) == QStringLiteral("not_found"));

        reply = rc(r, QStringLiteral("clip.cut"));
        REQUIRE(errCode(reply) == QStringLiteral("bad_params"));
    }
    timeline.reset();
    pCore->projectManager()->closeCurrentDocument(false, false);
}

/** One red 20-frame clip at frame 100 on a fresh track. */
struct RcFixture
{
    std::shared_ptr<DocUndoStack> undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document{undoStack};
    std::shared_ptr<TimelineItemModel> timeline;
    int tid = -1;
    int cid = -1;
    QString binId;
    RcFixture()
    {
        pCore->projectManager()->testSetDocument(&document);
        QDateTime documentDate = QDateTime::currentDateTime();
        KdenliveTests::updateTimeline(false, QString(), QString(), documentDate, 0);
        timeline = document.getTimeline(document.uuid());
        pCore->projectManager()->testSetActiveTimeline(timeline);
        REQUIRE(timeline->requestTrackInsertion(-1, tid));
        binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", pCore->projectItemModel());
        REQUIRE(timeline->requestClipInsertion(binId, tid, 100, cid));
    }
    ~RcFixture()
    {
        timeline.reset();
        pCore->projectManager()->closeCurrentDocument(false, false);
    }
};

static QJsonObject cut(RemoteControl &r, int clipId, int pos)
{
    return rc(r, QStringLiteral("clip.cut"), {{QStringLiteral("clipId"), clipId}, {QStringLiteral("position"), pos}});
}

static void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

TEST_CASE("RC undo groups", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;

    SECTION("a group is one undo step labelled AI: <label>")
    {
        REQUIRE(rc(r, QStringLiteral("undo.begin"), {{QStringLiteral("label"), QStringLiteral("x")}}).value(QStringLiteral("ok")).toBool());
        REQUIRE(cut(r, f.cid, 110).value(QStringLiteral("ok")).toBool());
        REQUIRE(cut(r, f.cid, 105).value(QStringLiteral("ok")).toBool());
        REQUIRE(rc(r, QStringLiteral("undo.end")).value(QStringLiteral("ok")).toBool());
        REQUIRE(clipsOf(r).size() == 3);
        REQUIRE(f.undoStack->text(f.undoStack->index() - 1) == QStringLiteral("AI: x"));
        REQUIRE(rc(r, QStringLiteral("undo")).value(QStringLiteral("ok")).toBool());
        REQUIRE(clipsOf(r).size() == 1);
        REQUIRE(rc(r, QStringLiteral("redo")).value(QStringLiteral("ok")).toBool());
        REQUIRE(clipsOf(r).size() == 3);
    }

    SECTION("nesting and stray end are errors")
    {
        REQUIRE(rc(r, QStringLiteral("undo.begin"), {{QStringLiteral("label"), QStringLiteral("a")}}).value(QStringLiteral("ok")).toBool());
        REQUIRE(errCode(rc(r, QStringLiteral("undo.begin"), {{QStringLiteral("label"), QStringLiteral("b")}})) == QStringLiteral("group_open"));
        REQUIRE(rc(r, QStringLiteral("undo.end")).value(QStringLiteral("ok")).toBool());
        REQUIRE(errCode(rc(r, QStringLiteral("undo.end"))) == QStringLiteral("no_group"));
    }

    SECTION("idle group auto-closes so a user edit is its own step")
    {
        r.setIdleTimeoutMs(50);
        REQUIRE(rc(r, QStringLiteral("undo.begin"), {{QStringLiteral("label"), QStringLiteral("idle")}}).value(QStringLiteral("ok")).toBool());
        REQUIRE(cut(r, f.cid, 110).value(QStringLiteral("ok")).toBool());
        waitMs(150);
        REQUIRE(f.timeline->requestClipMove(f.cid, f.tid, 50));
        f.undoStack->undo(); // user move
        REQUIRE(clipsOf(r).size() == 2);
        REQUIRE(clipsOf(r).at(0).toObject().value(QStringLiteral("position")).toInt() == 100);
        f.undoStack->undo(); // AI cut group
        REQUIRE(clipsOf(r).size() == 1);
    }
}
