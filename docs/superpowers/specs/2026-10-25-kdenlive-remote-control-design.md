# Kdenlive Remote Control + MCP Server — Design

Date: 2026-10-25
Fork: https://github.com/priyanshusaini105/kdenlive (upstream: invent.kde.org/multimedia/kdenlive)

## Intent

The user edits video in Kdenlive and knows how to edit. They do not want an AI to
edit end to end. They want to stay the editor and delegate individual steps in plain
language ("dissolve between these two", "fade out the last 2s of the clip under the
playhead", "blur V2 from 1:10 to 1:15", "add a title saying X"), and see each change
appear **live in the open Kdenlive window**.

Stock Kdenlive cannot do this. Its D-Bus surface (`/kdenlive/MainWindow_1`,
`org.kde.kdenlive.MainWindow`) has only `addEffect`, `addProjectClip`,
`addTimelineClip`, `scriptRender` and 28 generic KStandardActions, and nothing can
read state back. The fork therefore adds a control interface inside Kdenlive. A
separate MCP server exposes it to the AI.

## Goals

- **Full control of editing**, live, in the running app: timeline, tracks, clips,
  groups, effects and keyframes, compositions and mixes, bin, titles, subtitles,
  guides and markers, zone, monitors, audio mixer, sequences, project, rendering,
  plus every registered QAction.
- **The AI can read state back**: timeline contents, playhead, selection, effect
  parameters, project profile.
- **The AI can "see"**: grab the current monitor frame as an image.
- **Edits are undoable** with Ctrl+Z wherever Kdenlive itself supports undo.
- Users point at things by **playhead, selection, or timecode**.

## Non-goals

- Autonomous end-to-end editing, or the AI analysing footage by itself. Frame grab is
  on demand only.
- Driving Kdenlive's dialogs. "Full control" means the model-level equivalent of what
  a dialog does (e.g. a title is created from title XML, not by clicking the titler).
- Upstreaming in this project. Keep it upstreamable, but don't spend effort on it.
- Windows/macOS, multi-user, remote (non-local) access.

## Anti-goals (it is a failure even if it "works")

- **A call pops a modal dialog.** The dialog blocks the D-Bus reply, which times out
  after about 25s, and leaves the GUI stuck. Never call UI slots that `exec()`, open a
  `KMessageBox` or open a file dialog.
- **A bad id crashes Kdenlive.** Model getters `Q_ASSERT` and then `.at()` the id.
  Every id must be validated (`isClip/isTrack/isComposition/isItem`) before use.
- **AI edits that can't be undone** where a normal GUI edit could be.
- **A sprawling diff into upstream files.** That makes rebasing on upstream painful.

## Constraints

- Machine: Arch Linux; Kdenlive 26.08.1 stock installed and often running. All
  build dependencies are present (CMake configure verified; KF6DocTools is missing
  but only needed for docs).
  - `/home` has ~13 GB free, so use a **Release** build.
  - ~3 GB RAM is free while working, so use `ninja -j4`.
- The fork is built from a shallow clone at `/home/work/personal/kdenlive/src`.
  - Remotes: `origin` = GitHub fork, `upstream` = invent.kde.org.
  - Work happens on branch `remote-control`. Nothing is pushed without the user's say-so.
- **The fork is for experiments and test projects only** (user decision). It stays on
  upstream master (26.11.70 dev); the stock 26.08.1 install remains the daily editor.
  The fork must not touch the stock app's state:
  - Install to its own prefix, `~/.local/opt/kdenlive-rc`, with `cmake --install`. Its
    data files (effect XML etc.) then match its version, instead of reading
    `/usr/share/kdenlive` from 26.08.
  - Launch it through a wrapper script that puts the prefix first in `XDG_DATA_DIRS`
    and uses a separate `XDG_CONFIG_HOME`/`XDG_DATA_HOME` (under
    `~/.local/opt/kdenlive-rc/home/`), so `kdenliverc` and `~/.local/share/kdenlive`
    are not shared.
- Kdenlive is GPL. The fork stays GPL with notices intact. No rebranding.
- Kdenlive's bus name is pid-suffixed (`org.kde.kdenlive-<pid>`, from `KDBusService`
  Multiple at `main.cpp:575`). The stock app and the fork can run at the same time.

## Architecture

```
Claude Code ──MCP(stdio)──▶ kdenlive-mcp (Python) ──D-Bus session──▶ forked Kdenlive
                                                                     /RemoteControl
```

### 1. Fork: `src/remotecontrol/` (C++)

- `RemoteControl : QObject` with `Q_CLASSINFO("D-Bus Interface", "org.kde.kdenlive.RemoteControl")`.
  - Register it in `Core::initGUI` right after `m_guiConstructed = true;`
    (`core.cpp:440`): `QDBusConnection::sessionBus().registerObject("/RemoteControl", obj, ExportAllSlots|ExportAllSignals)`,
    guarded by `#ifndef NODBUS`.
  - Calls arrive on the GUI thread, so no marshalling is needed.
- **The D-Bus surface is a JSON dispatcher, not one D-Bus method per operation:**
  - `QString call(QString command, QString paramsJson)` returns
    `{"ok":true,"result":…}` or `{"ok":false,"error":{"code","message"}}`.
  - `QString listCommands()` returns each command's name, a one-line description, and its parameters.
  - Why: dozens of operations without D-Bus type marshalling; one place for
    validation and undo grouping; new commands need no interface changes.
  - Kdenlive builds with `-fno-exceptions`, so nothing can be caught. Every id and
    parameter is validated **before** any model call, and errors are return values.
- **Units are frames** at the D-Bus layer. Positions inside an effect are item-local
  (relative to the clip's in point), matching Kdenlive's own keyframe positions.
- **Undo:** each mutating command is one undo step, using Kdenlive's own undo
  labels. Groups are labelled `AI: <label>` (see below). Commands use model-level
  functions with `Fun& undo, Fun& redo`, or ones that push their own undo.
  - Known exceptions with no undo in Kdenlive itself: mixer volume/pan, profile
    switch, subtitle import. Each must be flagged `undoable:false` in `listCommands`.
- **One Ctrl+Z per user request** (user decision). One plain-language request
  usually takes several commands, so they are grouped:
  - `undo.begin {label}` / `undo.end` map to `QUndoStack::beginMacro`/`endMacro` on
    `pCore->undoStack()` (a `DocUndoStack : QUndoStack`). Every command, whether
    it uses `pushUndo` or a model's own `PUSH_UNDO`, lands on that stack, so the
    whole request becomes one entry labelled `AI: <label>`.
  - Safety: if a group stays open with no AI call for 10s, the dispatcher closes
    it, so a crashed AI never swallows the user's own GUI edits into an open macro.
    `undo`/`redo` close any open group first. Nested `begin` is an error.
  - MCP side: the high-level tools each open and close their own group. A
    `begin_request(label)` / `end_request()` tool pair lets the AI group a
    multi-tool request. The server's instructions tell the AI to wrap every user
    request in it.
- **Long operations** (open, save-as, render): reply immediately with a job id;
  `job.status` polls the result. Render progress comes from `RenderServer`.
- **Access to protected model internals:** add `friend class RemoteControl;` to the
  `TimelineModel` friend list (`timelinemodel.hpp:97-112`), the same trick
  `KdenliveTests` uses. Outside `src/remotecontrol/`, keep upstream edits to: that
  friend line, the registration lines in `core.cpp`, and `CMakeLists.txt`.

### 2. Command catalog (full control)

Entry points for each area are mapped in the research notes below. Namespaces:

| Namespace | Covers |
|---|---|
| `project` | info (fps, size, profile), new, open, save, save-as, set document property |
| `sequence` | list, active, open/switch, create |
| `timeline` | get (tracks + clips + compositions + mixes), playhead, selection get/set, zone get/set, duration |
| `track` | add, delete, rename, lock, hide/mute, move, track effects |
| `clip` | insert from bin, move, resize/trim, slip, ripple-trim, cut, delete, speed, enable/disable, split audio/video, copy/paste |
| `edit` | lift/extract zone, insert/remove space, delete blank |
| `group` | group, ungroup |
| `composition` / `mix` | add (composition between tracks, same-track mix), list, params, delete |
| `asset` | list effects/transitions (id, name, type, audio?), parameter schema (from repository XML) |
| `effect` | add (with initial params), remove, reorder, enable, get/set params. Targets a clip, track, master, or bin clip |
| `keyframe` | add, remove, move, set value, set interpolation; or set a whole MLT animation string |
| `bin` | list tree, import file(s), folder create/rename, color clip, delete, rename, properties get/set, proxy |
| `title` | create title clip from a structured spec (text, font, size, color, position, background, scroll), edit existing |
| `subtitle` | list, add, edit, resize, move, delete, styles, layers, import/export |
| `marker` | guides and clip markers: list, add, add range, edit, remove, categories |
| `monitor` | seek, play/pause, position, **grab frame (PNG, base64)** |
| `mixer` | track volume (dB), pan, mute, solo, master |
| `render` | list presets, render (preset, output, whole/zone/guides) as a job, status, abort |
| `action` | list (name, label, shortcut, enabled), trigger. This is the catch-all for anything else |
| `undo` / `redo` | step the document undo stack |

Masks, rotoscoping and transforms are not separate commands. They are effect
parameters (`qtblend` `rect` = `"x y w h opacity"`; `rotoscoping` `spline` = JSON)
set with `effect`/`keyframe` commands. The MCP layer may offer helpers.

### 3. MCP server: `mcp/` in the fork repo (Python)

- Python 3.14, packaged with `uv`. Dependencies: the official `mcp` SDK, plus
  `dbus-fast` or `jeepney` (both already installed). Its own license file may differ
  from GPL, because it is a separate process.
- **Instance discovery:** find bus names `org.kde.kdenlive-*` that expose `/RemoteControl`.
  - Exactly one: use it.
  - Several: `KDENLIVE_PID` env var, or a `select_instance` tool.
  - None: a clear error ("start the forked Kdenlive build").
- **Timecode conversion** using fps from `project.info`. It accepts `HH:MM:SS:FF`,
  `MM:SS(.sss)`, seconds (`"2.5s"`) and raw frames.
- **Tool surface:**
  - A curated set of ~20 high-level tools for common edits (get timeline, cut,
    move, trim, delete, add effect, set param/keyframe, add transition, add title,
    seek, grab frame, undo, trigger action…). They accept timecodes and default to the
    playhead/selection when a target is omitted.
  - Plus a generic `kdenlive_call(command, params)` and `kdenlive_commands()` so
    everything in the catalog is reachable without one tool per command.
- `monitor.grab` is returned as MCP image content so the model can look at the frame.
- Registered in Claude Code (`claude mcp add`).

## Testing

- **C++:** `tests/remotecontroltest.cpp`, using the existing headless harness
  (`tests/TestMain.cpp`, `KdenliveTests`). It covers the dispatcher: bad JSON, an
  unknown command, invalid ids (must return an error, not crash), one representative
  edit per namespace, and undo of an AI edit.
- **Python:** one small test of timecode parsing and conversion.
- **End to end against the live build:** a short scripted scenario on a test project,
  using generated media (e.g. `melt`/ffmpeg color bars and tone). Steps: import,
  insert, cut at 0:05, fade out the last 2s, add a dissolve, add a title, grab a
  frame, undo once. The scenario is checked through `timeline.get`.

## Delivery order

Each phase ends with a working build and an end-to-end check.

1. **Thin slice:** dispatcher and registration, `listCommands`, `timeline.get`,
   `clip.cut`, `effect.add` and `effect.set`, `action.list` and `action.trigger`,
   `undo`; MCP server with discovery, timecodes, the matching tools and `kdenlive_call`.
2. Timeline editing: clip, edit, group, track, marker, zone, selection, monitor seek/play.
3. Assets and effects in depth: `asset` schemas, keyframes, compositions, mixes.
4. Bin, titles, subtitles, monitor grab, mixer.
5. Project, sequences, render jobs. Mask/transform helpers in MCP.

## Left to the builder

- Exact JSON shapes for each command, and internal C++ file layout inside `src/remotecontrol/`.
- `dbus-fast` vs `jeepney`.
- The exact curated MCP tool list and its wording.
- Whether frame grab returns base64 in the JSON or writes a temp file.

## Calls made by the designer (not confirmed by the user)

- JSON dispatcher over a typed D-Bus API.
- MCP server in Python, living in the fork under `mcp/`.
- Release build, `-j4`; run from the build directory alongside the stock install.
- Content-based pointing ("the shot where she laughs") is handled only through the
  on-demand frame grab, not automatic analysis.
- **When "no dialog" conflicts with "minimal upstream diff":** the command refuses
  with a clear error rather than prompting. For example, `project.open`/`project.new`
  on a modified document return `document_modified`, and the AI must save or
  discard first (`project.save-as`, or `project.discard` which clears the modified
  flag). Cases that can't be pre-checked (backup or recovery prompts in `openFile`)
  are documented as "may prompt the user at the window". Small upstream "silent" flags
  are added only if a case proves unworkable this way.
- **`action.trigger` is deferred** (`QTimer::singleShot(0, …)`), so the D-Bus reply
  returns first. If the action opens a dialog, the user, who is at the window,
  handles it. `action.list` returns the action's text, so the AI can tell when an
  action is "…"-style (opens a dialog). There is no allow/deny list.
- The order of the delivery phases.

## Research notes (entry points)

All paths are relative to `src/src/`. Line numbers are from upstream commit `406de22`.

- **Undo:** `pCore->pushUndo(Fun,Fun,QString)` `core.h:215`. Model getters are at `core.h:126-169`.
  `triggerAction` is at `core.h:450`; the action lookup is `core.cpp:1779`.
- **Active timeline:** `pCore->projectManager()->getTimeline()` `projectmanager.h:120`.
  Sequences: `getTimelinesUuids`, `openTimeline(binId,-1,uuid)`, `ClipCreator::createPlaylistClip`.
- **Clips:** `getItemsInRange(tid,0,-1,true)` `timelinemodel.hpp:873`; getters at `:226-358`.
  - `requestItemResize` `:641`, `requestClipSlip` `:661`, `requestClipTimeWarp` (public overload takes percent; RC converts its ratio ×100) `:912`.
  - `TimelineFunctions::requestClipCut`, `extractZone` (`timelinefunctions.hpp:40,132`).
- **Tracks:** `requestTrackInsertion` `:713`, `requestTrackDeletion` `:723`, `setTrackName`,
  `setTrackLockedState` `:432`. `hide` property: audio 1/3, video 2/3 (`trackmodel.cpp:1657`).
  - **Avoid** `beginAddTrack`, `deleteMultipleTracks`, `changeItemSpeed`: they open dialogs.
- **Effects:** `getClipEffectStackModel` `:300`, `getTrackEffectStackModel` `:903`,
  `getMasterEffectStackModel` `:904`; `EffectStackModel::appendEffect(id,false,params)`.
  - Parameter writes: `pCore->pushUndo(new AssetCommand(model, idx, value))` (as `assetparameterview.cpp:212`).
  - Keyframes: `KeyframeModelList` (`addKeyframe`, `updateKeyframe`, `updateKeyframeType`).
    MLT animation string syntax is at `keyframemodel.cpp:23-45`.
  - Hidden built-in effects occupy stack rows. Filter them with `hideFromStack()`.
- **Repositories:** `EffectsRepository::get()`, `TransitionsRepository::get()`;
  parameter schema from `getXml(id)`.
- **Bin:** `ClipCreator::createClipFromFile` / `createColorClip` / `createTitleClip({{"xmldata",xml}},…)`.
  `requestAddFolder`, `requestBinClipDeletion` plus `pushUndo`, and `Bin::slotEditClipCommand` for properties.
  - Bin clips load asynchronously: check `statusReady()` or use `readyCallBack` before inserting them.
- **Title XML format:** `data/titles/simple.kdenlivetitle`, `TitleDocument::xmlItem` `titler/titledocument.cpp:333`.
- **Subtitles:** `getSubtitleModel()` (may be null → `createSubtitleModel()`), `addSubtitle/editSubtitle/…` `subtitlemodel.hpp:113-271`.
- **Markers/guides:** `MarkerListModel` (`addMarker`, `addRangeMarker`, `editMarker`, `removeMarker`, `getAllMarkers`).
- **Zone:** `TimelineController::setZone(QPoint(in,out),withUndo)`. `out` is exclusive.
- **Monitor:** `pCore->seekMonitor`, `Monitor::switchPlay/pause`;
  frame grab via `monitor->getControllerProxy()->extractFrame(QString(),w,h)` (`monitorproxy.cpp:418`).
- **Mixer:** hidden `volume` (`level` dB) and `panner` (`start` 0..1) filters on the track tractor,
  `internal_added=237` (`mixerwidget.cpp:290-460`). No undo; the slider UI doesn't refresh.
  Solo: `pCore->mixer()->slotSwitchSoloMode`.
- **Render:** `RenderRequest` (headless path `main.cpp:504-530`), `RenderPresetRepository`;
  progress via `RenderServer` (`render/renderserver.h:25`).
- **Project:** `ProjectManager::newFile/openFile/saveFileAs(path,true,false)`.
  `saveFile`/`closeCurrentDocument(true)` can prompt; `openFile` can show backup or missing-clip dialogs.
- **Headless test harness:** `tests/TestMain.cpp:53-80`, `tests/effectstest.cpp:15-35`, `tests/test_utils.hpp`.
