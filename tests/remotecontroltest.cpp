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
        waitMs(150);
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
