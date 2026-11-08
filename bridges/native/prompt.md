# The native bridge — how the pieces fit

`lhai_bridge.cpp` is one file that builds two things:

| artifact | what it is for |
|---|---|
| `lhai_bridge.dll` | injected into VoiSona / VOCALOID 6; hosts MCP in-process |
| `lhai_bridge.exe` | the same server standalone, so the transport is testable without injecting into anyone's editor |

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

`zig` is the only toolchain required — it ships its own clang, linker and Windows
headers, and the output depends only on system DLLs, so there is nothing to
install at the other end.

## Installing into an editor

```
python install.py voisona
python install.py vocaloid
python install.py voisona --eject
```

The load is the ordinary `LoadLibraryW`-from-inside sequence in
`lhai/live/inject.py`. The unload is not symmetric in the obvious way: a thread
cannot free the module it is running in, so the DLL publishes
`LhaiBridgeUnload`, the client computes that export's remote address (module base
+ RVA, read from the on-disk PE) and runs it with `CreateRemoteThread`. The export
schedules the real `FreeLibraryAndExitThread` on a fresh thread inside the target.

Always eject before rebuilding. Windows will not overwrite a loaded DLL.

## The tools

Names are prefixed with the engine id, taken from the running editor's own file
name, so one client can drive both editors without special cases:

```
<engine>_status       bridge version, engine identity, per-capability levels
<engine>_modules      loaded modules with base, size and path
<engine>_scan         signature scan, "48 8B ?? ??" wildcards
<engine>_read         raw bytes, by address or through a pointer chain
<engine>_write        patch bytes
<engine>_chain        module+RVA -> pointer chain -> typed value
<engine>_windows      this process's windows
<engine>_load_map     bind the note vocabulary onto chains, from a JSON map
```

`chain` resolves like a map file says: `p = base; for each offset o: p = *(void**)p
+ o`, then read `type` at `p`. Types: `u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 ptr
utf8 utf16 bytes`, with `count` for arrays.

## Why the note vocabulary is data

Nothing here hard-codes where VoiSona or VOCALOID 6 keeps its note list, because
those offsets move whenever the editor updates, and a bridge with a stale offset
does not fail — it reads plausible garbage and writes it back. So the offsets live
in a JSON note-map, learned once per build, and `status` reports
`capabilities.notes: "mapped"` only once one is loaded.

The alternative — a bridge that ships with offsets compiled in and lies after the
first update — is worse than a bridge that says `none` and explains why.

## Learning a note-map

1. `<engine>_modules` for the base addresses.
2. `<engine>_scan` for a signature near the note structure — a lyric string
   adjacent to a pitch and a duration is the usual anchor.
3. `<engine>_chain` to walk candidate pointer chains from the module base to a
   value that changes when you edit exactly one note by hand in the editor.
4. Write the confirmed chains into a note-map JSON and call `<engine>_load_map`.
5. `<engine>_status` should then read `capabilities.notes: "mapped"`.

Step 3 is the one that cannot be automated away: a value that moves when a note
moves is the only evidence a chain is right. Confirm against the running editor,
never against intuition.

## Verified

The whole MCP surface was exercised by running `lhai_bridge.exe` standalone and
driving it over HTTP:

```
initialize      -> serverInfo LhBridge 0.1.0, protocol 2024-11-05
tools/list      -> 8 tools
<engine>_modules-> 17 modules, first is the host image
<engine>_scan   -> "4D 5A" found at the image base
<engine>_chain  -> u16 at the image base reads 23117 == 0x5A4D == "MZ"
<engine>_windows-> 1 window
voisona_status  -> {"ok": false, "error": "tool voisona_status does not belong
                    to engine unknown"}   (the wrong-engine guard)
```

The `_export_rva` resolver in `lhai/live/inject.py` was checked against the built
DLL: `LhaiBridgePort` → `0x1380`, `LhaiBridgePid` → `0x1390`,
`LhaiBridgeUnload` → `0x13a0`, and a name that does not exist → `0x0`.

## Verified inside the editors

Both targets were then injected for real, one after the other on this machine:

```
VoiSona.exe 1.18.0.5    -> http://127.0.0.1:43102/mcp
VOCALOID6.exe 6.13.0.1  -> http://127.0.0.1:43102/mcp
```

For each: 8 tools published *with their real argument schemas*, the host image
found in the module list (107 modules for VoiSona, 255 for VOCALOID6), `scan
"4D 5A"` hitting the image base, a `u16` read there returning `0x5A4D`, `read`
returning the real header `4d5a90000300000004000000ffff0000`, `windows` listing
the editor's own top-level windows, and the wrong-engine guard rejecting the
other engine's tools. `status` reports `level: partial` with
`capabilities.notes: "none"` and the reason attached, because no note-map is
loaded yet — which is the honest answer, not a placeholder.

Two things the injection itself turned up:

* **VOCALOID6's editor is WPF on .NET, not a native C++ editor.** Its host image
  is only `0x3d000` bytes and its windows are `HwndWrapper[VOCALOID6;;...]`, so
  the editor is a shim over managed assemblies. A note-map is a chain of raw
  pointers read from a module base, and the GC *moves* managed objects, so that
  chain can go stale under you. VoiSona is JUCE and native and fits the workflow
  as written; VOCALOID6 needs this question answered before a discovery pass is
  worth attempting there.
* a note-map is per **build**, not per editor. `VoiSona 1.18.0.5` and
  `VOCALOID6 6.13.0.1` need separate maps even though both go through the same
  DLL, and a map learned for one build must not be loaded against another — the
  bridge cannot tell a wrong offset from a right one, which is exactly why
  `load_map` takes the identity it was exported for.
