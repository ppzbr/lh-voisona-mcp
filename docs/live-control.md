# Live control of the four editors

`lhai/mcp` can *write files* for every engine. This document is about the other
half: driving the **running** editor — CeVIO AI, VoiSona, Synthesizer V Studio and
VOCALOID 6 — without closing it and without a reload.

The target is **singing synthesis**, so the surface that matters is the song:
notes, lyrics, phonemes, pitch, the parameter lanes, transport. CeVIO's *talk*
half has a real automation API and the song half does not, but talk is a side
branch here — it is documented in section 6 because it exists, not because it is
the point.

## The one table that decides everything

Four engines, four very different amounts of surface, and no two are the same:

| engine | strongest channel | notes | lyrics | pitch | transport |
|---|---|---|---|---|---|
| **synthesizer v** | `lua` — sanctioned in-process API | read + write | per note | offset per note | play / stop / seek |
| **CeVIO AI** | `uia` | file open (`.ccs`) | **bulk dialog (live)** | parameter lane + pen | play / stop / head / markers |
| **VOCALOID6** | `uia` | file open (`.vsqx`) | clipboard paste | parameter lane + pen | play / stop / return to top |
| **VoiSona** | `uia` | file open | clipboard paste | parameter lane + pen | play / stop |

Read it as a statement about *mechanism*, not about effort: "notes: file open" is
not a shortcut, it is the truth — on three of the four engines the score is a
canvas, not a control, and there is nothing to set a note's pitch *through*
except the file or the mouse. `song_caps` returns this table live.

```python
from lhai.live import song
song.caps()            # all four
song.caps("cevio")     # one
```

`live_status` reports which engines are up; `song_caps` reports what can be done
to the one that is.

---

## 1. The song surface — `song_*`

| tool | does |
|---|---|
| `song_caps [engine]` | the capability table above |
| `song_modes engine` | which **tool**, **lane** and **transport mode** are active *right now* |
| `song_transport engine action` | `play` / `stop` / `head` / `repeat` / `marker_start` / `marker_end` |
| `song_tool engine name` | `arrow` / `pen` / `line` / `eraser` / `bulk` |
| `song_lane engine name` | `volume` / `pitch` / `vibrato` / `alpha` / `husky` / `timing` |
| `song_lyrics engine text [phonemes]` | fill the selected notes with one syllable per note |
| `song_readouts engine` | tempo, time signature, position, quantize |
| `song_snapshot engine [path]` | save + parse → the open song, as data |
| `song_open engine path` | open a project in the running editor |
| `song_undo_redo engine [redo]` / `song_save engine` | the editor's own stack and save |
| `song_map engine` / `song_learn engine` | the control map, and how to measure one |

Two properties are structural rather than decorative:

* **state is read back, not assumed.** Pressing a radio button is not evidence it
  became the active one. `song_transport`, `song_tool` and `song_lane` re-read the
  toggle/selection state and return it as `active`, and `song_modes` reads the
  whole toolbar. A batch is therefore checkable without a human watching.
* **a refusal names its reason.** "control is disabled", "no control mapped",
  "not running" and "control not found in tree" are four different answers and
  come back as four. CeVIO disables the tools and every parameter lane until a
  song track exists, and that is reported as exactly that — with the hint — rather
  than as a generic failure.

The six lanes are the reason this surface exists: volume, **pitch**, **vibrato**,
alpha, husky and timing are the curves a vocal is actually shaped with, and
switching between them is the difference between "notes exist" and "a performance
exists".

### The control map

Automation ids are stable per build and per install language, so CeVIO's map
ships and is not guessed:

```json
{"transport": {"play": "playButton", "stop": "stopButton", "head": "headButton", ...},
 "tools":     {"pen": "penButton", "arrow": "arrowButton", ...},
 "lanes":     {"volume": "volButton", "pitch": "pitButton", "vibrato": "viaButton",
               "alpha": "alpButton", "husky": "husButton", "timing": "tmgButton"},
 "readouts":  {"tempo": "tempoText", "beat": "beatText", "quantize": "quantizeText"}}
```

VOCALOID6 and VoiSona are **not** guessed. `song_learn engine` walks the running
toolbar, keeps only visible elements with a clickable rectangle, derives a name
from each id (`pitButton` → `pitch`), saves the map, and every later call uses it.
A guessed id clicks something else, which is worse than a missing one.

---

## 2. The native agent (`lhai/live/native/lhai_agent.dll`)

The lowest layer, and the one that makes VoiSona and VOCALOID6 reachable beyond
their UI at all: neither ships a scripting API, so a model-level edit can only
happen through their own memory.

### What it is

A ~480 KB DLL with **no CRT dependency** (freestanding C++, `kernel32`/`user32`
only), built by `agents/native/build.ps1`. It starts a thread on `DllMain`, opens
a named pipe `\\.\pipe\lhai_agent.<pid>`, and serves a request loop until
`UNLOAD`.

### Injection

```python
from lhai.live import inject, agent
inject.inject(pid)          # OpenProcess -> VirtualAllocEx -> WriteProcessMemory -> CreateRemoteThread(LoadLibraryW)
agent.ensure(pid)           # ping, and inject only if nothing answers
agent.ping(pid)             # {'ok': True, 'info': {'pid', 'arch', 'tid', 'exe', 'agent'}}
inject.eject(pid)           # CreateRemoteThread(FreeLibrary) - unloads cleanly
```

Injection needs the same integrity level as the target. An editor started
elevated needs an elevated MCP host, and the failure is reported as
`Access is denied`, not silently.

### Wire protocol

Deliberately not JSON: one request is tab-separated ASCII, and the response is a
4-byte little-endian length followed by the body. No parser in the agent, no
allocator churn per field.

```
request : COMMAND\targ1\targ2\n
response: <u32 len><body>
```

`body` is either the payload or `ERR <text>`.

| command | args | returns |
|---|---|---|
| `PING` | — | `pid`, `arch`, `tid`, `exe`, `agent` |
| `MODULES` | — | `base size name` per line |
| `READ` | `addr_hex size` | hex bytes |
| `WRITE` | `addr_hex hexbytes` | bytes written |
| `SCAN` | `module\|*` `pattern` | matching addresses, one per line |
| `KEY` / `HOTKEY` / `TYPE` | `vk` / `vk1 vk2 ...` / `text` | synthesise input *from inside* the target |
| `FOCUS` | — | raise the process's main window |
| `CLIPGET` / `CLIPSET` | — / `text` | clipboard round-trip |
| `WINDOWS` | — | `hwnd pid class title` |
| `UNLOAD` | — | frees the DLL |

`SCAN` takes a hex pattern with `??` wildcards, e.g. `48 8B ?? ?? E8`. A scan is
the honest first step for an unknown build: locate the structure, then `READ` it,
and only then decide whether a `WRITE` is safe. `KEY`/`HOTKEY`/`TYPE` matter more
than they look: they synthesise input **from inside the target process**, which is
the one way to drive an editor that refuses input from an external process.

```python
agent.modules(pid)                  # 109 modules in VoiSona
agent.scan(pid, "4D 5A")            # 20 PE headers
agent.read_mem(pid, 0x7FF6_0000_0000, 64)
```

### Building it

```powershell
pwsh -File agents/native/build.ps1
```

The script resolves a compiler in this order: `$env:ZIG`, `zig` on `PATH`,
`python -m ziglang`, a cached `%TEMP%\zig`, and finally a download. zig is used
because it is a single self-contained toolchain — no MSVC, no Windows SDK, no
`vcvarsall.bat`, which matters on a machine that has none of them.

### Honest limit

The agent gives **access**, not **understanding**. `READ`/`WRITE`/`SCAN` are
generic; a note table or a pitch curve still has to be identified per build. What
it removes is the "impossible".

---

## 3. Synthesizer V — the resident Lua agent

Synthesizer V is the only editor with a sanctioned in-process API, so it gets the
richest channel — and the only one where a **whole song** moves in one call.

```python
from lhai.live import synthv_live
synthv_live.install()     # writes LHai_Agent.lua into every generation present
synthv_live.status()      # installed? command file? result file?
```

The script folder is **not** reliably `%APPDATA%`, and no single rule finds it.
Dreamtonics 1.x keeps its data under Documents (which Windows may redirect - here
it is `D:\文档`), 2.x under `%APPDATA%`, and the `DataLocation` in the registry can
be a leftover from an install the editor has since left behind. `synthv_live.install()`
weighs every candidate and takes the one the editor is writing to, so read the
paths it returns rather than assuming one.

Then, in the editor: **Scripts → LHai Agent**. The script never blocks — it polls
with `SV:setTimeout(tick, 250)`, so the piano roll keeps painting. To update the
agent once it is running, use **Scripts → Stop all running scripts** and start it
again; see the caveat about pre-existing copies in the live-control notes.

### Protocol (v3)

```
commands: <ipc>\cmd.txt    one verb per line, then  RESULT <token>
results : <ipc>\res.txt    one  R <verb> ok|err <payload>  per command, then  RESULT <token>
```

The token is what makes the client reliable: `synthv_live.send` records the file
offset before writing and only accepts a `RESULT <token>` that appears *after*
it, so a stale result can never be mistaken for this batch's answer.

### Verbs

| verb | effect |
|---|---|
| `GROUP n` / `TRACKSEL n` / `TRACKS` | note group and track selection |
| `CLEAR` / `NOTE onset dur pitch lyric` / `LYRIC` / `DEL` | note edits |
| `PHONEME index text` | phoneme override |
| `PITCH index semitone` | pitch offset |
| `TIMESHIFT` | shift the group in time |
| `STATE` | dump the group as `N index onset dur pitch lyric` |
| `SEEK tick` / `PLAY` / `STOP` | transport |
| `SAVE` / `UNDO` / `REDO` | project and stack |
| `PROBE` | which optional APIs this build actually exposes |

Every optional call is wrapped in `pcall`, so an unsupported verb comes back as
`err setPhonemes unsupported: ...` instead of throwing inside the poll loop and
killing the agent. A batch is wrapped in `project:newUndoBlock` when available, so
a pushed song is **one Ctrl+Z away** from being gone.

```python
synthv_live.push_notes(ipc, notes)   # GROUP/CLEAR/NOTE.../STATE round-trip
```

---

## 4. UI Automation and keys

For CeVIO, VOCALOID6 and VoiSona, UIA carries the song surface.

```python
from lhai.live import uia, win32
uia.available()                          # (True, 'comtypes')
uia.dump_tree(hwnd, depth=6)             # what is actually there
el = uia.find(hwnd, automation_id="pitButton")
uia.invoke(el); uia.state_of(el)         # press, then read back
uia.set_text(el, "…")                    # Value pattern, no focus needed
```

`state_of` answers "is this the active mode?" through whichever pattern the
widget has — `Toggle` for a WPF radio button, `SelectionItem` for a list item —
which is what makes a click verifiable.

### Focus is the whole ballgame

`SetForegroundWindow` is **refused** when the calling process does not own the
foreground — the normal case for an MCP server that was not just clicked. A
refused call returns success anyway, and every keystroke afterwards goes to
whatever *is* focused. That failure mode is silent and looks like "the tool does
nothing".

`win32.focus` therefore attaches to the target's thread **and** to the current
foreground window's thread, retries, and — if the lock still holds — taps ALT
first, because a real input event releases the foreground lock. It returns
whether it actually reached the foreground, so every caller can check instead of
hoping.

### Dialogs

CeVIO's bulk lyric entry is `F8` (or `Ctrl+P` for lyrics with phonemes), and it
opens a dialog that has to be found and filled. `song_lyrics` does the round
trip: shortcut → wait for a second window owned by the editor → set the text →
Enter → confirm the dialog closed. It returns `dialog_closed` so a caller can tell
an accepted entry from a dismissed one.

---

## 5. Reading the score back

On three of the four engines the score is not a control, so the honest read is
the editor's own save: `song_snapshot` sends `Ctrl+S` (or `Ctrl+Shift+S` to a path
you give it) and parses the file with the reader we already ship. Nothing is
transcribed by hand and nothing is guessed.

If the project has never been saved, `Ctrl+S` opens a dialog instead of writing a
file — so a caller that did not pass `path` is told exactly that, rather than
being handed a dialog nobody is looking at.

---

## 6. CeVIO talk (COM) — a footnote

CeVIO's *talk* half does have a documented automation surface:

```
CeVIO.Talk.RemoteService2.ServiceControl2V40   HostVersion InterfaceVersion IsHostStarted StartHost CloseHost
CeVIO.Talk.RemoteService2.Talker2V40            Volume Speed Tone ToneScale Alpha Cast AvailableCasts
                                               Components Speak GetTextDuration GetPhonemes OutputWaveToFile
```

Read with `comtypes`, because `AvailableCasts` is a custom `IStringArray2`
(`Length` / `At(i)`) that the late-bound path cannot enumerate. `Speak` returns an
`ISpeakingState2` with `Wait()` and `Wait_2(timeout)`, so synthesis can be
synchronous; there is no `IsSpeaking` property, and **no pitch control at all** —
pitch is a song concept, which is the whole reason this document is not about
talk.

`AvailableCasts.Length == 0` is a real state, not an error: it means no *talk*
voicebank is installed (song-only voicebanks never appear here). Every
talk-dependent tool fails fast with that message.

---

## 7. What was actually verified

Run on the machine this was developed on (Python 3.10.11, Windows, zig 0.17.1):

| check | result |
|---|---|
| DLL build | 483 KB, `lhai/live/native/lhai_agent.dll` |
| agent on `notepad.exe` | inject → `PING` → `MODULES` (61) → `READ` (`MZ`) → `SCAN` (`4D 5A`, 7 hits) → `CLIPGET` → `UNLOAD` |
| agent on VoiSona | launch → inject → `PING` → `MODULES` (109) → `SCAN` (20 hits) → eject → close |
| CeVIO UIA tree | `mainMenu` with a real **Song** menu (`Enter Lyrics Collectively (F8)`, `Enter Lyrics with Phoneme (Ctrl+P)`, quantize, measures) plus the transport, tool palette, six parameter lanes and the tempo/beat/quantize readouts |
| `song_transport('cevio','play')` | `ok`, and the state read back as `active: true` — then `stop`, likewise |
| `song_modes('cevio')` | read back the live toolbar: pen active, `normal` lane, `stop` active |
| `song_tool` / `song_lane` without a song track | reported `control is disabled` with the reason, not a silent no-op |
| `win32.focus` | was silently failing; now attaches to both threads, retries, ALT-taps, and returns whether it actually reached the foreground |
| SynthV Lua | installed to the appdata `scripts` folder; `SV:setTimeout` confirmed available |
| tests | `tests/test_live.py` — 40 passing |

```powershell
python -m pytest tests/test_live.py -q
```

**Not yet verified end to end**, and named so nobody assumes otherwise: a full
note-level round trip on VOCALOID6 and VoiSona (`learn` then act), and CeVIO's
F8 bulk lyric entry against a project that has a song track — the machine used
for development had an empty CeVIO project, so the tools and lanes were
correctly reported *disabled* and the dialog path could not be exercised.

---

## 8. Limits, stated plainly

* **The score is a canvas on three engines.** Notes arrive by opening a file or
  by drawing; there is no API to set one note's pitch. `song_caps` says so per
  engine. CeVIO AI's song model lives in `CeVIO.Song.dll`, a .NET assembly, and
  no song `RemoteService` is registered on this machine — only the Talk one.
* **The agent is access, not a model.** `SCAN`/`READ`/`WRITE` are generic on
  purpose; per-build structure discovery is still work.
* **UI Automation is a contract with the vendor's widget names.** A renamed
  button breaks a selector; `song_learn` is the tool that re-measures it.
* **Integrity levels.** An elevated editor cannot be driven from a non-elevated
  host.
* **SynthV's Lua script must be started once per session** (Scripts → LHai
  Agent). Everything else is automatic.
* **Talk is not song.** CeVIO's COM surface has no pitch, because talk has no
  pitch. Do not reach for it when the goal is a vocal.
