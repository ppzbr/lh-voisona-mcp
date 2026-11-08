"""Install or remove the native bridge in a running VoiSona / VOCALOID 6.

    python bridges/native/install.py voisona
    python bridges/native/install.py vocaloid
    python bridges/native/install.py voisona --eject

The install path is the same one the agent uses: ``LoadLibraryW`` on the DLL
from inside the target, via the classic OpenProcess / VirtualAllocEx /
WriteProcessMemory / CreateRemoteThread sequence in :mod:`lhai.live.inject`.
Nothing is patched and no file of the editor is modified - the DLL is mapped in
and the editor keeps running.

Removal needs the DLL's own unload export, because a thread cannot free the
module it is running in: the export schedules the unload on a fresh thread and
this waits for the module to leave the list.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lhai.live import apps, bridges, inject  # noqa: E402


def _report(engine: str) -> dict:
    entry = bridges.for_engine(engine)
    if entry is None:
        return {"ok": False, "error": "the bridge is not answering yet"}
    status = bridges.status(entry["endpoint"])
    tools = bridges.tools(entry["endpoint"])
    return {
        "ok": True,
        "endpoint": entry["endpoint"],
        "tool_count": len(tools),
        "tools": [t.get("name", "") for t in tools],
        "app": status.get("app", {}),
        "compatibility": status.get("compatibility", {}),
    }


def install(engine: str) -> int:
    live = apps.find(engine)
    if not live.running:
        print(f"{live.label} is not running - start it first")
        return 2

    dll = bridges.native_bridge_dll()
    if not dll.exists():
        print(f"{dll} is missing - build it: powershell -File bridges/native/build.ps1")
        return 2

    print(f"target  : {live.label} pid={live.pid}")
    print(f"bridge  : {dll}")
    print(f"sha256  : {hashlib.sha256(dll.read_bytes()).hexdigest()}")

    if inject.is_loaded(live.pid, dll.name):
        print("already loaded; eject first if you rebuilt it")
    else:
        result = inject.inject(live.pid, dll=dll, wait=10.0)
        if not result.ok:
            print(f"injection failed: {result.error}")
            return 1
        print(f"loaded  : hmodule={result.hmodule:#x}")

    # The bridge announces itself by binding a port; give it a moment.
    deadline = time.time() + 10
    entry = None
    while time.time() < deadline:
        entry = bridges.for_engine(engine)
        if entry:
            break
        time.sleep(0.5)
    if not entry:
        print("the DLL loaded but no endpoint appeared - check the editor's folder for lhai_bridge.boot.txt")
        return 1

    report = _report(engine)
    print(f"endpoint: {report['endpoint']}")
    print(f"engine  : {report['app'].get('exe')} {report['app'].get('version')}")
    print(f"tools   : {report['tool_count']}")
    for name in report["tools"]:
        print(f"   {name}")
    compatibility = report["compatibility"]
    print(f"compat  : ok={compatibility.get('ok')} level={compatibility.get('level')}")
    for reason in compatibility.get("reasons", []):
        print(f"   reason: {reason}")
    return 0 if compatibility.get("ok") else 1


def eject(engine: str) -> int:
    live = apps.find(engine)
    if not live.running:
        print(f"{live.label} is not running")
        return 2
    dll = bridges.native_bridge_dll()
    result = inject.unload_module(live.pid, dll.name, image=dll, export=bridges.EJECT_EXPORT)
    print(result)
    return 0 if result.get("ok") else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="native bridge installer")
    parser.add_argument("engine", choices=list(apps.ENGINE_KEYS), help="which editor")
    parser.add_argument("--eject", action="store_true", help="unload instead of load")
    args = parser.parse_args(argv)
    return eject(args.engine) if args.eject else install(args.engine)


if __name__ == "__main__":
    raise SystemExit(main())
