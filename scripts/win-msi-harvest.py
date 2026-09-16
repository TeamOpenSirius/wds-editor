#!/usr/bin/env python3
"""Normalize wixl-heat output so the first install actually drops the exe.

wixl-heat wraps root files in <Directory Name=".">. That extra directory
plus regex "hoist + merge" has produced MSIs whose payload lands outside
INSTALLDIR (shortcut says Program Files, Repair later copies files there).

This script:
  1) Hoists every Name="." directory into its parent (real XML, not regex).
  2) Forces Win64="yes" on every Component.
  3) Pulls wds_editor.exe + private runtime DLLs into one stable-GUID
     component directly under INSTALLDIR (exe is KeyPath).
"""

from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

WIX = "http://schemas.microsoft.com/wix/2006/wi"
ET.register_namespace("", WIX)

PAYLOAD_GUID = "8F3A2C1D-4B5E-4A67-9C8D-1E2F3A4B5C6D"
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
            "Guid": PAYLOAD_GUID,
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

    tree.write(path, encoding="utf-8", xml_declaration=True)

    text = path.read_text(encoding="utf-8")
    if "WdsEditorPayload" not in text:
        raise SystemExit("WdsEditorPayload missing after harvest rewrite")
    for name in REQUIRED:
        if name not in text:
            raise SystemExit(f"{name} missing from heat WXS after rewrite")
    print(
        "MSI harvest: hoisted Name='.'; Win64=yes; "
        "pinned wds_editor.exe + runtime DLLs as WdsEditorPayload under INSTALLDIR"
    )


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <heat.wxs>")
    rewrite(Path(sys.argv[1]))


if __name__ == "__main__":
    main()
