# `lhai.mcp` — the singing-engine MCP server

One MCP server that writes and reads **Synthesizer V**, **VOCALOID**, **CeVIO AI**
and **VoiSona** project files from a single song description, and can push a
track straight into a running Synthesizer V.

It is the bridge between "I have a melody and a lyric" and "the editor is open
with my notes in it". Every capability below is stated with its actual fidelity,
because a writer that silently loses a note is worse than one that refuses.

```
LM / agent  ──MCP──▶  lhai.mcp  ──▶  .svp / .vsqx / .ccs / .mid
                          │
                          └──Lua bridge──▶ Synthesizer V (live, notes only)
```

## Install

```powershell
python -m pip install -e ".[mcp]"      # only the transport needs the SDK
lhai-mcp --help                        # stdio (default) / http / sse
```

Without the `mcp` extra the four writers still import: `lhai.mcp.ir`,
`lhai.mcp.engines`, `lhai.mcp.midiio` are standard library only.

Client config (opencode / Claude Desktop / anything stdio) — a copy lives in
[docs/mcp-opencode.json](mcp-opencode.json):

```json
{
  "mcp": {
    "lh-ai-studio": {
      "type": "local",
      "command": ["lhai-mcp"],
      "enabled": true
    }
  }
}
```

```powershell
python -m lhai.mcp.server          # works without installing the console script
python -m lhai.mcp.server http     # streamable HTTP, MCP_PORT (default 8000)
```

## Workspace

`$LH_MCP_HOME` or `%LOCALAPPDATA%\lh-ai-studio\singing`:

```
projects/<slug>.json      the editable project (the IR, not any engine's format)
exports/                  every file `export` wrote
ipc/                      SynthV bridge command/result files
```

Projects are plain JSON so an agent can diff them; nothing is lost when you
round-trip through an export.

## The IR

`lhai.mcp.ir.Song` / `Track` / `SNote`:

| field | meaning |
|---|---|
| `onset`, `duration` | **beats** (quarter notes) from the song start, float |
| `pitch` | MIDI note number (0-127) |
| `lyric` | one syllable per note |
| `phonemes` | optional phoneme string; empty = let the engine's G2P decide |
| `language` | optional per-note override (mixed-lingual lines) |

Beats, not ticks: each adapter multiplies by its own clock at the boundary
(SynthV 705 600 000 blick/quarter, VOCALOID 480 tick/quarter via 120 BPM
centi-tempo, CeVIO 960 clock/quarter, MIDI 480 PPQ). One IR note is one sung
note; a melisma spanning two notes has two notes.

`split_lyrics()` decides how one lyric line becomes syllables:

1. `-` and `|` are explicit separators — never overridden;
2. whitespace separates words; pure CJK is one syllable per character;
3. a Latin word stays whole (the engine's G2P finds its vowels better than a
   regex does); `mode="syllables"` opts into a vowel-group split;
4. an unspaced Latin string with no separators at all is vowel-split anyway
   (`kanade` → `ka na de`), or it would eat the whole melody.

Leftover syllables and lyric-less notes are reported, never dropped silently.

## Tools

26 tools, all returning `{"ok": true|false, ...}`.

**Discovery**

| tool | does |
|---|---|
| `engines()` | which of the four are installed, their exe, running state, supported formats |
| `voices(engine, folder="")` | installed voice names (`synthv`, `vocaloid`, `cevio`) |

**Projects**

| tool | does |
|---|---|
| `project_create(name, bpm, beats_per_bar, beat_unit, language)` | new project + first track |
| `project_list()` / `project_show(project, track=None, preview=True)` | list / inspect + ASCII piano roll |
| `project_import(path, name="")` | read `.svp` / `.vsqx` / `.ccs` / `.mid` into the IR |
| `project_delete(project)` | remove from the workspace |

**Tracks**

| tool | does |
|---|---|
| `track_add(project, name, voice, language)` | one singer per track |
| `track_set(project, track, voice, language, gain_db, pan, muted, solo)` | any field |
| `track_remove(project, track)` | drop a track |

**Notes**

| tool | does |
|---|---|
| `notes_add(project, track, notes)` | append `[{onset, duration, pitch, lyric, ...}]` |
| `notes_list(project, track, offset, limit)` | page through notes in onset order |
| `note_update(project, track, index, ...)` | change one note by index |
| `note_delete(project, track, indexes)` | delete by index (drop-safe) |
| `notes_clear(project, track)` | empty a track, keep the singer |
| `lyrics_fill(project, track, text, start_index, mode)` | assign syllables to existing notes |
| `transpose(project, track, semitones, start_index)` | shift pitch |
| `quantize(project, track, grid, durations)` | snap onsets/durations to a grid |
| `shift(project, track, beats, start_index)` | move notes in time |
| `song_validate(project)` | report overlaps, empty lyrics, out-of-range pitches |

**Export**

| tool | does |
|---|---|
| `export(project, engines=None, filename="", voice_id="")` | write each engine's native file |
| `export_midi(project, filename="")` | Standard MIDI File — the one format all four import |
| `open_in(project, engine, fmt="")` | export for that engine and launch it on the file |

**Synthesizer V live**

| tool | does |
|---|---|
| `synthv_bridge_install()` | write `LHai_Agent.lua` into each generation's `scripts/` folder |
| `synthv_status()` | installed? current? is SynthV running? |
| `synthv_push(project, track, clear=True, timeout=15)` | replace the selected group's notes, live |

## Per-engine fidelity — the honest table

| engine | file | written | read | live channel |
|---|---|---|---|---|
| Synthesizer V Studio 2 | `.svp` (v153 JSON) | **yes**, field-for-field with an editor-written file | 113 / 153 / 187 | **yes** — resident Lua script, notes into the selected group |
| VOCALOID6 | `.vsqx` (`vsq4.xsd`) | **yes**, schema order, `preMeasure=0`, 16-char component id | yes | no public API; launch + import |
| CeVIO AI | `.ccs` (XML) | **yes**, 960 clock/quarter, pre-measure 3840 | yes | no public API; launch + import |
| VoiSona | — | **no writer** (it imports) | via `.svp`/`.ccs`/`.mid` | no public API |

VoiSona's own import list (read out of `VoiSona.exe`) is `.svp`, `.ccs`, `.mid`,
so `open_in("voisona")` hands it a Synthesizer V file rather than inventing a
fourth format. `export` therefore produces three real files plus the shared MIDI,
and says so in its return value instead of pretending otherwise.

`export(voice_id=...)` is VOCALOID only: a 16-character component id. Omit it
and the editor falls back to its default singer, which is what you want unless
you have already pinned a voice.

Voice discovery, again honestly: CeVIO's list is parsed from its own package
registry (`Configuration/VocalSource/Song/*/setting.cfg`), so you get the names
the editor shows — `IA`, `KAFU`, `SEKAI`, `RIME` — not the folder codes
(`CSNV-JPF-FP5`). VOCALOID names come from the installed voicebank folders under
`CommonFiles/Model`. Synthesizer V database blobs are encrypted, so names are
read from real `.svp` files in the recent-files list (`mainRef.database.name`);
the `databases/meta` catalog is used only as a fallback and is tagged
`"source": "catalog"`.

## The Synthesizer V bridge

SynthV is the only one of the four that can be driven while it is open, via its
own Lua script host.

1. `synthv_bridge_install()` — writes the agent into **every generation present**.
   The paths are not guessable: 2.x keeps its data under
   `%APPDATA%\Dreamtonics\Synthesizer V Studio 2\scripts`, while 1.x keeps it under
   *Documents* — and Documents is redirected to `D:\` on this machine, so the live
   folder is `D:\文档\Dreamtonics\Synthesizer V Studio\scripts`. Neither the
   `%APPDATA%` folder of the same name nor the `DataLocation` the installer recorded
   is what 1.x uses here; both exist and both are ignored. The call weighs every
   candidate and installs into the one the editor is actually writing to — read the
   paths it returns; do not assume one. The IPC path is baked into the generated
   source, because SynthV's Lua cannot read environment variables.
2. In Synthesizer V: **Scripts → LHai Agent**. It stays resident. The agent
   installs a stamp of its own id in `ipc/synthv_agent.txt` at start and checks it
   each turn, so a second run leaves the newcomer serving instead of doubling it.
   Measured caveat: the host has never written the `# agent retired` line, so it is
   evidently stopping the previous instance itself — and a copy from a build older
   than the stamp file neither retires nor gets stopped, so **Scripts → Stop all
   running scripts** is the reliable way to update an agent that is already
   running.
3. Select the group you want filled.
4. `synthv_push(project="My Song", track=0)` — clears the group (unless
   `clear=False`) and writes the notes; returns `pushed`, `notes`, `group`.

Protocol, if you need to debug it: `ipc/synthv_cmd.txt` carries line commands
(`GROUP n`, `CLEAR`, `NOTE <blick> <blick> <pitch> <lyric> <lang>`, `END <token>`)
and the agent appends `RESULT <token> ...` to `ipc/synthv_result.txt`;
`ipc/synthv_agent.txt` holds the running instance's stamp and is internal.
`synthv_status()` tells you whether the script is missing, stale (written for a
different IPC directory) or never answered.

## Limits, stated plainly

- **No piano-roll automation.** Pitch bends, vibrato, dynamics, note
  expressions and breathiness are not written; only notes, lyrics, language,
  tempo, time signature and track voice. The files are valid and the notes are
  exact, but a rendered vocal is flat until you shape it in the editor.
- **One syllable per note.** A tied note is two notes with the same lyric, not a
  tie; all four formats represent ties differently and none of them needs it for
  a first pass.
- **`open_in` launches, it does not synchronise.** After the import the editor
  owns the file; `synthv_push` is the only bidirectional channel.
- **SVP is a versioned JSON format**, not a documented one. The writer targets
  v153 because that is what the current editor writes; the reader accepts 113,
  153 and 187.
