# lh-voisona-mcp

Get a program **inside** a running **VoiSona**, holding VoiSona's own object graph,
so it can read and (once calibrated) change the open song. Nothing is clicked or
typed: not to install, not to drive.

VoiSona has no scripting API and no plugin API. Its note model is native C++ that
exists only inside the running editor process, so there is nothing to attach to from
outside. This bridge is therefore a **DLL loaded into VoiSona** which hosts an MCP
server *in-process* and publishes it on loopback HTTP. It is the shape of an editor
plugin the vendor does not offer — not screen scraping, and not keyboard or mouse.

One C++ source file builds two artifacts:

| artifact | purpose |
|---|---|
| `lhai_bridge.dll` | injected into VoiSona; hosts MCP in-process |
| `lhai_bridge.exe` | the same server standalone, so the transport can be tested without injecting into anybody's editor |

## Read this before you promise anything

**The transport, the loader, the memory primitives and the MCP surface are built and
measured. The note vocabulary is not.** Nothing here hard-codes where VoiSona keeps
its note list, because those offsets move between builds, and a stale offset reads
plausible garbage and writes it back. `bridge_status` therefore reports
`capabilities.notes: "none"` until a note map has been learned, and that is the
honest state rather than a missing step.

What that means in practice:

* **Works now:** attach and detach, process and module facts, signature scanning,
  raw reads and writes by address or pointer chain, window enumeration, a wrong-engine
  guard, and a learned-vocabulary loader.
* **Needs a human once:** finding where the note list lives. The intended route is a
  diff while a person edits one note by hand (`song_learn`), then `*_load_map` binds
  that vocabulary. Until someone does that, note-level editing is not available.
* **Also available as fallbacks:** the UIA live layer and the project-file route.

## Requirements

* Windows, VoiSona installed, and the editor **running** before you attach.
* Python 3.10+.
* `ziglang` — the only toolchain the native bridge needs. It ships its own clang,
  linker and Windows headers, and the resulting DLL depends only on system DLLs, so
  nothing has to be installed on the machine that runs VoiSona.

## Install

```powershell
git clone https://github.com/ppzbr/lh-voisona-mcp
cd lh-voisona-mcp
python -m pip install -e ".[mcp]"
python -m pip install comtypes
python -m pip install ziglang
```

Build the bridge:

```powershell
powershell -ExecutionPolicy Bypass -File bridges/native/build.ps1
```

It writes `lhai_bridge.build-info.txt` beside the artifacts. `csc`-style bit
determinism is not available for this target, so the build record — not the compiler —
is the proof of what was built. If the build fails, stop and report the compiler
output; do not paper over it.

## Prove the transport with no editor involved

Run `lhai_bridge.exe` and drive it over HTTP:

```
initialize   -> serverInfo LhBridge, protocol 2024-11-05
tools/list   -> 8 tools
```

This step exists so that a later failure is known to be about *the host*, not about
the bridge. Do it first, every time.

## Attach, and know that it worked

```python
bridge_inject(engine="voisona")
```

A healthy injection reads back like this (counts are from the development machine;
the shape is what matters):

```
host image in the module list     17 modules in the standalone, 107 inside VoiSona
scan "4D 5A"                      found at the image base
chain u16 at the image base       0x5A4D  ("MZ")
read at the image base            4d5a90000300000004000000ffff0000
windows                           1 (the editor's own)
status                            level: partial, capabilities.notes: "none"
```

Then the surface:

```python
bridge_list()                     # every bridge answering right now, and its engine
bridge_status(engine="voisona")   # version, app baseline, per-capability levels
bridge_tools(engine="voisona")    # the 8 tools with their real argument schemas
bridge_call(engine="voisona", tool=..., arguments=...)
```

The eight tools the bridge publishes:

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

The wrong-engine guard is deliberate: asking a VoiSona bridge for a `vocaloid_*`
tool answers `ok: false, error: "tool <name> does not belong to engine unknown"`.
If you see that, you are talking to the wrong bridge or the host does not match its
tools.

`chain` resolves exactly as a map file states it: `p = base; for each offset o:
p = *(void**)p + o`, then read the typed value at `p`.

## Detach

```python
bridge_eject(engine="voisona")
```

**Always eject before rebuilding.** Windows will not overwrite a DLL that is loaded in
a running process, and replacing the file behind a live bridge is how you get an
editor with a half-mapped image.

## Rules this bridge lives by

1. Do not modify, replace or delete any VoiSona file.
2. Never kill the editor process. If it holds unsaved work, that is not yours to lose.
3. Never substitute UI automation, keystrokes or clipboard for a bridge capability.
4. If a step fails, stop at that step and report exactly what happened.

## When something looks wrong

| symptom | cause |
|---|---|
| `bridge_inject` cannot find the process | VoiSona is not running, or it runs under a different name — check `live_status()` |
| the DLL will not overwrite | it is loaded in the process: `bridge_eject` first |
| the endpoint answers but the engine tools refuse | the wrong-engine guard |
| `capabilities.notes: "none"` and you expected edits | no note map is loaded; that is the state, not a bug |
| `comtypes` ImportError | `python -m pip install comtypes` |

## Layout

| path | what |
|---|---|
| `prompt.md` | the paste-ready brief: the whole install-and-drive story for an agent that has never seen this repository |
| `bridges/native/lhai_bridge.cpp` | the bridge: one file, two artifacts |
| `bridges/native/build.ps1` | the build, and the build record it writes |
| `bridges/native/install.py` | injection without an MCP client (`python bridges/native/install.py voisona`) |
| `lhai/live/` | the client: the injector, the live layer, and the folder/named-pipe channels |
| `lhai/mcp/` | the MCP server, which is how an MCP client reaches `bridge_*` |
| `docs/mcp-engine-bridge.md` | the contract every bridge in this family follows: transport, tool vocabulary, diagnostics |
| `tests/` | the client's suite, plus the repository's own consistency checks |
| `MANIFEST.txt` | sha256 of every tracked file; `python tools/make-manifest.py --check` re-derives it |

The client ships with adapters for the other editors in the family
(`lhai/mcp/engines/`). They are inert unless that editor is the target; the tree is
kept whole so that one client has one copy and one test suite.

## Tests

```powershell
python -m pip install -e .
python -m pytest
```

## Licence

MIT. See `LICENSE`.
