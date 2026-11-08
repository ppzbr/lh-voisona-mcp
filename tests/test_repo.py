"""This repository must be self-describing and internally consistent.

Three kinds of failure this file exists to catch, all of which have happened in
the project these repositories came from:

* a shipped binary that does not match the build record beside it, so a user
  installs something nobody built - the manifest and the build record are the
  two statements of provenance, and they must agree;
* a manifest that has drifted from the working tree, so a hand-edited file ships
  under a stale hash;
* a test run that exercises a `lhai` installed somewhere else, which reports
  green for code this repository does not contain.

It also asserts that the paths `prompt.md` tells an agent to run are present: a
prompt that names a file which is not here is worse than no prompt.
"""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
MANIFEST = REPO / "MANIFEST.txt"

# --------------------------------------------------------------------------- #
# what this repository ships, and how to prove it
# --------------------------------------------------------------------------- #
# (binary, the build record that documents it, the key holding its sha256)
PROVENANCE = [
    ("bridges/native/lhai_bridge.dll", "bridges/native/lhai_bridge.build-info.txt", "DllSha256"),
    ("bridges/native/lhai_bridge.exe", "bridges/native/lhai_bridge.build-info.txt", "ExeSha256"),
]

# (build record, key, expected value) - properties of the build that must hold
FACTS = [
    ("bridges/native/lhai_bridge.build-info.txt", "TargetFramework", "native-x64"),
]

# The files prompt.md tells the agent to run.  The regex test below covers every
# backticked path in the prompt; this list is the ones that must never go away.
PROMPT_PATHS = [
    "bridges/native/build.ps1",
    "bridges/native/install.py",
    "bridges/native/lhai_bridge.cpp",
    "bridges/native/lhai_bridge.dll",
    "bridges/native/lhai_bridge.exe",
    "lhai/live/inject.py",
    "lhai/mcp/server.py",
]

# Every repository path in prompt.md, in either separator style and wherever it
# appears - prose or code block.  Requiring an extension keeps bare mentions of the
# directories (`lhai/`, `docs/`) and non-paths (`tools/list`) out of the check.
PROMPT_PATH_RE = re.compile(
    r"(?:bridges|lhai|tests|docs|tools|VocalPatch|xstudio)[\\/][\w./\\-]*\.\w+"
)

SKIP_DIRS = {
    ".git",
    "__pycache__",
    ".pytest_cache",
    ".venv",
    "venv",
    "build",
    ".mypy_cache",
    ".ruff_cache",
}
# `pip install -e .` writes these into the project root; they are build output.
SKIP_ENDINGS = (".egg-info", ".dist-info")
SKIP_SUFFIXES = (".pyc", ".pyo", ".lib", ".exp", ".pdb")
# Diagnostics a run drops beside the binaries.  Names this repository never
# produces are harmless here; the list is shared across the family on purpose.
SKIP_NAMES = {
    "MANIFEST.txt",
    "LhCeVioBridge.boot.txt",
    "LhCeVioBridge.status.json",
    "LhCeVioLoader.boot.txt",
    "LhCeVioLoader.target.txt",
    "lhai_bridge.boot.txt",
    "lhai_bridge.status.json",
}


# --------------------------------------------------------------------------- #
# reading the tree
# --------------------------------------------------------------------------- #
def _hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _tracked() -> set[str]:
    out = set()
    for path in REPO.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(REPO)
        if any(part in SKIP_DIRS or part.endswith(SKIP_ENDINGS) for part in rel.parts):
            continue
        if path.name.endswith(SKIP_SUFFIXES) or path.name in SKIP_NAMES:
            continue
        out.add(rel.as_posix())
    return out


def _manifest() -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        match = re.match(r"^([0-9a-f]{64})\s+(\S+)$", line)
        if match:
            entries[match.group(2)] = match.group(1)
    return entries


def _record(relative: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in (REPO / relative).read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            out[key.strip()] = value.strip()
    return out


# --------------------------------------------------------------------------- #
# the manifest is the record of what is here
# --------------------------------------------------------------------------- #
def test_the_manifest_lists_every_file_and_only_files_that_exist() -> None:
    listed = set(_manifest())
    present = _tracked()
    assert present - listed == set(), "in the repository but not in MANIFEST.txt"
    assert listed - present == set(), "listed in MANIFEST.txt but not in the repository"


def test_every_manifest_hash_matches_its_file() -> None:
    wrong = [name for name, want in _manifest().items() if _hash(REPO / name) != want]
    assert wrong == [], "MANIFEST.txt disagrees with the files - run tools/make-manifest.py"


def test_the_manifest_does_not_list_itself() -> None:
    assert "MANIFEST.txt" not in _manifest()


# --------------------------------------------------------------------------- #
# provenance: the shipped binaries say what they are
# --------------------------------------------------------------------------- #
def test_every_shipped_binary_is_the_one_its_build_record_describes() -> None:
    if not PROVENANCE:
        pytest.skip("this repository ships no prebuilt binaries")
    for binary, record, key in PROVENANCE:
        want = _record(record)[key].upper()
        got = _hash(REPO / binary).upper()
        assert got == want, f"{binary} is not the binary {record} recorded"


def test_every_recorded_build_property_still_holds() -> None:
    if not FACTS:
        pytest.skip("this repository records no build properties")
    for record, key, want in FACTS:
        assert _record(record).get(key) == want, f"{record}: {key} is not {want}"


# --------------------------------------------------------------------------- #
# the tree is the one being tested
# --------------------------------------------------------------------------- #
def test_the_client_under_test_is_this_repository() -> None:
    """A `lhai` imported from somewhere else would make every other test a
    statement about a different tree."""
    package = REPO / "lhai" / "__init__.py"
    if not package.is_file():
        pytest.skip("this repository ships no Python package")
    import lhai

    assert Path(lhai.__file__).resolve() == package.resolve()


# --------------------------------------------------------------------------- #
# the prompt describes files that exist
# --------------------------------------------------------------------------- #
def test_the_files_the_prompt_names_are_present() -> None:
    if not PROMPT_PATHS:
        pytest.skip("this repository has no file checklist")
    missing = [name for name in PROMPT_PATHS if not (REPO / name).is_file()]
    assert missing == [], "prompt.md names files that are not here"


def test_every_path_prompt_md_mentions_exists() -> None:
    """The prompt is pasted into an agent that has never seen this repository, so
    every path it names has to be a path in it."""
    text = (REPO / "prompt.md").read_text(encoding="utf-8")
    missing = []
    for match in PROMPT_PATH_RE.finditer(text):
        candidate = match.group(0).replace("\\", "/").rstrip(".,;:)")
        if not (REPO / candidate).exists():
            missing.append(candidate)
    assert missing == [], f"prompt.md names paths that are not here: {sorted(set(missing))}"
