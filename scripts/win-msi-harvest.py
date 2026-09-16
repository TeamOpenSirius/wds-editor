#!/usr/bin/env python3
"""Normalize wixl-heat output so first install and dest rebuilds both work.

wixl-heat wraps root files in <Directory Name=".">. That extra directory
plus regex "hoist + merge" has produced MSIs whose payload lands outside
INSTALLDIR (shortcut says Program Files, Repair later copies files there).

This script:
  1) Hoists every Name="." directory into its parent (real XML, not regex).
  2) Forces Win64="yes" on every Component.
  3) Pulls wds_editor.exe + private runtime DLLs into one component
     directly under INSTALLDIR (exe is KeyPath).
  4) Replaces Guid="*" with a UUID5 from the install-relative path. wixl's
     Guid="*" hashes heat Component Ids, which change every package; late
     RemoveExistingProducts then deletes the files the new product just
     copied. Path-stable GUIDs keep refcounts correct across overlays.
"""

from __future__ import annotations

import sys
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path

WIX = "http://schemas.microsoft.com/wix/2006/wi"
ET.register_namespace("", WIX)

REQUIRED = (
    "wds_editor.exe",
    "bass.dll",
    "bassmix.dll",
    "vulkan-1.dll",
    "libstdc++-6.dll",
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll",
)
OPTIONAL = ("libssp-0.dll",)
# Same value as the MSI UpgradeCode; only used as a UUID5 namespace.
GUID_NS = uuid.UUID("a7e3c2b1-9f4d-4e8a-9c6b-1d2e3f4a5b6c")
AUTHORED_GUIDS = {"RegistryInstallDir", "ShortcutPrefs"}


def q(tag: str) -> str:
    return f"{{{WIX}}}{tag}"


def local_name(tag: str) -> str:
    return tag.split("}", 1)[-1]


def file_basename(source: str | None) -> str | None:
    if not source:
        return None
    return source.replace("\\", "/").rsplit("/", 1)[-1]


def hoist_dot_directories(parent: ET.Element) -> None:
    children = list(parent)
    for child in children:
        hoist_dot_directories(child)
        if local_name(child.tag) != "Directory" or child.get("Name") != ".":
            continue
        idx = list(parent).index(child)
        parent.remove(child)
        for grandchild in list(child):
            child.remove(grandchild)
            parent.insert(idx, grandchild)
            idx += 1


def force_win64(root: ET.Element) -> None:
    for comp in root.iter(q("Component")):
        comp.set("Win64", "yes")


def relative_source(source: str | None) -> str:
    if not source:
        return ""
    s = source.replace("\\", "/")
    marker = "$(var.SourceDir)/"
    if marker in s:
        s = s.split(marker, 1)[1]
    elif s.startswith("$(var.SourceDir)"):
        s = s[len("$(var.SourceDir)") :].lstrip("/")
    return s.lower()


def stable_guid(relpath: str) -> str:
    return str(uuid.uuid5(GUID_NS, relpath)).upper()


def source_of(comp: ET.Element) -> str | None:
    for child in comp:
        if local_name(child.tag) == "File":
            return file_basename(child.get("Source"))
    return None


def rewrite(path: Path) -> None:
    tree = ET.parse(path)
    root = tree.getroot()
    hoist_dot_directories(root)
    force_win64(root)

    wanted = set(REQUIRED) | set(OPTIONAL)
    found: dict[str, str] = {}
    drop_ids: list[str] = []
    parent_of: dict[ET.Element, ET.Element] = {}
    for cand in root.iter():
        for child in cand:
            parent_of[child] = cand

    for comp in list(root.iter(q("Component"))):
        name = source_of(comp)
        if name not in wanted:
            continue
        fil = next(c for c in comp if local_name(c.tag) == "File")
        src = fil.get("Source")
        if not src:
            raise SystemExit(f"heat component for {name} has no Source")
        found[name] = src
        cid = comp.get("Id")
        if cid:
            drop_ids.append(cid)
        parent = parent_of.get(comp)
        if parent is None:
            raise SystemExit(f"cannot detach heat component for {name}")
        parent.remove(comp)

    missing = [n for n in REQUIRED if n not in found]
    if missing:
        raise SystemExit(f"heat WXS missing payload file(s): {', '.join(missing)}")

    dirref = None
    for cand in root.iter(q("DirectoryRef")):
        if cand.get("Id") == "INSTALLDIR":
            dirref = cand
            break
    if dirref is None:
        raise SystemExit("heat WXS missing DirectoryRef INSTALLDIR")

    payload = ET.Element(
        q("Component"),
        {
            "Id": "WdsEditorPayload",
            "Guid": stable_guid("payload/wds_editor.exe"),
            "Win64": "yes",
        },
    )
    order = [n for n in REQUIRED] + [n for n in OPTIONAL if n in found]
    for i, name in enumerate(order):
        fid = "WdsFile_" + name.replace(".", "_").replace("+", "x").replace("-", "_")
        attrs = {"Id": fid, "Source": found[name]}
        if i == 0:
            attrs["KeyPath"] = "yes"
        payload.append(ET.Element(q("File"), attrs))
    dirref.insert(0, payload)
    if list(dirref)[0] is not payload:
        raise SystemExit("WdsEditorPayload must be a direct child of DirectoryRef INSTALLDIR")

    group = None
    for cand in root.iter(q("ComponentGroup")):
        if cand.get("Id") == "ProductFiles":
            group = cand
            break
    if group is None:
        raise SystemExit("heat WXS missing ComponentGroup ProductFiles")

    drop = set(drop_ids)
    for ref in list(group):
        if local_name(ref.tag) == "ComponentRef" and ref.get("Id") in drop:
            group.remove(ref)
    group.insert(0, ET.Element(q("ComponentRef"), {"Id": "WdsEditorPayload"}))

    leftover_dot = [
        d
        for d in root.iter(q("Directory"))
        if d.get("Name") == "."
    ]
    if leftover_dot:
        raise SystemExit("Directory Name='.' still present after hoist")

    for comp in root.iter(q("Component")):
        cid = comp.get("Id") or ""
        if cid in AUTHORED_GUIDS:
            continue
        if cid == "WdsEditorPayload":
            continue
        src = None
        for child in comp:
            if local_name(child.tag) == "File":
                src = child.get("Source")
                break
        rel = relative_source(src)
        if not rel:
            raise SystemExit(f"cannot assign stable GUID to component {cid!r}")
        guid = stable_guid(rel)
        if guid.startswith("{") or "*" in guid:
            raise SystemExit(f"invalid stable GUID for {cid}: {guid}")
        comp.set("Guid", guid)

    star_guids = [c.get("Id") for c in root.iter(q("Component")) if c.get("Guid") == "*"]
    if star_guids:
        raise SystemExit(f"Guid='*' still present after harvest: {', '.join(star_guids)}")

    tree.write(path, encoding="utf-8", xml_declaration=True)

    text = path.read_text(encoding="utf-8")
    if "WdsEditorPayload" not in text:
        raise SystemExit("WdsEditorPayload missing after harvest rewrite")
    for name in REQUIRED:
        if name not in text:
            raise SystemExit(f"{name} missing from heat WXS after rewrite")
    print(
        "MSI harvest: hoisted Name='.'; Win64=yes; "
        "pinned payload + path-stable component GUIDs under INSTALLDIR"
    )


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <heat.wxs>")
    rewrite(Path(sys.argv[1]))


if __name__ == "__main__":
    main()
