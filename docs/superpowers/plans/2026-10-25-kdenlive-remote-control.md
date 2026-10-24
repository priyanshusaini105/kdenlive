# Kdenlive Remote Control + MCP Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an AI drive a running (forked) Kdenlive live, with full editing control, through a D-Bus JSON dispatcher and a Python MCP server.

**Architecture:** A `RemoteControl` QObject inside Kdenlive (`src/src/remotecontrol/`) is registered at `/RemoteControl` on the session bus. It exposes `call(command, paramsJson) → json` and `listCommands() → json`, and dispatches to per-namespace command handlers that call Kdenlive's model-level APIs on the GUI thread. A Python MCP server (`mcp/`) discovers the instance, converts timecodes to frames, and exposes curated tools plus a generic passthrough.

**Tech Stack:** C++17/Qt6/KF6 (Kdenlive master), Catch (bundled `tests/catch.hpp`), Python 3.14 with `uv`, `mcp` (official SDK, FastMCP), `dbus-fast`, `pytest`.

**Spec:** `docs/superpowers/specs/2026-10-25-kdenlive-remote-control-design.md`. Read its "Research notes" for the entry point of every command below.

## Global Constraints

- Repo is `/home/work/personal/kdenlive/src`, branch `remote-control`. Commit after each task. Never push.
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build dir is `/home/work/personal/kdenlive/build`, a **Release** build with Ninja, always `-j4`. `/home` has ~13 GB free, so don't build all tests: build only the `remotecontroltest` and `kdenlive` targets.
- Install prefix is `~/.local/opt/kdenlive-rc`. Never touch `/usr` or the stock 26.08.1 install, its `~/.config/kdenliverc`, or `~/.local/share/kdenlive`.
- Upstream files may change **only** in: `src/src/CMakeLists.txt` (one `add_subdirectory`), `src/src/core.cpp` (registration), `src/src/timeline2/model/timelinemodel.hpp` (one `friend class RemoteControl;`), `src/tests/CMakeLists.txt` (one source line). Everything else goes in `src/src/remotecontrol/`, `mcp/`, or `dev/`.
- **No modal dialogs**:
  - Never call a slot that `exec()`s, shows `KMessageBox`, or opens a file dialog. The spec lists the ones to avoid (`beginAddTrack`, `deleteMultipleTracks`, `changeItemSpeed`, `TimelineController::addMarker/editMarker`, `importSubtitle`, `exportSubtitle`, `saveFile` without a URL, `closeCurrentDocument(true)`, `Bin::slotDeleteClip`, `slotExtractCurrentFrame`, `RenderWidget::slotPrepareExport`).
  - If a dialog cannot be avoided, refuse with an error code instead.
- **No exceptions** (`-fno-exceptions`): validate every id with `isClip/isTrack/isComposition/isItem` (and the equivalents for bin, markers and subtitles) before any getter. Out-of-range values give `bad_params`.
- D-Bus: object `/RemoteControl`, interface `org.kde.kdenlive.RemoteControl`, methods `call(ss) → s` and `listCommands() → s`. The bus name is `org.kde.kdenlive-<pid>`.
- Reply envelope: `{"ok":true,"result":<any>}` or `{"ok":false,"error":{"code":"<code>","message":"<text>"}}`.
- Error codes, and only these: `bad_json`, `unknown_command`, `bad_params`, `not_found`, `no_timeline`, `no_gui`, `failed`, `document_modified`, `group_open`, `no_group`.
- Units: frames everywhere on D-Bus. Effect and keyframe positions are item-local (relative to the clip's in point).
- Undo: every mutating command is undoable where Kdenlive supports it. `listCommands` marks `"undoable": false` for mixer volume/pan, `project.setProfile`, and `subtitle.import`.
- Fork stays GPL with headers matching neighbouring files (`SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`). `mcp/` gets its own `LICENSE` (MIT).

## Review Focus

1. **Stale ids.** The user deletes a clip in the GUI after the AI read the timeline, then the AI edits that id. Expect `not_found`, not a crash. The test lives in Task 1 (`clip.cut` on a deleted id) and is repeated per namespace.
2. **Locked tracks.** Kdenlive refuses edits on a locked track. Expect `ok:false` with `failed` and a message naming the lock, never `ok:true`. The test lives in Task 9.
3. **User edits while an AI group is open.** The idle auto-close must fire, so the user's next GUI edit is its own undo step. The test lives in Task 2 (short timeout).
4. **Stock Kdenlive running next to the fork.** Discovery must skip instances without `/RemoteControl`, and pick by `KDENLIVE_PID` when there are several. The test lives in Task 7 (`pick_instance`).
5. **Non-integer fps (29.97 / 23.976).** Timecode `HH:MM:SS:FF` uses the nominal rate; seconds forms use the real rate. The test lives in Task 7 (timecode tests).

---

## Shared C++ interfaces (defined in Task 1, used by every command task)

`src/src/remotecontrol/rccommand.h`:

```cpp
struct RcReply { QJsonValue result; QString code; QString message; bool ok() const { return code.isEmpty(); } };
RcReply rcOk(const QJsonValue &v = QJsonValue());
RcReply rcFail(const QString &code, const QString &message);

struct RcParam { QString name; QString type; bool required; QString description; }; // type: int|double|string|bool|object|array
struct RcCommand {
    QString name;            // "clip.cut"
    QString description;     // one line
    QList<RcParam> params;
    bool mutating;           // true → counts toward undo grouping / idle timer
    bool undoable;           // false only for the spec's exceptions
    bool needsGui;           // true → returns no_gui when pCore->window() is null
    std::function<RcReply(const QJsonObject &params)> run;
};
using RcRegistry = QMap<QString, RcCommand>;

// helpers (rchelpers.h): all return std::nullopt and fill `err` on failure
std::shared_ptr<TimelineItemModel> rcTimeline(RcReply &err);                 // no_timeline
std::optional<int>    rcInt(const QJsonObject &p, const char *key, RcReply &err, std::optional<int> def = {});
std::optional<double> rcDouble(const QJsonObject &p, const char *key, RcReply &err, std::optional<double> def = {});
std::optional<QString> rcString(const QJsonObject &p, const char *key, RcReply &err, std::optional<QString> def = {});
std::optional<int>    rcClip(const std::shared_ptr<TimelineItemModel> &tl, const QJsonObject &p, RcReply &err, const char *key = "clipId");  // not_found
std::optional<int>    rcTrack(const std::shared_ptr<TimelineItemModel> &tl, const QJsonObject &p, RcReply &err, const char *key = "trackId");
std::shared_ptr<EffectStackModel> rcEffectStack(const QJsonObject &target, RcReply &err); // target {type: clip|track|master|bin, id}
```

Each namespace file exposes `void registerXxxCommands(RcRegistry &r);`, and `RemoteControl`'s constructor calls them all. `RemoteControl` has public `QString call(const QString &command, const QString &paramsJson)`, `QString listCommands()`, and `void setIdleTimeoutMs(int ms)` (default 10000, for tests). Tests construct `RemoteControl` directly with no bus.

Test harness pattern for `tests/remotecontroltest.cpp` (copy from `tests/effectstest.cpp:15-35`):
- Build a `KdenliveDoc`, `testSetDocument`, `updateTimeline`, `testSetActiveTimeline`.
- Use `KdenliveTests::createProducer(...)` for bin clips.
- Parse replies with a local helper `QJsonObject rc(RemoteControl &r, const char *cmd, const QJsonObject &p)`.

---

## Phase 1: Thin slice, end to end

### Task 1: Dispatcher, registration, `timeline.get`, `project.info`, `clip.cut`

**Files:**
- Create: `src/src/remotecontrol/CMakeLists.txt` (appends sources to `kdenlive_SRCS` with `PARENT_SCOPE`, the same pattern as `src/src/render/CMakeLists.txt`)
- Create: `src/src/remotecontrol/remotecontrol.{h,cpp}`, `rccommand.{h,cpp}`, `rchelpers.{h,cpp}`, `commands_timeline.cpp`
- Modify: `src/src/CMakeLists.txt` (add `add_subdirectory(remotecontrol)` after `add_subdirectory(render)`)
- Modify: `src/src/timeline2/model/timelinemodel.hpp:110` (add `friend class RemoteControl;` next to `KdenliveTests`)
- Modify: `src/src/core.cpp` (after `m_guiConstructed = true;`, inside `#ifndef NODBUS`: `m_remoteControl = new RemoteControl(this); QDBusConnection::sessionBus().registerObject(QStringLiteral("/RemoteControl"), m_remoteControl, QDBusConnection::ExportScriptableSlots);`). `call`/`listCommands` are `Q_SCRIPTABLE` public slots. Use a static or local pointer so `core.h` stays untouched.
- Modify: `src/tests/CMakeLists.txt` (add `remotecontroltest.cpp` to `KdenliveTest_SOURCES`)
- Test: `src/tests/remotecontroltest.cpp`

**Interfaces:**
- Produces: everything in "Shared C++ interfaces".
- Produces these commands:
  - `project.info {} → {fps: double, fpsNum: int, fpsDen: int, width, height, profile: string, duration: int}`
  - `timeline.get {} → {fps, duration, playhead: int|null, tracks:[{id, name, audio: bool, locked: bool, hidden: bool, muted: bool, position: int}], clips:[{id, trackId, binId, name, position, in, out, duration, speed}], compositions:[{id, trackId, aTrack, assetId, position, duration}], selection:[int]}`.
    Tracks are ordered top to bottom (video first, then audio, following `getTracksIds`); clips are sorted by `(trackId, position)`. `playhead` is null when there's no GUI.
  - `clip.cut {clipId, position} → {leftId, rightId}`. `position` is absolute in timeline frames and must be strictly inside the clip, otherwise `bad_params`. Implemented with `TimelineFunctions::requestClipCut`. `rightId` is the clip that now starts at `position` on the same track.

- [ ] **Step 1: Write failing tests** in `remotecontroltest.cpp`:
  - `TEST_CASE("RC envelope")`:
    - `call("nope","{}")` → `ok==false`, `code=="unknown_command"`.
    - `call("timeline.get","{bad")` → `bad_json`.
    - `listCommands()` parses as a JSON array that contains an object with `name=="clip.cut"`, `mutating==true`, `undoable==true`.
  - `TEST_CASE("RC timeline.get + clip.cut")`:
    - Insert a 20-frame red clip at 100 on a new track; `timeline.get` has a clip with `position==100`, `duration==20`.
    - `clip.cut {clipId, position:110}` → ok; the timeline now has 2 clips at 100 and 110.
    - `undoStack->undo()` → 1 clip.
    - `clip.cut` at `position:100` (an edge) → `bad_params`.
    - Delete the clip with `timeline->requestItemDeletion(cid)`, then `clip.cut` on that id → `not_found`, and the process is still alive.
    - `clip.cut {}` → `bad_params`.
- [ ] **Step 2: Configure and build the test; confirm it fails**
  - Run: `cmake -S /home/work/personal/kdenlive/src -B /home/work/personal/kdenlive/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local/opt/kdenlive-rc -DBUILD_TESTING=ON && ninja -C /home/work/personal/kdenlive/build -j4 remotecontroltest`
  - Expected: a compile error (`remotecontrol/remotecontrol.h` not found). The first build of `kdenliveLib` takes a long time; run it in the background.
- [ ] **Step 3: Implement** the dispatcher, helpers, registration and the three commands.
  - Dispatcher order: parse JSON (`bad_json` unless it's an object) → look up the command (`unknown_command`) → if `needsGui` and `!pCore->window()` → `no_gui` → `run`.
  - `listCommands` serialises the registry: `[{name, description, params:[{name,type,required,description}], mutating, undoable, needsGui}]`.
- [ ] **Step 4: Run the tests**
  - Run: `ninja -C /home/work/personal/kdenlive/build -j4 remotecontroltest && cd /home/work/personal/kdenlive/build && ctest -R remotecontroltest --output-on-failure`
  - Expected: PASS.
- [ ] **Step 5: Commit** with the message `remotecontrol: add D-Bus JSON dispatcher with timeline.get and clip.cut`.

### Task 2: Undo grouping (`undo.begin/end`, `undo`, `redo`)

**Files:**
- Create: `src/src/remotecontrol/commands_undo.cpp`
- Modify: `remotecontrol.{h,cpp}` (group state and idle `QTimer`)
- Test: `src/tests/remotecontroltest.cpp`

**Interfaces:**
- Produces these commands:
  - `undo.begin {label: string} → {}`: `pCore->undoStack()->beginMacro("AI: " + label)`. Returns `group_open` if a group is already open.
  - `undo.end {} → {}`: `endMacro`. Returns `no_group` if none is open.
  - `undo {} → {index}` and `redo {} → {index}`: close any open group first, then `undoStack()->undo()`/`redo()`.
- Idle rule: while a group is open, every `call` restarts a single-shot timer of `idleTimeoutMs`. When it fires, it calls `endMacro`.

- [ ] **Step 1: Write failing tests** in `TEST_CASE("RC undo groups")`:
  - `begin{label:"x"}`, two `clip.cut`s, `end` → the clip count goes from 1 to 3. One `undo` gives 1 clip. `undoStack->text(index-1)` after `redo` equals `"AI: x"`.
  - A second `begin` while one is open → `group_open`. `end` with none open → `no_group`.
  - Idle close: `setIdleTimeoutMs(50)`, `begin`, one `clip.cut`, then `QTest::qWait(150)`. The next `timeline->requestClipMove(...)` (simulating a user edit) is a separate undo step: two `undoStack->undo()` calls are needed to get back to 1 clip at its original position.
- [ ] **Step 2: Build and run; confirm they fail.**
  - Run the Task 1 Step 4 command.
  - Expected: FAIL, `unknown_command`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the Task 1 Step 4 command.** Expected: PASS.
- [ ] **Step 5: Commit** with the message `remotecontrol: add undo grouping with idle auto-close`.

### Task 3: `effect.add`, `effect.get`, `effect.set`

**Files:**
- Create: `src/src/remotecontrol/commands_effect.cpp`
- Test: `src/tests/remotecontroltest.cpp`

**Interfaces:**
- Consumes: `rcEffectStack` (Task 1).
- `target` is `{type:"clip"|"track"|"master"|"bin", id}`. `id` is a clip id, track id, or bin id string, and is omitted for master.
- Produces these commands:
  - `effect.add {target, effectId, params?: object} → {index}`:
    - Uses `appendEffect(effectId, false, params)`; `index` counts **visible** rows only (skipping `hideFromStack()`).
    - An unknown `effectId` (`!EffectsRepository::get()->exists`) → `not_found`.
  - `effect.get {target} → [{index, effectId, name, enabled, params:[{name, type, value, default, min, max, values?}]}]`, from `AssetParameterModel` data roles.
  - `effect.set {target, index, params: {name: value-string}} → {}`:
    - One parameter: `pCore->pushUndo(new AssetCommand(m, idx, value))`.
    - Several parameters: one `AssetMultiCommand`.
    - An unknown parameter name → `bad_params`.
    - The value string can be an MLT animation string (`"0=0;50~=1"`), which covers keyframes in bulk.

- [ ] **Step 1: Write failing tests** in `TEST_CASE("RC effects")`, using the `"sepia"` effect like `effectstest.cpp`:
  - `add` → `index==0`.
  - `get` → one effect with `effectId=="sepia"`.
  - `set {index:0, params:{u:"100"}}` → `get` shows `"100"`. `undoStack->undo()` restores the previous value.
  - `add` with an unknown id → `not_found`.
  - `set` with `index:5` → `bad_params`.
  - `target {type:"clip", id:<deleted>}` → `not_found`.
- [ ] **Step 2: Build and run; confirm they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** with the message `remotecontrol: add effect.add/get/set`.

### Task 4: `action.list`, `action.trigger`

**Files:**
- Create: `src/src/remotecontrol/commands_action.cpp`
- Test: `src/tests/remotecontroltest.cpp`

**Interfaces:**
- Produces these commands (both `needsGui:true`):
  - `action.list {filter?: string} → [{name, text, shortcut, enabled, checkable, checked}]`:
    - Covers every action in `pCore->window()->actionCollection()->actions()`.
    - `filter` is a case-insensitive substring match against `name` and `text`.
    - `text` keeps its "…", so the AI can tell when an action opens a dialog.
  - `action.trigger {name} → {}`:
    - An unknown name → `not_found`; a disabled action → `failed`.
    - Otherwise `QTimer::singleShot(0, action, &QAction::trigger)`, and the reply returns immediately.
    - `mutating:true` (it may push undo).

- [ ] **Step 1: Write the failing test.** In test mode with no window, both commands return `no_gui`.
- [ ] **Step 2: Build and run; confirm it fails** (`unknown_command`).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS. Real behaviour is checked live in Task 5.
- [ ] **Step 5: Commit** with the message `remotecontrol: add action.list/trigger`.

### Task 5: Install the fork and the isolated launcher

**Files:**
- Create: `dev/kdenlive-rc.sh` (executable)

**Interfaces:**
- Produces: `dev/kdenlive-rc.sh [args…]`, which launches the fork with isolated state.
  - `P=$HOME/.local/opt/kdenlive-rc`
  - `XDG_DATA_DIRS=$P/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}`
  - `XDG_CONFIG_HOME=$P/home/config`, `XDG_DATA_HOME=$P/home/data`, `XDG_CACHE_HOME=$P/home/cache` (create them with `mkdir -p`)
  - Prefix `QML_IMPORT_PATH` / `QT_PLUGIN_PATH` with `$P/lib/qt6/qml` and `$P/lib/qt6/plugins` if they exist after install.
  - `exec $P/bin/kdenlive "$@"`

- [ ] **Step 1: Build and install**
  - Run: `ninja -C /home/work/personal/kdenlive/build -j4 kdenlive && cmake --install /home/work/personal/kdenlive/build`
  - Expected: `~/.local/opt/kdenlive-rc/bin/kdenlive` exists. `~/.local/opt/kdenlive-rc/bin/kdenlive --version` prints `26.11.70`.
- [ ] **Step 2: Write `dev/kdenlive-rc.sh`, launch it in the background, and verify**
  - Run: `busctl --user list | grep kdenlive` → note the pid of the new instance.
  - Run: `busctl --user call org.kde.kdenlive-<pid> /RemoteControl org.kde.kdenlive.RemoteControl call ss timeline.get '{}'` → `s "{\"ok\":true,...}"`.
  - Check `~/.config/kdenliverc` is unchanged: compare its mtime before and after launch.
- [ ] **Step 3: Live-check the GUI-only parts**
  - `action.list {filter:"razor"}` returns an entry.
  - `action.trigger {name:"edit_undo"}` returns ok immediately.
  - `timeline.get.playhead` is an int.
- [ ] **Step 4: Commit** with the message `dev: add isolated launcher for the remote-control build`.

### Task 6: MCP server, core modules (discovery, bus, timecode)

**Files:**
- Create: `mcp/pyproject.toml` (name `kdenlive-mcp`, `requires-python >=3.12`, deps `mcp`, `dbus-fast`, dev dep `pytest`, script `kdenlive-mcp = "kdenlive_mcp.server:main"`)
- Create: `mcp/LICENSE` (MIT), `mcp/kdenlive_mcp/__init__.py`, `mcp/kdenlive_mcp/timecode.py`, `mcp/kdenlive_mcp/bus.py`
- Test: `mcp/tests/test_timecode.py`, `mcp/tests/test_bus.py`

**Interfaces:**
- `timecode.parse(value: str | int, fps: float) -> int` (frames). Accepts:
  - an int or `"123"` or `"123f"` → frames
  - `"HH:MM:SS:FF"` → `((h*3600+m*60+s) * round(fps)) + ff`, where `ff >= round(fps)` raises `ValueError`
  - `"HH:MM:SS.mmm"`, `"MM:SS"`, `"MM:SS.sss"`, `"12.5s"` → `int(seconds*fps + 0.5)`
  - Negative or unparseable input raises `ValueError`.
- `timecode.format(frames: int, fps: float) -> str` → `"HH:MM:SS:FF"` (nominal rate).
- `bus.pick_instance(candidates: list[tuple[str, int]], env_pid: str | None) -> str`:
  - `candidates` are (bus name, pid) pairs **already filtered** to those exposing `/RemoteControl`.
  - Returns the bus name. With 0 candidates it raises `KdenliveNotRunning("start the forked build: dev/kdenlive-rc.sh")`. With more than 1 and no `env_pid` it raises `AmbiguousInstance` listing the pids. A non-matching `env_pid` raises `KdenliveNotRunning`.
- `class Kdenlive` (`bus.py`) with `async def call(command: str, params: dict | None = None) -> Any`:
  - Returns `result`, or raises `KdenliveError(code, message)`.
  - It connects lazily, discovers names `org.kde.kdenlive-*`, keeps those whose introspection at `/RemoteControl` has interface `org.kde.kdenlive.RemoteControl`, then calls `pick_instance(..., os.environ.get("KDENLIVE_PID"))`.
  - It re-discovers once on a D-Bus error (the app was restarted).
  - Also `async def commands() -> list[dict]` and `async def fps() -> float`; fps is cached per `call` sequence and refreshed when the timeline is re-read.

- [ ] **Step 1: Write failing tests**
  - Timecode at 25 fps:
    - `parse("00:00:01:05",25)==30`
    - `parse("1:10",25)==1750`
    - `parse("2.5s",25)==63`
    - `parse(40,25)==40`
    - `parse("40f",25)==40`
    - `parse("00:00:00:25",25)` raises
    - `parse("-1",25)` raises
  - Timecode at 29.97 (`30000/1001`):
    - `parse("00:01:00:00",30000/1001)==1800`
    - `parse("60s",30000/1001)==1798`
  - Timecode at 23.976 (`24000/1001`): `parse("00:00:10:00",24000/1001)==240`.
  - Round trip: `format(1750,25)=="00:01:10:00"`.
  - `pick_instance`:
    - `[] ` → `KdenliveNotRunning`
    - `[("org.kde.kdenlive-5",5)]` → that name
    - two candidates with `None` → `AmbiguousInstance`
    - two candidates with `"7"` → the pid-7 name
- [ ] **Step 2: Run and confirm they fail**
  - Run: `cd /home/work/personal/kdenlive/src/mcp && uv run pytest -q`
  - Expected: FAIL (ImportError).
- [ ] **Step 3: Implement** `timecode.py` and `bus.py`.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** with the message `mcp: add timecode parsing and instance discovery`.

### Task 7: MCP server, phase-1 tools, and registration in Claude Code

**Files:**
- Create: `mcp/kdenlive_mcp/server.py`, `mcp/README.md` (install and run steps only)

**Interfaces:**
- Consumes: `Kdenlive`, `timecode` (Task 6).
- Server instructions text (FastMCP `instructions`), verbatim:
  > Wrap each user request in begin_request/end_request so it is one Ctrl+Z. Read get_timeline before editing; ids can go stale if the user edits. Times accept HH:MM:SS:FF, MM:SS, 2.5s or frames.
- Phase-1 tools:
  - `get_timeline()`: `timeline.get`, adding a `timecode` field next to every position and duration.
  - `cut_at(time: str|int|None=None, track_id: int|None=None)`:
    - `time` defaults to the playhead.
    - Targets: clips spanning the time on `track_id`; else selected clips spanning it; else all clips spanning it on unlocked tracks.
    - Each cut is one `clip.cut`, inside its own `undo.begin("cut")/end` unless a request group is open.
    - Returns the new ids.
  - `add_effect(effect_id: str, clip_id: int|None=None, track_id: int|None=None, master: bool=False, params: dict|None=None)`: the target defaults to the selected clips (one `effect.add` each).
  - `set_effect_params(index: int, params: dict, clip_id|track_id|master as above)`
  - `list_actions(filter: str|None=None)`, `trigger_action(name: str)`
  - `undo()`, `redo()`, `begin_request(label: str)`, `end_request()`
  - `kdenlive_commands()`: `listCommands`. `kdenlive_call(command: str, params: dict|None=None)`: raw passthrough.
- `KdenliveError` is raised as an MCP tool error with the text `"<code>: <message>"`.
- `main()` runs stdio.

- [ ] **Step 1: Implement `server.py`.**
- [ ] **Step 2: Smoke-test against the live fork from Task 5**
  - Run: `cd /home/work/personal/kdenlive/src/mcp && uv run python -c "import asyncio; from kdenlive_mcp.bus import Kdenlive; print(asyncio.run(Kdenlive().call('project.info')))"`
  - Expected: a dict with `fps`.
- [ ] **Step 3: Register and verify**
  - Run: `claude mcp add kdenlive -- uv --directory /home/work/personal/kdenlive/src/mcp run kdenlive-mcp`, then `claude mcp list`.
  - Expected: `kdenlive … ✓ Connected`.
- [ ] **Step 4: Commit** with the message `mcp: add phase-1 tools and server`.

### Task 8: Thin-slice end-to-end scenario

**Files:**
- Create: `mcp/tests/e2e_live.py` (not collected by pytest by default: file name doesn't start with `test_`, run explicitly)

**Interfaces:**
- Consumes: `Kdenlive` (Task 6).
- Script:
  1. Generate `/tmp/claude-1002/kdenlive-e2e/bars.mp4` (10 s of smptebars + tone) with `ffmpeg -f lavfi -i smptebars=s=1280x720:r=25:d=10 -f lavfi -i sine=d=10 -shortest`.
  2. Assert the timeline has ≥1 clip. The user loads the media by hand in this phase; bin import arrives in Task 18.
  3. Cut the first clip at its position + 125 inside an `undo.begin("e2e")` group.
  4. Add `sepia` to the right half.
  5. `undo.end`, `undo`. Assert the clip count and effect are back to the start state.
  6. `redo`. Assert they are applied again.
  7. `action.list{filter:"razor"}` is non-empty.

- [ ] **Step 1: Write and run** `cd mcp && uv run python tests/e2e_live.py` against the fork with the clip loaded by hand. Expected: `E2E OK`.
- [ ] **Step 2: Commit** with the message `mcp: add live thin-slice e2e`.

---

## Phases 2–5: Full control

Every command task below follows the Task 3 cycle:
1. Write failing Catch tests in `remotecontroltest.cpp`.
2. Run `ninja … remotecontroltest && ctest -R remotecontroltest`.
3. Implement in the named `commands_*.cpp`.
4. Run to PASS.
5. Commit with the message `remotecontrol: add <namespace> commands`.

Each command task also has two **mandatory tests**:
- (a) one representative edit, then `undoStack->undo()`, restores the state (skip for `undoable:false`);
- (b) a deleted or unknown id → `not_found`.

The tests listed per task are additional.

### Phase 2: Timeline editing

#### Task 9: clip, edit and group commands
**Files:** `commands_clip.cpp`

| Command | Params → result | Implementation |
|---|---|---|
| `clip.insert` | `{binId, trackId, position} → {clipId}` | `requestClipInsertion`. A bin clip that is not `statusReady()` → `failed` "clip still loading" |
| `clip.move` | `{clipId, trackId, position} → {}` | `requestClipMove(..., logUndo=true)` |
| `clip.resize` | `{clipId, duration, fromEnd: bool=true} → {duration}` | `requestItemResize(id, duration, fromEnd, true)`; `-1` → `failed` |
| `clip.slip` | `{clipId, offset} → {}` | `requestClipSlip` |
| `clip.rippleResize` | `{clipId, duration, fromEnd} → {}` | `requestItemRippleResize` |
| `clip.delete` | `{clipId} → {}` | `requestItemDeletion` |
| `clip.speed` | `{clipId, speed: double, pitchCompensate=false, changeDuration=true} → {}` | `requestClipTimeWarp` (ratio). `speed==0` → `bad_params` |
| `clip.enable` | `{clipIds:[int], enabled: bool} → {}` | `TimelineFunctions::setClipsEnabled` |
| `clip.splitAudio` / `clip.splitVideo` | `{clipId} → {}` | `requestSplitAudio/Video` |
| `clip.copy` | `{clipIds} → {xml}`; `clip.paste {xml, trackId, position} → {}` | `copyClips/pasteClips` |
| `edit.extractZone` | `{in, out, trackIds?:[int], lift: bool} → {}` | `extractZoneWithUndo`. Default: active tracks |
| `edit.insertSpace` / `edit.removeSpace` | `{trackId, position, duration} → {}` | `requestInsertSpace/removeSpace` |
| `edit.deleteBlank` | `{trackId, position} → {}` | `requestDeleteBlankAt` |
| `group.create` | `{ids:[int]} → {groupId}`; `group.ungroup {id} → {}` | `requestClipsGroup/requestClipUngroup` |
| `timeline.select` | `{ids:[int]} → {}` | `requestSetSelection` |

Extra tests:
- Lock the track with `setTrackLockedState(tid,true)`; `clip.move` → `ok:false, failed`, message contains "locked" (Review Focus 2).
- `clip.move` to an occupied spot → `failed`.
- `clip.speed {speed:0}` → `bad_params`.

#### Task 10: track commands
**Files:** `commands_track.cpp`

| Command | Params → result | Implementation |
|---|---|---|
| `track.add` | `{audio: bool, position: int=-1, name?} → {trackId}` | `requestTrackInsertion` |
| `track.delete` | `{trackId} → {}` | `requestTrackDeletion` |
| `track.rename` | `{trackId, name} → {}` | `setTrackName` |
| `track.lock` | `{trackId, locked} → {}` | `setTrackLockedState` |
| `track.hide` | `{trackId, hidden} → {}` | Video: `hide` "3"/"2", via `setTrackProperty` plus a pushed undo `Fun` pair restoring the old value. Audio: `bad_params` (use `track.mute`) |
| `track.mute` | `{trackId, muted} → {}` | Audio: "3"/"1", same undo pattern |
| `track.move` | `{trackId, up: bool} → {}` | `requestTrackMove` |

Extra test: `track.hide` on an audio track → `bad_params`.

#### Task 11: markers, guides and zone
**Files:** `commands_marker.cpp`

| Command | Params → result |
|---|---|
| `marker.list` | `{scope:"guides"|"clip", binId?} → [{position, comment, category, duration}]` |
| `marker.add` | `{scope, binId?, position, comment="", category=0, duration=0} → {}` (`addRangeMarker` when `duration>0`) |
| `marker.edit` | `{scope, binId?, position, newPosition?, comment?, category?} → {}` |
| `marker.remove` | `{scope, binId?, position} → {}` |
| `marker.categories` | `{} → [{index, name, color}]` |
| `zone.get` | `{} → {in, out}` (out exclusive). `zone.set {in, out} → {}` uses `TimelineController::setZone(QPoint, true)` (`needsGui`) |

- Guides use the active timeline's `getGuideModel()`. Clip markers use `getBinClip(binId)->getMarkerModel()`.
- Positions are `GenTime(frames, pCore->getCurrentFps())`.
- Extra test: `marker.remove` at a position without a marker → `not_found`.

#### Task 12: monitor transport
**Files:** `commands_monitor.cpp`

| Command | Params → result | Notes |
|---|---|---|
| `monitor.seek` | `{position, monitor:"project"|"clip"="project"} → {}` | `pCore->seekMonitor` |
| `monitor.play` | `{play: bool} → {}` | `switchPlay` |
| `monitor.position` | `{} → {position, playing}` | |

All of these are `needsGui`. The test asserts `no_gui`; the live check is in Task 13.

#### Task 13: MCP phase-2 tools and live check
**Files:** `mcp/kdenlive_mcp/server.py`

- Add these tools:
  - `insert_clip(bin_id, track_id, time)`, `move_clip(clip_id, time, track_id=None)`, `trim_clip(clip_id, duration, from_end=True)`
  - `delete_clips(clip_ids=None)` (defaults to the selection), `set_speed(clip_id, speed)`
  - `add_track(audio, name=None)`, `set_track(track_id, name=None, locked=None, hidden=None, muted=None)`
  - `add_marker(time, comment="", clip_bin_id=None)`, `set_zone(start, end)`
  - `seek(time)`, `play(play=True)`, `select(clip_ids)`
- All times go through `timecode.parse`.
- Live check: extend `e2e_live.py` with move, trim, a marker and a seek. Expected: `E2E OK`.
- Commit with the message `mcp: add phase-2 tools`.

### Phase 3: Effects and assets in depth

#### Task 14: asset catalog
**Files:** `commands_asset.cpp`

| Command | Params → result |
|---|---|
| `asset.list` | `{kind:"effect"|"transition", filter?, audio?: bool} → [{id, name, type, audio, description}]` |
| `asset.schema` | `{kind, id} → {id, name, params:[{name, type, default, min, max, values?, animated: bool}]}`, parsed from `getXml(id)` `<parameter>` elements. `animated` = type in {animated, animatedrect, keyframe, geometry} |

Extra test: `asset.schema {kind:"effect", id:"sepia"}` has a parameter `u`.

#### Task 15: effect stack management and keyframes
**Files:** `commands_effect.cpp`

| Command | Params → result | Implementation |
|---|---|---|
| `effect.remove` | `{target, index} → {}` | `removeEffect` |
| `effect.move` | `{target, from, to} → {}` | `moveEffect`. Convert visible indexes to rows and apply the "insert before" `+1` rule for downward moves |
| `effect.enable` | `{target, index, enabled} → {}` | `markEnabled` with undo/redo, then `pushUndo` |
| `keyframe.list` | `{target, index, param} → [{frame, value, interp}]` | |
| `keyframe.set` | `{target, index, param, frame, value, interp?:"linear"|"discrete"|"smooth"|…} → {}` | `addKeyframe` if absent, then `updateKeyframe` (and `updateKeyframeType` if `interp` is given) |
| `keyframe.remove` | `{target, index, param, frame} → {}` | |
| `keyframe.move` | `{target, index, param, frame, newFrame} → {}` | |

Map `interp` names to `KeyframeType::KeyframeEnum` (`definitions.h:215`). An unknown name → `bad_params`.

Extra tests:
- Keyframe on a non-animated parameter → `bad_params`.
- `effect.move` 0→1 on a 2-effect stack swaps them.
- Positions are item-local: a keyframe at frame 0 on a clip at timeline position 100 is listed as `frame:0`.

#### Task 16: compositions and mixes
**Files:** `commands_composition.cpp`

| Command | Params → result | Implementation |
|---|---|---|
| `composition.add` | `{transitionId, trackId, position, duration, aTrack?} → {compositionId}` | `TimelineModel::requestCompositionInsertion` |
| `composition.delete` | `{compositionId} → {}` | |
| `composition.move` | `{compositionId, trackId, position} → {}` | |
| `composition.resize` | `{compositionId, duration, fromEnd} → {}` | `requestItemResize` |
| `composition.get` / `composition.set` | same shape as `effect.get/set`, on `getCompositionParameterModel(cid)` | |
| `mix.add` | `{clipId, transitionId="luma", duration?} → {}` | Same-track crossfade between `clipId` and the clip ending at its start. The implementer finds the model-level equivalent behind `TimelineController::insertNewMix`, which must not open a dialog. No adjacent clip → `failed` |
| `mix.remove` | `{clipId} → {}` | |
| `mix.get` / `mix.set` | `{clipId}` / `{clipId, params}` | |

Also extend `timeline.get` (Task 1) with `mixes: [{clipId, trackId, assetId, duration}]`, read through `getTrackById_const` (friend) and `getMixInfo`.

Extra tests:
- `mix.add` on a clip with no left neighbour → `failed`.
- After `mix.add`, `timeline.get.mixes` has one entry.

#### Task 17: MCP phase-3 tools
**Files:** `server.py`

- Add these tools:
  - `list_effects(filter=None, audio=None)`, `effect_schema(effect_id)`, `list_transitions(filter=None)`
  - `get_effects(target…)`, `remove_effect`, `move_effect`, `toggle_effect`
  - `set_keyframe(param, time, value, effect_index, target…, interp=None)`: `time` is a **timeline** time, converted to item-local by subtracting the clip's position and adding its in point
  - `add_transition(clip_a, clip_b)`:
    - same track → `mix.add`;
    - different tracks → `composition.add` over their overlap, defaulting to `dissolve` when present.
  - `add_composition(...)`
- Extend `e2e_live.py` with a fade keyframe and a dissolve.
- Commit.

### Phase 4: Bin, titles, subtitles, seeing and mixer

#### Task 18: bin
**Files:** `commands_bin.cpp`

| Command | Params → result | Implementation |
|---|---|---|
| `bin.list` | `{} → [{binId, name, type, folderId, duration, url, ready, hasProxy}]` plus `folders:[{id, name, parentId}]` | |
| `bin.import` | `{paths:[string], folderId="-1"} → {binIds:[string]}` | `createClipFromFile` per path. A missing file → `not_found` naming the path |
| `bin.folder.create` | `{name, parentId="-1"} → {folderId}` | `requestAddFolder` + `pushUndo` |
| `bin.rename` | `{binId, name} → {}` | |
| `bin.delete` | `{binId} → {}` | `requestBinClipDeletion` + `pushUndo` |
| `bin.color` | `{color:"#RRGGBB[AA]", duration, name} → {binId}` | `createColorClip`. Convert to `0xRRGGBBAA` |
| `bin.properties.get` | `{binId, keys?:[string]} → {key:value}` | |
| `bin.properties.set` | `{binId, props:{}} → {}` | `Bin::slotEditClipCommand`, `needsGui` |
| `bin.proxy` | `{binIds, enabled} → {}` | `slotProxyCurrentItem`. Proxies disabled in the project → `failed` |

Extra tests:
- `bin.import` on a nonexistent path → `not_found`.
- `bin.color {color:"#ff0000", duration:50}` → the bin has a color clip of 50 frames.

#### Task 19: titles
**Files:** `commands_title.cpp`, `rctitle.{h,cpp}` (spec → `kdenlivetitle` XML)

- `title.create {name, duration, spec} → {binId}`. `spec` is:

  ```
  {
    background?: "#RRGGBBAA",
    items: [
      { type: "text", text, x, y, font = "Sans", size = 64, weight = 400,
        italic = false, color = "#ffffffff", align = "left|center|right",
        outline = 0, outlineColor = "#000000ff", shadow? },
      { type: "rect", x, y, w, h, fill = "#000000ff", stroke?, strokeWidth = 0 }
    ],
    scroll?: { from: [x, y, w, h], to: [x, y, w, h] }
  }
  ```

  `x`/`y` are in profile pixels. The XML format follows `data/titles/simple.kdenlivetitle` and `TitleDocument::xmlItem`; the canvas size is the profile size.
- `title.update {binId, spec} → {}`: `slotEditClipCommand` with `xmldata` + `force_reload=1`, `needsGui`.
- `title.xml {binId} → {xml}` (read back).

Extra test: `rcTitleXml(spec, 1920, 1080)` has a single `item type="QGraphicsTextItem"` whose text is the given text and whose `font-color="255,255,255,255"`.

#### Task 20: subtitles
**Files:** `commands_subtitle.cpp`

| Command | Params → result |
|---|---|
| `subtitle.list` | `{} → [{id, layer, start, end, text, style}]` |
| `subtitle.add` | `{start, end, text, layer=0} → {id}` (creates the subtitle model if absent) |
| `subtitle.edit` | `{id, text?, start?, end?, style?} → {}` |
| `subtitle.delete` | `{id} → {}` |
| `subtitle.styles` | `{} → [...]`; `subtitle.setStyle {name, style:{…SubtitleStyle fields}} → {}` |
| `subtitle.layer.add` / `subtitle.layer.remove` | `{layer?}` → `{layer}` / `{layer} → {}` (`requestCreateLayer/requestDeleteLayer`) |
| `subtitle.import` | `{path, offset=0} → {}` (`undoable:false`) |
| `subtitle.export` | `{path} → {}` |

Extra test: `subtitle.add` then `subtitle.list` round-trips the text, including non-ASCII (`"héllo ✓"`).

#### Task 21: monitor grab and audio mixer
**Files:** `commands_monitor.cpp`, `commands_mixer.cpp`

| Command | Params → result | Notes |
|---|---|---|
| `monitor.grab` | `{monitor="project", width=640} → {mime:"image/png", width, height, position, data: base64}` | `extractFrame(QString(), w, h)`, height keeps the profile aspect ratio. `needsGui` |
| `mixer.get` | `{trackId?} → [{trackId, volumeDb, pan, muted, solo}]` | `trackId` omitted = all audio tracks plus master (`trackId:-1`) |
| `mixer.set` | `{trackId, volumeDb?, pan?: 0..1} → {}` | `undoable:false`. Pan outside 0..1 → `bad_params`. Call `setDocumentModified()` |
| `mixer.solo` | `{trackId} → {}` | `needsGui` |

Extra test: `mixer.set {pan:1.5}` → `bad_params`. `volumeDb:-6` reads back `-6` from the `volume` filter's `level`.

#### Task 22: MCP phase-4 tools
**Files:** `server.py`

- Add these tools:
  - `import_media(paths, folder_id=None)`, `list_bin()`, `add_color_clip(color, duration, name)`
  - `add_title(text, duration="5s", time=None, track_id=None, style…)`: creates the title, waits until `bin.list` shows `ready`, polling every 200 ms for up to 10 s, then inserts it at the time/playhead on the top video track, all inside one `undo.begin("title")` group
  - `update_title`
  - `add_subtitle(start, end, text)`, `list_subtitles()`, `edit_subtitle`
  - `grab_frame(time=None)`: seeks if `time` is given, waits 300 ms, grabs, and **returns MCP `Image` content** (PNG)
  - `set_volume(track_id, db)`, `set_pan(track_id, pan)`
- Extend `e2e_live.py`: `bin.import` the generated bars (replacing the manual load), add a title, grab a frame (non-empty PNG).
- Commit.

### Phase 5: Project, sequences, render

#### Task 23: project and sequences
**Files:** `commands_project.cpp`

| Command | Params → result | Implementation |
|---|---|---|
| `project.save` | `{path?: string} → {path}` | With `path`: `saveFileAs(path, true, false)`. Without, if the doc has a URL: save. No URL → `bad_params` "path required" |
| `project.discard` | `{} → {}` | Marks the doc unmodified so `new`/`open` won't prompt |
| `project.new` | `{profile?} → {}` | Modified doc → `document_modified` |
| `project.open` | `{path} → {}` | Same check. A missing file → `not_found` |
| `project.setProfile` | `{profile} → {}` | `slotSwitchProfile` (`undoable:false`). An unknown profile → `not_found` |
| `project.profiles` | `{} → [{path, description, fps, width, height}]` | |
| `project.getProperty` / `project.setProperty` | `{name}` → `{value}` / `{name, value} → {}` | `KdenliveDoc::getDocumentProperty/setDocumentProperty` |
| `sequence.list` | `{} → [{uuid, binId, name, active}]` | |
| `sequence.open` | `{uuid} → {}` | |
| `sequence.create` | `{name, videoTracks=2, audioTracks=2} → {uuid}` | |

- `project.new`/`open`/`save` are long operations: reply with `{jobId}` and run them via `QTimer::singleShot(0, …)`.
- Add `job.status {jobId} → {state:"running"|"done"|"failed", error?}` to `commands_project.cpp` and reuse it in Task 24.
- In the `listCommands` description of `project.open`, document that the recovery/backup prompts may appear at the window.

Extra test: a modified doc plus `project.new` → `document_modified`.

#### Task 24: render jobs
**Files:** `commands_render.cpp`

| Command | Params → result |
|---|---|
| `render.presets` | `{} → [{name, category, extension}]` |
| `render.start` | `{preset, output, range:"all"|"zone"|{in, out}="all"} → {jobId}` |
| `render.status` | `{jobId} → {state, progress: 0..100, frame, error?}` |
| `render.abort` | `{jobId} → {}` |

- `render.start` uses `RenderRequest` (`loadPresetParams`, `setOutputFile`, `setBounds`, `setDelayedRendering(false)`), then `process()` and `startDetached(kdenliverendererpath(), argsByJob(job))`.
- Progress comes from connecting to `RenderServer` progress/finished.
- An unknown preset → `not_found`. An existing output file is overwritten (the spec says no prompts).

Extra test: `render.start` with an unknown preset → `not_found`. The live render is checked in Task 25.

#### Task 25: MCP phase-5 tools, mask/transform helpers, full e2e
**Files:** `server.py`, `mcp/kdenlive_mcp/geometry.py`, `mcp/tests/test_geometry.py`, `e2e_live.py`

- `geometry.rect(x, y, w, h, opacity=1.0) -> str` returns `"x y w h opacity"`, with x, y, w, h as ints and opacity formatted as `%g`.
- `geometry.animate(frames_values: list[tuple[int, str]], fps, interp="linear") -> str` returns an MLT animation string, e.g. `"0=…;50~=…"`, with separators per the spec (none = linear, `|` discrete, `~` smooth).
- `geometry.roto(points: list[tuple[float, float]]) -> str` returns rotoscoping JSON. Its handles equal the point, and points are normalised 0..1.
- New tools:
  - `transform_clip(clip_id, x, y, w, h, opacity=1, at=None)`: adds `qtblend` if it isn't on the clip, then sets `rect`, or a keyframe when `at` is given
  - `zoom_clip(clip_id, from_scale, to_scale)`
  - `mask_clip(clip_id, points)`
  - `save_project(path=None)`, `new_project(profile=None)`, `open_project(path)`
  - `list_sequences()`, `open_sequence(uuid)`
  - `list_render_presets()`
  - `render(preset, output, range="all", wait=False)`: when `wait`, polls `render.status` every 1 s
- Tests:
  - `rect(0, 0, 1920, 1080) == "0 0 1920 1080 1"`
  - `animate([(0,"0"),(50,"1")], 25, "smooth") == "0~=0;50~=1"`
  - `roto` of a unit square gives 4 points of 3 pairs each.
- Full e2e: import, insert, cut, transition, title, fade keyframe, then `render` with the first preset whose `extension=="mp4"` to `/tmp/claude-1002/kdenlive-e2e/out.mp4` and `wait=True`. `ffprobe` must report a duration > 0. Expected: `E2E OK`.
- Commit with the message `mcp: add project/render tools and geometry helpers`.
