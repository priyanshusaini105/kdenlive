/*
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "catch.hpp"
#include "test_utils.hpp"

#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "remotecontrol/rctitle.h"
#include "remotecontrol/remotecontrol.h"

#include <QApplication>
#include <QDialog>
#include <QDomDocument>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
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

    SECTION("an empty group leaves no undo entry")
    {
        const int count = f.undoStack->count();
        REQUIRE(rc(r, QStringLiteral("undo.begin"), {{QStringLiteral("label"), QStringLiteral("empty")}}).value(QStringLiteral("ok")).toBool());
        REQUIRE(errCode(cut(r, f.cid, 100)) == QStringLiteral("bad_params")); // fails, pushes nothing
        REQUIRE(rc(r, QStringLiteral("undo.end")).value(QStringLiteral("ok")).toBool());
        REQUIRE(f.undoStack->count() == count);
    }

    SECTION("idle group auto-closes so a user edit is its own step")
    {
        r.setIdleTimeoutMs(50);
        REQUIRE(rc(r, QStringLiteral("undo.begin"), {{QStringLiteral("label"), QStringLiteral("idle")}}).value(QStringLiteral("ok")).toBool());
        REQUIRE(cut(r, f.cid, 110).value(QStringLiteral("ok")).toBool());
        waitMs(500);
        REQUIRE(f.timeline->requestClipMove(f.cid, f.tid, 50));
        f.undoStack->undo(); // user move
        REQUIRE(clipsOf(r).size() == 2);
        REQUIRE(clipsOf(r).at(0).toObject().value(QStringLiteral("position")).toInt() == 100);
        f.undoStack->undo(); // AI cut group
        REQUIRE(clipsOf(r).size() == 1);
    }
}

static QJsonObject clipTarget(int cid)
{
    return {{QStringLiteral("type"), QStringLiteral("clip")}, {QStringLiteral("id"), cid}};
}

static QString paramValue(const QJsonObject &getReply, int index, const QString &name)
{
    const QJsonArray effects = getReply.value(QStringLiteral("result")).toArray();
    for (const auto &p : effects.at(index).toObject().value(QStringLiteral("params")).toArray()) {
        if (p.toObject().value(QStringLiteral("name")).toString() == name) {
            return p.toObject().value(QStringLiteral("value")).toString();
        }
    }
    return QStringLiteral("<missing>");
}

TEST_CASE("RC effects", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    const QJsonObject target = clipTarget(f.cid);

    auto added = rc(r, QStringLiteral("effect.add"), {{QStringLiteral("target"), target}, {QStringLiteral("effectId"), QStringLiteral("sepia")}});
    REQUIRE(added.value(QStringLiteral("ok")).toBool());
    REQUIRE(added.value(QStringLiteral("result")).toObject().value(QStringLiteral("index")).toInt() == 0);

    auto got = rc(r, QStringLiteral("effect.get"), {{QStringLiteral("target"), target}});
    REQUIRE(got.value(QStringLiteral("result")).toArray().size() == 1);
    REQUIRE(got.value(QStringLiteral("result")).toArray().at(0).toObject().value(QStringLiteral("effectId")).toString() == QStringLiteral("sepia"));
    const QString before = paramValue(got, 0, QStringLiteral("u"));
    REQUIRE(before != QStringLiteral("<missing>"));

    SECTION("set and undo a parameter")
    {
        auto set = rc(r, QStringLiteral("effect.set"),
                      {{QStringLiteral("target"), target},
                       {QStringLiteral("index"), 0},
                       {QStringLiteral("params"), QJsonObject{{QStringLiteral("u"), QStringLiteral("100")}}}});
        REQUIRE(set.value(QStringLiteral("ok")).toBool());
        REQUIRE(paramValue(rc(r, QStringLiteral("effect.get"), {{QStringLiteral("target"), target}}), 0, QStringLiteral("u")) == QStringLiteral("100"));
        f.undoStack->undo();
        REQUIRE(paramValue(rc(r, QStringLiteral("effect.get"), {{QStringLiteral("target"), target}}), 0, QStringLiteral("u")) == before);
    }

    SECTION("errors")
    {
        REQUIRE(errCode(rc(r, QStringLiteral("effect.add"),
                           {{QStringLiteral("target"), target}, {QStringLiteral("effectId"), QStringLiteral("no_such_fx")}})) == QStringLiteral("not_found"));
        REQUIRE(errCode(rc(r, QStringLiteral("effect.set"),
                           {{QStringLiteral("target"), target},
                            {QStringLiteral("index"), 5},
                            {QStringLiteral("params"), QJsonObject{{QStringLiteral("u"), QStringLiteral("1")}}}})) == QStringLiteral("bad_params"));
        REQUIRE(errCode(rc(r, QStringLiteral("effect.set"),
                           {{QStringLiteral("target"), target},
                            {QStringLiteral("index"), 0},
                            {QStringLiteral("params"), QJsonObject{{QStringLiteral("nope"), QStringLiteral("1")}}}})) == QStringLiteral("bad_params"));
        REQUIRE(f.timeline->requestItemDeletion(f.cid));
        REQUIRE(errCode(rc(r, QStringLiteral("effect.get"), {{QStringLiteral("target"), target}})) == QStringLiteral("not_found"));
    }
}

TEST_CASE("RC actions need the GUI", "[RemoteControl]")
{
    RemoteControl r;
    REQUIRE(errCode(rc(r, QStringLiteral("action.list"))) == QStringLiteral("no_gui"));
    REQUIRE(errCode(rc(r, QStringLiteral("action.trigger"), {{QStringLiteral("name"), QStringLiteral("edit_undo")}})) == QStringLiteral("no_gui"));
}

static int clipPos(RemoteControl &r, int cid)
{
    for (const auto &c : clipsOf(r)) {
        if (c.toObject().value(QStringLiteral("id")).toInt() == cid) return c.toObject().value(QStringLiteral("position")).toInt();
    }
    return -1;
}

TEST_CASE("RC clip editing", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    auto ok = [](const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); };

    SECTION("move + undo, stale id")
    {
        REQUIRE(
            ok(rc(r, QStringLiteral("clip.move"), {{QStringLiteral("clipId"), f.cid}, {QStringLiteral("trackId"), f.tid}, {QStringLiteral("position"), 300}})));
        REQUIRE(clipPos(r, f.cid) == 300);
        f.undoStack->undo();
        REQUIRE(clipPos(r, f.cid) == 100);
        REQUIRE(f.timeline->requestItemDeletion(f.cid));
        REQUIRE(errCode(rc(r, QStringLiteral("clip.move"),
                           {{QStringLiteral("clipId"), f.cid}, {QStringLiteral("trackId"), f.tid}, {QStringLiteral("position"), 0}})) ==
                QStringLiteral("not_found"));
    }

    SECTION("locked track refuses with a message")
    {
        f.timeline->setTrackLockedState(f.tid, true);
        auto reply =
            rc(r, QStringLiteral("clip.move"), {{QStringLiteral("clipId"), f.cid}, {QStringLiteral("trackId"), f.tid}, {QStringLiteral("position"), 300}});
        REQUIRE(errCode(reply) == QStringLiteral("failed"));
        REQUIRE(reply.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().contains(QStringLiteral("locked")));
        REQUIRE(clipPos(r, f.cid) == 100);
    }

    SECTION("occupied spot, insert, resize, delete, speed")
    {
        auto ins =
            rc(r, QStringLiteral("clip.insert"), {{QStringLiteral("binId"), f.binId}, {QStringLiteral("trackId"), f.tid}, {QStringLiteral("position"), 200}});
        REQUIRE(ok(ins));
        const int cid2 = ins.value(QStringLiteral("result")).toObject().value(QStringLiteral("clipId")).toInt();
        REQUIRE(clipPos(r, cid2) == 200);
        REQUIRE(errCode(rc(r, QStringLiteral("clip.move"),
                           {{QStringLiteral("clipId"), cid2}, {QStringLiteral("trackId"), f.tid}, {QStringLiteral("position"), 105}})) ==
                QStringLiteral("failed"));
        REQUIRE(ok(rc(r, QStringLiteral("clip.resize"), {{QStringLiteral("clipId"), f.cid}, {QStringLiteral("duration"), 10}})));
        REQUIRE(f.timeline->getClipPlaytime(f.cid) == 10);
        REQUIRE(errCode(rc(r, QStringLiteral("clip.speed"), {{QStringLiteral("clipId"), f.cid}, {QStringLiteral("speed"), 0}})) ==
                QStringLiteral("bad_params"));
        REQUIRE(ok(rc(r, QStringLiteral("clip.delete"), {{QStringLiteral("clipId"), cid2}})));
        REQUIRE(clipsOf(r).size() == 1);
    }

    SECTION("group and select")
    {
        auto ins =
            rc(r, QStringLiteral("clip.insert"), {{QStringLiteral("binId"), f.binId}, {QStringLiteral("trackId"), f.tid}, {QStringLiteral("position"), 200}});
        const int cid2 = ins.value(QStringLiteral("result")).toObject().value(QStringLiteral("clipId")).toInt();
        auto g = rc(r, QStringLiteral("group.create"), {{QStringLiteral("ids"), QJsonArray{f.cid, cid2}}});
        REQUIRE(ok(g));
        REQUIRE(f.timeline->isInGroup(f.cid));
        REQUIRE(ok(rc(r, QStringLiteral("timeline.select"), {{QStringLiteral("ids"), QJsonArray{cid2}}})));
        REQUIRE(!rc(r, QStringLiteral("timeline.get")).value(QStringLiteral("result")).toObject().value(QStringLiteral("selection")).toArray().isEmpty());
        REQUIRE(errCode(rc(r, QStringLiteral("group.create"), {{QStringLiteral("ids"), QJsonArray{f.cid, 9999}}})) == QStringLiteral("not_found"));
    }
}

static QJsonObject trackOf(RemoteControl &r, int tid)
{
    for (const auto &t : rc(r, QStringLiteral("timeline.get")).value(QStringLiteral("result")).toObject().value(QStringLiteral("tracks")).toArray()) {
        if (t.toObject().value(QStringLiteral("id")).toInt() == tid) return t.toObject();
    }
    return {};
}

TEST_CASE("RC tracks", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    auto ok = [](const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); };

    auto added = rc(r, QStringLiteral("track.add"), {{QStringLiteral("audio"), true}, {QStringLiteral("name"), QStringLiteral("Music")}});
    REQUIRE(ok(added));
    const int atid = added.value(QStringLiteral("result")).toObject().value(QStringLiteral("trackId")).toInt();
    REQUIRE(trackOf(r, atid).value(QStringLiteral("audio")).toBool());

    REQUIRE(ok(rc(r, QStringLiteral("track.rename"), {{QStringLiteral("trackId"), atid}, {QStringLiteral("name"), QStringLiteral("Score")}})));
    REQUIRE(trackOf(r, atid).value(QStringLiteral("name")).toString().contains(QStringLiteral("Score")));

    REQUIRE(ok(rc(r, QStringLiteral("track.lock"), {{QStringLiteral("trackId"), f.tid}, {QStringLiteral("locked"), true}})));
    REQUIRE(trackOf(r, f.tid).value(QStringLiteral("locked")).toBool());
    f.undoStack->undo();
    REQUIRE(!trackOf(r, f.tid).value(QStringLiteral("locked")).toBool());

    REQUIRE(ok(rc(r, QStringLiteral("track.hide"), {{QStringLiteral("trackId"), f.tid}, {QStringLiteral("hidden"), true}})));
    REQUIRE(trackOf(r, f.tid).value(QStringLiteral("hidden")).toBool());
    f.undoStack->undo();
    REQUIRE(!trackOf(r, f.tid).value(QStringLiteral("hidden")).toBool());
    REQUIRE(errCode(rc(r, QStringLiteral("track.hide"), {{QStringLiteral("trackId"), atid}, {QStringLiteral("hidden"), true}})) ==
            QStringLiteral("bad_params"));

    REQUIRE(ok(rc(r, QStringLiteral("track.mute"), {{QStringLiteral("trackId"), atid}, {QStringLiteral("muted"), true}})));
    REQUIRE(trackOf(r, atid).value(QStringLiteral("muted")).toBool());

    REQUIRE(ok(rc(r, QStringLiteral("track.delete"), {{QStringLiteral("trackId"), atid}})));
    REQUIRE(trackOf(r, atid).isEmpty());
    REQUIRE(errCode(rc(r, QStringLiteral("track.rename"), {{QStringLiteral("trackId"), atid}, {QStringLiteral("name"), QStringLiteral("x")}})) ==
            QStringLiteral("not_found"));
}

TEST_CASE("RC markers and guides", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    auto ok = [](const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); };
    const QJsonObject guides{{QStringLiteral("scope"), QStringLiteral("guides")}};
    auto list = [&](const QJsonObject &scope) { return rc(r, QStringLiteral("marker.list"), scope).value(QStringLiteral("result")).toArray(); };
    auto with = [](QJsonObject o, const QJsonObject &extra) {
        for (auto it = extra.begin(); it != extra.end(); ++it)
            o.insert(it.key(), it.value());
        return o;
    };

    REQUIRE(ok(rc(r, QStringLiteral("marker.add"), with(guides, {{QStringLiteral("position"), 50}, {QStringLiteral("comment"), QStringLiteral("intro")}}))));
    REQUIRE(list(guides).size() == 1);
    REQUIRE(list(guides).at(0).toObject().value(QStringLiteral("position")).toInt() == 50);
    REQUIRE(list(guides).at(0).toObject().value(QStringLiteral("comment")).toString() == QStringLiteral("intro"));

    REQUIRE(
        ok(rc(r, QStringLiteral("marker.edit"),
              with(guides, {{QStringLiteral("position"), 50}, {QStringLiteral("newPosition"), 60}, {QStringLiteral("comment"), QStringLiteral("start")}}))));
    REQUIRE(list(guides).at(0).toObject().value(QStringLiteral("position")).toInt() == 60);
    REQUIRE(list(guides).at(0).toObject().value(QStringLiteral("comment")).toString() == QStringLiteral("start"));

    REQUIRE(errCode(rc(r, QStringLiteral("marker.remove"), with(guides, {{QStringLiteral("position"), 999}}))) == QStringLiteral("not_found"));
    REQUIRE(ok(rc(r, QStringLiteral("marker.remove"), with(guides, {{QStringLiteral("position"), 60}}))));
    REQUIRE(list(guides).isEmpty());
    f.undoStack->undo();
    REQUIRE(list(guides).size() == 1);

    const QJsonObject clipScope{{QStringLiteral("scope"), QStringLiteral("clip")}, {QStringLiteral("binId"), f.binId}};
    REQUIRE(ok(rc(r, QStringLiteral("marker.add"), with(clipScope, {{QStringLiteral("position"), 5}, {QStringLiteral("duration"), 4}}))));
    REQUIRE(list(clipScope).at(0).toObject().value(QStringLiteral("duration")).toInt() == 4);
    REQUIRE(errCode(rc(r, QStringLiteral("marker.list"),
                       {{QStringLiteral("scope"), QStringLiteral("clip")}, {QStringLiteral("binId"), QStringLiteral("nope")}})) == QStringLiteral("not_found"));
    REQUIRE(rc(r, QStringLiteral("marker.categories")).value(QStringLiteral("result")).toArray().size() > 0);
    REQUIRE(errCode(rc(r, QStringLiteral("zone.get"))) == QStringLiteral("no_gui"));
}

TEST_CASE("RC monitor needs the GUI", "[RemoteControl]")
{
    RemoteControl r;
    REQUIRE(errCode(rc(r, QStringLiteral("monitor.seek"), {{QStringLiteral("position"), 10}})) == QStringLiteral("no_gui"));
    REQUIRE(errCode(rc(r, QStringLiteral("monitor.play"), {{QStringLiteral("play"), true}})) == QStringLiteral("no_gui"));
    REQUIRE(errCode(rc(r, QStringLiteral("monitor.position"))) == QStringLiteral("no_gui"));
}

TEST_CASE("RC asset catalog", "[RemoteControl]")
{
    RemoteControl r;
    const QJsonArray effects =
        rc(r, QStringLiteral("asset.list"), {{QStringLiteral("kind"), QStringLiteral("effect")}, {QStringLiteral("filter"), QStringLiteral("sepia")}})
            .value(QStringLiteral("result"))
            .toArray();
    bool hasSepia = false;
    for (const auto &e : effects) {
        hasSepia |= e.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("sepia");
    }
    REQUIRE(hasSepia);
    REQUIRE(!rc(r, QStringLiteral("asset.list"), {{QStringLiteral("kind"), QStringLiteral("transition")}}).value(QStringLiteral("result")).toArray().isEmpty());

    const QJsonObject schema =
        rc(r, QStringLiteral("asset.schema"), {{QStringLiteral("kind"), QStringLiteral("effect")}, {QStringLiteral("id"), QStringLiteral("sepia")}})
            .value(QStringLiteral("result"))
            .toObject();
    bool hasU = false;
    for (const auto &p : schema.value(QStringLiteral("params")).toArray()) {
        if (p.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("u")) {
            hasU = true;
            REQUIRE(p.toObject().value(QStringLiteral("animated")).toBool());
            REQUIRE(p.toObject().value(QStringLiteral("default")).toString() == QStringLiteral("75"));
        }
    }
    REQUIRE(hasU);
    REQUIRE(errCode(rc(r, QStringLiteral("asset.schema"),
                       {{QStringLiteral("kind"), QStringLiteral("effect")}, {QStringLiteral("id"), QStringLiteral("nope")}})) == QStringLiteral("not_found"));
    REQUIRE(errCode(rc(r, QStringLiteral("asset.list"), {{QStringLiteral("kind"), QStringLiteral("filter")}})) == QStringLiteral("bad_params"));
}

static QStringList effectIds(RemoteControl &r, const QJsonObject &target)
{
    QStringList ids;
    for (const auto &e : rc(r, QStringLiteral("effect.get"), {{QStringLiteral("target"), target}}).value(QStringLiteral("result")).toArray()) {
        ids << e.toObject().value(QStringLiteral("effectId")).toString();
    }
    return ids;
}

TEST_CASE("RC effect stack and keyframes", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    auto ok = [](const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); };
    const QJsonObject target = clipTarget(f.cid);
    REQUIRE(ok(rc(r, QStringLiteral("effect.add"), {{QStringLiteral("target"), target}, {QStringLiteral("effectId"), QStringLiteral("sepia")}})));
    REQUIRE(ok(rc(r, QStringLiteral("effect.add"), {{QStringLiteral("target"), target}, {QStringLiteral("effectId"), QStringLiteral("crop")}})));
    REQUIRE(effectIds(r, target) == QStringList{QStringLiteral("sepia"), QStringLiteral("crop")});

    SECTION("move, enable, remove")
    {
        REQUIRE(ok(rc(r, QStringLiteral("effect.move"), {{QStringLiteral("target"), target}, {QStringLiteral("from"), 0}, {QStringLiteral("to"), 1}})));
        REQUIRE(effectIds(r, target) == QStringList{QStringLiteral("crop"), QStringLiteral("sepia")});
        f.undoStack->undo();
        REQUIRE(effectIds(r, target) == QStringList{QStringLiteral("sepia"), QStringLiteral("crop")});

        REQUIRE(
            ok(rc(r, QStringLiteral("effect.enable"), {{QStringLiteral("target"), target}, {QStringLiteral("index"), 0}, {QStringLiteral("enabled"), false}})));
        REQUIRE(!rc(r, QStringLiteral("effect.get"), {{QStringLiteral("target"), target}})
                     .value(QStringLiteral("result"))
                     .toArray()
                     .at(0)
                     .toObject()
                     .value(QStringLiteral("enabled"))
                     .toBool());

        REQUIRE(ok(rc(r, QStringLiteral("effect.remove"), {{QStringLiteral("target"), target}, {QStringLiteral("index"), 1}})));
        REQUIRE(effectIds(r, target) == QStringList{QStringLiteral("sepia")});
        f.undoStack->undo();
        REQUIRE(effectIds(r, target).size() == 2);
    }

    SECTION("keyframes are item-local, undoable, typed")
    {
        const QJsonObject kf{{QStringLiteral("target"), target}, {QStringLiteral("index"), 0}, {QStringLiteral("param"), QStringLiteral("u")}};
        auto with = [](QJsonObject o, const QJsonObject &extra) {
            for (auto it = extra.begin(); it != extra.end(); ++it)
                o.insert(it.key(), it.value());
            return o;
        };
        auto frames = [&]() {
            QList<int> out;
            for (const auto &k : rc(r, QStringLiteral("keyframe.list"), kf).value(QStringLiteral("result")).toArray()) {
                out << k.toObject().value(QStringLiteral("frame")).toInt();
            }
            return out;
        };
        REQUIRE(frames().contains(0)); // clip at timeline 100 → first keyframe at item-local 0
        REQUIRE(ok(
            rc(r, QStringLiteral("keyframe.set"),
               with(kf,
                    {{QStringLiteral("frame"), 10}, {QStringLiteral("value"), QStringLiteral("200")}, {QStringLiteral("interp"), QStringLiteral("smooth")}}))));
        REQUIRE(frames().contains(10));
        bool found = false;
        for (const auto &k : rc(r, QStringLiteral("keyframe.list"), kf).value(QStringLiteral("result")).toArray()) {
            if (k.toObject().value(QStringLiteral("frame")).toInt() == 10) {
                found = true;
                REQUIRE(k.toObject().value(QStringLiteral("value")).toString().toDouble() == 200.);
                REQUIRE(k.toObject().value(QStringLiteral("interp")).toString() == QStringLiteral("smooth"));
            }
        }
        REQUIRE(found);
        f.undoStack->undo();
        REQUIRE(!frames().contains(10));

        REQUIRE(ok(rc(r, QStringLiteral("keyframe.set"), with(kf, {{QStringLiteral("frame"), 5}, {QStringLiteral("value"), QStringLiteral("50")}}))));
        REQUIRE(ok(rc(r, QStringLiteral("keyframe.move"), with(kf, {{QStringLiteral("frame"), 5}, {QStringLiteral("newFrame"), 8}}))));
        REQUIRE(frames().contains(8));
        REQUIRE(ok(rc(r, QStringLiteral("keyframe.remove"), with(kf, {{QStringLiteral("frame"), 8}}))));
        REQUIRE(!frames().contains(8));
        REQUIRE(errCode(rc(r, QStringLiteral("keyframe.remove"), with(kf, {{QStringLiteral("frame"), 8}}))) == QStringLiteral("not_found"));
        REQUIRE(errCode(rc(r, QStringLiteral("keyframe.set"),
                           with(kf, {{QStringLiteral("frame"), 3},
                                     {QStringLiteral("value"), QStringLiteral("1")},
                                     {QStringLiteral("interp"), QStringLiteral("wobbly")}}))) == QStringLiteral("bad_params"));
        REQUIRE(errCode(rc(r, QStringLiteral("keyframe.set"),
                           {{QStringLiteral("target"), target},
                            {QStringLiteral("index"), 1},
                            {QStringLiteral("param"), QStringLiteral("top")},
                            {QStringLiteral("frame"), 0},
                            {QStringLiteral("value"), QStringLiteral("1")}})) == QStringLiteral("bad_params"));
    }
}

TEST_CASE("RC compositions and mixes", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    auto ok = [](const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); };
    auto tlGet = [&]() { return rc(r, QStringLiteral("timeline.get")).value(QStringLiteral("result")).toObject(); };

    SECTION("composition add, params, undo, stale id")
    {
        int tid2;
        REQUIRE(f.timeline->requestTrackInsertion(-1, tid2));
        auto added = rc(r, QStringLiteral("composition.add"),
                        {{QStringLiteral("transitionId"), QStringLiteral("composite")},
                         {QStringLiteral("trackId"), tid2},
                         {QStringLiteral("position"), 100},
                         {QStringLiteral("duration"), 20}});
        REQUIRE(ok(added));
        const int compo = added.value(QStringLiteral("result")).toObject().value(QStringLiteral("compositionId")).toInt();
        REQUIRE(tlGet().value(QStringLiteral("compositions")).toArray().size() == 1);
        REQUIRE(!rc(r, QStringLiteral("composition.get"), {{QStringLiteral("compositionId"), compo}})
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("params"))
                     .toArray()
                     .isEmpty());
        REQUIRE(ok(rc(r, QStringLiteral("composition.move"),
                      {{QStringLiteral("compositionId"), compo}, {QStringLiteral("trackId"), tid2}, {QStringLiteral("position"), 110}})));
        f.undoStack->undo();
        f.undoStack->undo();
        REQUIRE(tlGet().value(QStringLiteral("compositions")).toArray().isEmpty());
        REQUIRE(errCode(rc(r, QStringLiteral("composition.delete"), {{QStringLiteral("compositionId"), compo}})) == QStringLiteral("not_found"));
        REQUIRE(errCode(rc(r, QStringLiteral("composition.add"),
                           {{QStringLiteral("transitionId"), QStringLiteral("nope")},
                            {QStringLiteral("trackId"), tid2},
                            {QStringLiteral("position"), 0},
                            {QStringLiteral("duration"), 10}})) == QStringLiteral("not_found"));
    }

    SECTION("same-track mix")
    {
        QString longBin = KdenliveTests::createProducer(pCore->getProjectProfile(), "blue", pCore->projectItemModel(), 100, true);
        int a, b;
        REQUIRE(f.timeline->requestClipInsertion(longBin, f.tid, 300, a));
        REQUIRE(f.timeline->requestItemResize(a, 50, true) == 50);
        REQUIRE(f.timeline->requestClipInsertion(longBin, f.tid, 350, b));
        REQUIRE(errCode(rc(r, QStringLiteral("mix.add"), {{QStringLiteral("clipId"), f.cid}})) == QStringLiteral("failed"));
        REQUIRE(ok(rc(r, QStringLiteral("mix.add"), {{QStringLiteral("clipId"), b}})));
        REQUIRE(tlGet().value(QStringLiteral("mixes")).toArray().size() == 1);
        REQUIRE(tlGet().value(QStringLiteral("mixes")).toArray().at(0).toObject().value(QStringLiteral("clipId")).toInt() == b);
        REQUIRE(!rc(r, QStringLiteral("mix.get"), {{QStringLiteral("clipId"), b}})
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("params"))
                     .toArray()
                     .isEmpty());
        REQUIRE(ok(rc(r, QStringLiteral("mix.remove"), {{QStringLiteral("clipId"), b}})));
        REQUIRE(tlGet().value(QStringLiteral("mixes")).toArray().isEmpty());
        REQUIRE(errCode(rc(r, QStringLiteral("mix.remove"), {{QStringLiteral("clipId"), b}})) == QStringLiteral("not_found"));
    }
}

TEST_CASE("RC refuses calls while a modal dialog is open", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    QDialog dialog;
    dialog.setModal(true);
    dialog.show();
    QApplication::processEvents();
    REQUIRE(QApplication::activeModalWidget() == &dialog);
    auto reply = rc(r, QStringLiteral("timeline.get"));
    REQUIRE(errCode(reply) == QStringLiteral("failed"));
    REQUIRE(reply.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().contains(QStringLiteral("dialog")));
    dialog.close();
    QApplication::processEvents();
    REQUIRE(rc(r, QStringLiteral("timeline.get")).value(QStringLiteral("ok")).toBool());
}

TEST_CASE("RC bin", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    auto ok = [](const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); };
    auto binClips = [&]() { return rc(r, QStringLiteral("bin.list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("clips")).toArray(); };
    auto clipById = [&](const QString &id) {
        for (const auto &c : binClips()) {
            if (c.toObject().value(QStringLiteral("binId")).toString() == id) return c.toObject();
        }
        return QJsonObject();
    };

    REQUIRE(!clipById(f.binId).isEmpty());
    REQUIRE(errCode(rc(r, QStringLiteral("bin.import"), {{QStringLiteral("paths"), QJsonArray{QStringLiteral("/nonexistent/x.mp4")}}})) ==
            QStringLiteral("not_found"));

    auto imported = rc(r, QStringLiteral("bin.import"), {{QStringLiteral("paths"), QJsonArray{QString(sourcesPath + QStringLiteral("/dataset/blue.mp4"))}}});
    REQUIRE(ok(imported));
    const QString importedId = imported.value(QStringLiteral("result")).toObject().value(QStringLiteral("binIds")).toArray().at(0).toString();
    REQUIRE(!clipById(importedId).isEmpty());

    auto color = rc(r, QStringLiteral("bin.color"),
                    {{QStringLiteral("color"), QStringLiteral("#ff0000")}, {QStringLiteral("duration"), 50}, {QStringLiteral("name"), QStringLiteral("Red")}});
    REQUIRE(ok(color));
    const QString colorId = color.value(QStringLiteral("result")).toObject().value(QStringLiteral("binId")).toString();
    for (int i = 0; i < 500 && !clipById(colorId).value(QStringLiteral("ready")).toBool(); ++i) {
        waitMs(20); // clips load asynchronously, as the MCP side polls 'ready'
    }
    REQUIRE(clipById(colorId).value(QStringLiteral("duration")).toInt() == 50);

    auto folder = rc(r, QStringLiteral("bin.folder.create"), {{QStringLiteral("name"), QStringLiteral("Music")}});
    REQUIRE(ok(folder));
    const QString folderId = folder.value(QStringLiteral("result")).toObject().value(QStringLiteral("folderId")).toString();
    bool hasFolder = false;
    for (const auto &fo : rc(r, QStringLiteral("bin.list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("folders")).toArray()) {
        hasFolder |= fo.toObject().value(QStringLiteral("id")).toString() == folderId &&
                     fo.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Music");
    }
    REQUIRE(hasFolder);

    // clip renames go through the Bin widget (absent headless); folder renames don't
    REQUIRE(errCode(rc(r, QStringLiteral("bin.rename"), {{QStringLiteral("binId"), colorId}, {QStringLiteral("name"), QStringLiteral("Crimson")}})) ==
            QStringLiteral("no_gui"));
    REQUIRE(ok(rc(r, QStringLiteral("bin.rename"), {{QStringLiteral("binId"), folderId}, {QStringLiteral("name"), QStringLiteral("Score")}})));
    bool renamed = false;
    for (const auto &fo : rc(r, QStringLiteral("bin.list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("folders")).toArray()) {
        renamed |= fo.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Score");
    }
    REQUIRE(renamed);

    REQUIRE(rc(r, QStringLiteral("bin.properties.get"), {{QStringLiteral("binId"), colorId}, {QStringLiteral("keys"), QJsonArray{QStringLiteral("resource")}}})
                .value(QStringLiteral("result"))
                .toObject()
                .contains(QStringLiteral("resource")));

    REQUIRE(ok(rc(r, QStringLiteral("bin.delete"), {{QStringLiteral("binId"), colorId}})));
    REQUIRE(clipById(colorId).isEmpty());
    f.undoStack->undo();
    REQUIRE(!clipById(colorId).isEmpty());
    REQUIRE(errCode(rc(r, QStringLiteral("bin.delete"), {{QStringLiteral("binId"), QStringLiteral("9999")}})) == QStringLiteral("not_found"));
}

TEST_CASE("RC titles", "[RemoteControl]")
{
    SECTION("spec to kdenlivetitle xml")
    {
        QString error;
        const QString xml = rcTitleXml(QJsonObject{{QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                                                    {QStringLiteral("text"), QStringLiteral("Hello ✓")},
                                                                                                    {QStringLiteral("x"), 100},
                                                                                                    {QStringLiteral("y"), 200}}}}},
                                       1920, 1080, 125, &error);
        REQUIRE(error.isEmpty());
        QDomDocument doc;
        REQUIRE(bool(doc.setContent(xml)));
        REQUIRE(doc.documentElement().tagName() == QStringLiteral("kdenlivetitle"));
        REQUIRE(doc.documentElement().attribute(QStringLiteral("width")) == QStringLiteral("1920"));
        const QDomNodeList items = doc.elementsByTagName(QStringLiteral("item"));
        REQUIRE(items.count() == 1);
        REQUIRE(items.item(0).toElement().attribute(QStringLiteral("type")) == QStringLiteral("QGraphicsTextItem"));
        const QDomElement content = items.item(0).firstChildElement(QStringLiteral("content"));
        REQUIRE(content.text() == QStringLiteral("Hello ✓"));
        REQUIRE(content.attribute(QStringLiteral("font-color")) == QStringLiteral("255,255,255,255"));
        REQUIRE(items.item(0).firstChildElement(QStringLiteral("position")).attribute(QStringLiteral("x")) == QStringLiteral("100"));

        rcTitleXml(QJsonObject{{QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("blob")}}}}}, 1920, 1080, 125, &error);
        REQUIRE(!error.isEmpty());
        rcTitleXml(QJsonObject{{QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                                {QStringLiteral("text"), QStringLiteral("x")},
                                                                                {QStringLiteral("color"), QStringLiteral("red")}}}}},
                   1920, 1080, 125, &error);
        REQUIRE(!error.isEmpty());
    }

    SECTION("title.create makes a bin clip; title.xml reads it back")
    {
        RcFixture f;
        RemoteControl r;
        auto created =
            rc(r, QStringLiteral("title.create"),
               {{QStringLiteral("name"), QStringLiteral("Intro")},
                {QStringLiteral("duration"), 50},
                {QStringLiteral("spec"), QJsonObject{{QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                                                      {QStringLiteral("text"), QStringLiteral("Hi")}}}}}}});
        REQUIRE(created.value(QStringLiteral("ok")).toBool());
        const QString binId = created.value(QStringLiteral("result")).toObject().value(QStringLiteral("binId")).toString();
        auto clip = pCore->projectItemModel()->getClipByBinID(binId);
        for (int i = 0; i < 500 && !clip->statusReady(); ++i) {
            waitMs(20);
        }
        REQUIRE(clip->statusReady());
        const QString xml = rc(r, QStringLiteral("title.xml"), {{QStringLiteral("binId"), binId}})
                                .value(QStringLiteral("result"))
                                .toObject()
                                .value(QStringLiteral("xml"))
                                .toString();
        REQUIRE(xml.contains(QStringLiteral(">Hi<")));
        REQUIRE(errCode(rc(r, QStringLiteral("title.create"),
                           {{QStringLiteral("name"), QStringLiteral("x")}, {QStringLiteral("duration"), 50}, {QStringLiteral("spec"), QJsonObject()}})) ==
                QStringLiteral("bad_params"));
        REQUIRE(errCode(rc(r, QStringLiteral("title.xml"), {{QStringLiteral("binId"), f.binId}})) == QStringLiteral("bad_params")); // not a title
    }
}

TEST_CASE("RC subtitles", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    auto ok = [](const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); };
    auto subs = [&]() { return rc(r, QStringLiteral("subtitle.list")).value(QStringLiteral("result")).toArray(); };

    auto added = rc(r, QStringLiteral("subtitle.add"),
                    {{QStringLiteral("start"), 25}, {QStringLiteral("end"), 75}, {QStringLiteral("text"), QStringLiteral("héllo ✓")}});
    REQUIRE(ok(added));
    const int sid = added.value(QStringLiteral("result")).toObject().value(QStringLiteral("id")).toInt();
    REQUIRE(subs().size() == 1);
    const QJsonObject s0 = subs().at(0).toObject();
    REQUIRE(s0.value(QStringLiteral("text")).toString() == QStringLiteral("héllo ✓"));
    REQUIRE(s0.value(QStringLiteral("start")).toInt() == 25);
    REQUIRE(s0.value(QStringLiteral("end")).toInt() == 75);
    REQUIRE(s0.value(QStringLiteral("id")).toInt() == sid);

    REQUIRE(ok(
        rc(r, QStringLiteral("subtitle.edit"), {{QStringLiteral("id"), sid}, {QStringLiteral("text"), QStringLiteral("bye")}, {QStringLiteral("end"), 100}})));
    REQUIRE(subs().at(0).toObject().value(QStringLiteral("text")).toString() == QStringLiteral("bye"));
    REQUIRE(subs().at(0).toObject().value(QStringLiteral("end")).toInt() == 100);
    f.undoStack->undo(); // one step for the whole edit
    REQUIRE(subs().at(0).toObject().value(QStringLiteral("text")).toString() == QStringLiteral("héllo ✓"));
    REQUIRE(subs().at(0).toObject().value(QStringLiteral("end")).toInt() == 75);

    QTemporaryDir dir;
    const QString out = dir.filePath(QStringLiteral("subs.srt"));
    REQUIRE(ok(rc(r, QStringLiteral("subtitle.export"), {{QStringLiteral("path"), out}})));
    REQUIRE(QFileInfo(out).size() > 0);

    REQUIRE(errCode(rc(r, QStringLiteral("subtitle.import"), {{QStringLiteral("path"), QStringLiteral("/nonexistent.srt")}})) == QStringLiteral("not_found"));
    REQUIRE(!rc(r, QStringLiteral("subtitle.styles")).value(QStringLiteral("result")).toArray().isEmpty());

    REQUIRE(ok(rc(r, QStringLiteral("subtitle.delete"), {{QStringLiteral("id"), sid}})));
    REQUIRE(subs().isEmpty());
    REQUIRE(errCode(rc(r, QStringLiteral("subtitle.delete"), {{QStringLiteral("id"), sid}})) == QStringLiteral("not_found"));
    REQUIRE(errCode(rc(r, QStringLiteral("subtitle.add"),
                       {{QStringLiteral("start"), 50}, {QStringLiteral("end"), 10}, {QStringLiteral("text"), QStringLiteral("x")}})) ==
            QStringLiteral("bad_params"));
}

TEST_CASE("RC mixer and frame grab", "[RemoteControl]")
{
    RcFixture f;
    RemoteControl r;
    auto ok = [](const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); };
    int atid;
    REQUIRE(f.timeline->requestTrackInsertion(-1, atid, QString(), true));
    auto mixerOf = [&](int tid) {
        for (const auto &m : rc(r, QStringLiteral("mixer.get")).value(QStringLiteral("result")).toArray()) {
            if (m.toObject().value(QStringLiteral("trackId")).toInt() == tid) return m.toObject();
        }
        return QJsonObject();
    };

    REQUIRE(!mixerOf(atid).isEmpty());
    REQUIRE(!mixerOf(-1).isEmpty());   // master
    REQUIRE(mixerOf(f.tid).isEmpty()); // video tracks have no mixer strip
    REQUIRE(ok(rc(r, QStringLiteral("mixer.set"), {{QStringLiteral("trackId"), atid}, {QStringLiteral("volumeDb"), -6}, {QStringLiteral("pan"), 0.25}})));
    REQUIRE(mixerOf(atid).value(QStringLiteral("volumeDb")).toDouble() == -6.);
    REQUIRE(mixerOf(atid).value(QStringLiteral("pan")).toDouble() == 0.25);
    REQUIRE(ok(rc(r, QStringLiteral("mixer.set"), {{QStringLiteral("trackId"), -1}, {QStringLiteral("volumeDb"), -3}})));
    REQUIRE(mixerOf(-1).value(QStringLiteral("volumeDb")).toDouble() == -3.);
    REQUIRE(errCode(rc(r, QStringLiteral("mixer.set"), {{QStringLiteral("trackId"), atid}, {QStringLiteral("pan"), 1.5}})) == QStringLiteral("bad_params"));
    REQUIRE(errCode(rc(r, QStringLiteral("mixer.set"), {{QStringLiteral("trackId"), f.tid}, {QStringLiteral("volumeDb"), 0}})) == QStringLiteral("bad_params"));
    REQUIRE(errCode(rc(r, QStringLiteral("mixer.solo"), {{QStringLiteral("trackId"), atid}})) == QStringLiteral("no_gui"));
    REQUIRE(errCode(rc(r, QStringLiteral("monitor.grab"))) == QStringLiteral("no_gui"));
}
