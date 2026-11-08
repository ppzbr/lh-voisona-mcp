# VoiSona — install an in-process bridge and drive it

> Paste everything below the rule into an agent that has shell access to the Windows
> machine running VoiSona. It is written to stand alone.

---

## Goal

Get a program **inside** a running VoiSona, holding VoiSona's own object graph, so it
can read and (once calibrated) change the open song. Nothing may be clicked or typed:
not to install, not to drive.

**Why it has to be this way.** VoiSona has no scripting API and no plugin API. Its
note model is native C++ that exists only inside the running editor process. So the
bridge is a **DLL loaded into VoiSona** that holds that model itself and publishes MCP
on loopback HTTP. This is the same shape as an editor plugin that the vendor does not
offer. It is not screen scraping and it does not use the keyboard or the mouse.

**What it honestly is today.** The transport, the loader, the memory primitives and
the MCP surface are built and measured. The **note vocabulary is not**: nothing
hard-codes where VoiSona keeps its note list, because those offsets move whenever the
editor updates, and a bridge with a stale offset does not fail — it reads plausible
garbage and writes it back. `bridge_status` therefore reports
`capabilities.notes: "none"` until a note-map has been learned. Read that field first
and believe it.

## Rules

1. Do not modify, replace or delete any VoiSona file.
2. Never kill the editor process. If it holds unsaved work, that is not yours to lose.
3. Never substitute UI automation, keystrokes or clipboard for a bridge capability.
4. **Always eject before rebuilding.** Windows will not overwrite a DLL that is loaded
   in a running process.
5. If a step fails, stop at that step and report exactly what happened.
6. Work only inside this repository. Nothing else on the machine is yours to change.

## Step 0 — the repository and dependencies

```powershell
git clone https://github.com/ppzbr/lh-voisona-mcp
cd lh-voisona-mcp
python -m pip install -e ".[mcp]"
python -m pip install comtypes
python -m pip install ziglang
```

* `.[mcp]` brings the `mcp` transport and `comtypes` and nothing heavier: the client
  is stdlib at import time, on purpose, so the bridge is attachable on a machine that
  has VoiSona and a bare Python.
* `comtypes` is what `lhai/live/uia.py` and `lhai/live/cevio_com.py` import; it is
  declared in the `[mcp]` extra above. The explicit line below is a safety net for an
  older checkout, where the UIA/COM half of the tool set cannot import without it.
* `ziglang` provides `zig`, the only toolchain the native bridge needs. It ships its
  own clang, linker and Windows headers, and the resulting DLL depends only on system
  DLLs, so nothing has to be installed on the target machine.

Start the MCP server (`lhai-mcp`, or `lhai-mcp http`) and call the tools below from
your client. With no MCP client the same functions are importable:
`from lhai.live import bridges, inject`.

Confirm VoiSona is actually running before anything else:

```
live_status()
```

It reports which of the four editors are running, their windows, and their live
channels.

## Step 1 — build the bridge

```powershell
powershell -ExecutionPolicy Bypass -File bridges/native/build.ps1
```

One source file, `bridges/native/lhai_bridge.cpp`, builds two artifacts:

| artifact | purpose |
|---|---|
| `lhai_bridge.dll` | injected into VoiSona; hosts MCP in-process |
| `lhai_bridge.exe` | the same server standalone, so the transport can be tested without injecting into anybody's editor |

It writes a build-info file beside them. Compare `bridges/native/lhai_bridge.build-info.txt`
before and after a rebuild: `csc`-style determinism is not available here, so the
build-info is the record. If the build fails, stop and report the compiler output.

## Step 2 — prove the transport with no editor involved

Run `lhai_bridge.exe` and drive it over HTTP:

```
initialize   -> serverInfo LhBridge, protocol 2024-11-05
tools/list   -> 8 tools
```

This step exists so that a later failure is known to be the injection, not the server.

## Step 3 — inject

```
bridge_inject(engine="voisona")
```

or, with no MCP client:

```powershell
python bridges/native/install.py voisona
```

The load is the ordinary `LoadLibraryW`-from-inside sequence in `lhai/live/inject.py`.
No administrator rights are needed for the inject itself; the target must be running.

To undo:

```
bridge_eject(engine="voisona")
```

```powershell
python bridges/native/install.py voisona --eject
```

The unload is not symmetric in the obvious way: a thread cannot free the module it is
running in, so the DLL publishes `LhaiBridgeUnload`, the client computes that export's
remote address (module base + RVA, read from the on-disk PE) and runs it with
`CreateRemoteThread`; the export schedules the real `FreeLibraryAndExitThread` on a
fresh thread inside the target.

## Step 4 — verify

```
bridge_list()                     # every bridge answering right now, and its engine
bridge_status(engine="voisona")   # version, app baseline, per-capability levels
bridge_tools(engine="voisona")    # the 8 tools with their real argument schemas
```

Then the primitives, all through `bridge_call(engine="voisona", tool=..., arguments=...)`:

```
voisona_modules   -> the loaded modules with base, size and path
voisona_windows   -> this process's own top-level windows
voisona_scan      -> signature scan with ?? wildcards, e.g. "4D 5A"
voisona_read      -> raw bytes, by address or through a pointer chain
voisona_write     -> patch bytes
voisona_chain     -> module + RVA + pointer chain -> typed value
voisona_load_map  -> bind a learned note vocabulary
voisona_status    -> bridge version, engine identity, per-capability levels
```

A healthy injection reads back like this (the numbers are from the machine this
bridge was developed on; the shape is what matters):

```
host image in the module list     17 modules in the standalone, 107 inside VoiSona
scan "4D 5A"                      found at the image base
chain u16 at the image base       0x5A4D  ("MZ")
read at the image base            4d5a90000300000004000000ffff0000
windows                           1 (the editor's own)
status                            level: partial, capabilities.notes: "none"
```

The wrong-engine guard is deliberate: asking a VoiSona bridge for `vocaloid_*` answers
`ok: false, error: "tool <name> does not belong to engine unknown"`. If you see that,
you are talking to the wrong bridge or the host does not match its tools.

`chain` resolves exactly as a map file states it: `p = base; for each offset o:
p = *(void**)p + o`, then read `type` at `p`. Types: `u8 u16 u32 u64 i8 i16 i32 i64
f32 f64 ptr utf8 utf16 bytes`, with `count` for arrays.

## Step 5 — the note vocabulary is data, and learning it needs the human

The vocabulary lives in a JSON note-map, learned once per **build** of the editor — not
per editor. The offsets move on update, and a stale map is worse than no map: the
bridge cannot tell a wrong offset from a right one, which is exactly why `load_map`
takes the identity it was exported for.

To learn one:

1. `voisona_modules` for the base addresses.
2. `voisona_scan` for a signature near the note structure — a lyric string adjacent to
   a pitch and a duration is the usual anchor.
3. `voisona_chain` to walk candidate chains from the module base to a value that
   changes when **exactly one note is edited by hand in the editor**.
4. Write the confirmed chains into a note-map JSON and call `voisona_load_map`.
5. `voisona_status` should then read `capabilities.notes: "mapped"`.

Step 3 cannot be automated away. A value that moves when a note moves is the only
evidence a chain is right. So this step needs the human: ask them to change one note
(then another, to be sure) while you diff memory. Confirm against the running editor,
never against intuition.

**A note-map is not a substitute for the note API you were hoping for.** It binds a
vocabulary you describe; it does not give you an add-note/delete-note call the vendor
never wrote. Say so before you start.

## Step 6 — what to use today for note-level editing

Until a note-map exists, put the bridge in its place: it is a **memory and transport
instrument**, and the working channels for editing a song are these two.

**A. The live layer, over UI Automation — no bridge needed.**

```
song_caps(engine="voisona")         # the authority on what can be changed, and through which channel
song_map(engine="voisona")          # the control map in use: name -> automation id
song_learn(engine="voisona")        # measure the toolbar and record which control is which
song_transport(engine="voisona", action="play")
song_lyrics(engine="voisona", text="...")     # fill the selected notes, clipboard-pasted
song_open(engine="voisona", path="song.mid")
song_save(engine="voisona")
song_undo_redo(engine="voisona", redo=False)
song_snapshot(engine="voisona")
```

`song_learn` exists because some editors' control ids have to be measured rather than
guessed; run it once per editor version and the result is the map. `song_caps` is the
authority — run it before promising anything to a human. **Do not describe a
capability to the human before `song_caps` or `bridge_status` has reported it.**

**B. The file route, which needs no running editor at all.**

```
engines()                                   # the engines this server knows, and their channels
project_create(name="song", bpm=120)
notes_add(project="song", track="Vocals", notes=[{"start": 0, "duration": 1, "lyric": "la", "pitch": 60}])
lyrics_fill(project="song", track="Vocals", text="la la la")
export(project="song", engines=["voisona"])  # write VoiSona's own format
export_midi(project="song")                  # the one format every engine imports
open_in(project="song", engine="voisona")    # export, then launch the editor with it
```

## Step 7 — when it goes wrong

| symptom | cause |
|---|---|
| `bridge_inject` cannot find the process | VoiSona is not running, or it is running under a different name — check `live_status()` |
| the DLL will not overwrite | it is loaded in the process: `bridge_eject` first |
| the endpoint answers but the engine tools refuse | the guard in step 4 — the host does not match the tool prefix |
| `capabilities.notes: "none"` and you expected edits | no note-map is loaded; that is the state, not a bug |
| `comtypes` ImportError | `python -m pip install comtypes` |

Stop at the first unexplained failure and report it with the text you saw, including
the build-info hash and the `bridge_status` payload. Never restart or kill an editor
that holds unsaved work; ask the human first.
