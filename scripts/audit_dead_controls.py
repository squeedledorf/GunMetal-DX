#!/usr/bin/env python3
"""Audit preference controls for settings nothing under Vulkan reads (#423).

For every control bound to a setting (control_name=) in the preference panels, the
Quick Prefs and performance floaters and the Lightbox, find the C++ that reads the
setting and classify it:

  live      read by code that runs under Vulkan (anything not listed as GL-only or
            as bookkeeping below)
  gl-only   read only in the legacy GL render path (GL_ONLY below), even after
            following LLPipeline's settings-mirroring members into VK code
  ui-only   only preference / Lightbox UI code, presets, the feature table manager
            or other bookkeeping mention it (writers, lists, greying)
  none      no C++ mention at all
  unknown   the control_name is not a declared setting (a typo, or a setting that
            was removed): the control writes into a setting created on the fly

A setting is also reported as "code-handled" when a C++ file names the WIDGET
(getChild("name")): such controls are often driven by hand (e.g. the Indirect*
sliders), so the classification is only a starting point for those. Every reader
is printed as file:line so a "live" verdict can be checked by eye.

Usage: python3 scripts/audit_dead_controls.py [--all] [--json]
  --all   also list the live settings (default: only the suspicious classes)
  --json  machine-readable output
Run from anywhere; paths resolve from this file.
"""

import json
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
NEWVIEW = os.path.join(ROOT, "indra", "newview")
XUI = os.path.join(NEWVIEW, "skins", "default", "xui", "en")

# Panels audited (#423): preferences, Quick Prefs, performance floater, Lightbox.
PANEL_RE = re.compile(r"^(panel_preferences_.*|floater_preferences.*|floater_quick_prefs|"
                      r"floater_performance|panel_performance_.*|panel_lightbox_.*|"
                      r"floater_lightbox_settings)\.xml$")

# Files only the legacy GL render path runs (Vulkan never calls into them for
# rendering). Matched against the path relative to indra/.
GL_ONLY = [
    r"^newview/pipeline\.cpp$",
    r"^newview/llviewershadermgr\.cpp$",
    r"^newview/lldrawpool[^/]*\.cpp$",
    r"^newview/llreflectionmap[^/]*\.cpp$",
    r"^newview/llheroprobemanager\.cpp$",
    r"^newview/llsettingsvo\.cpp$",      # GL shader-uniform setup (VK builds its own)
    r"^newview/llvowlsky\.cpp$",
    r"^newview/llvosky\.cpp$",
    r"^newview/llglsandbox\.cpp$",
    r"^newview/llgltfmaterialpreviewmgr\.cpp$",
]
# Files whose mention of a setting is bookkeeping, not behaviour: preference and
# Lightbox UI, presets, feature tables, the importer, settings plumbing.
UI_ONLY = [
    r"^newview/llfloaterpreference[^/]*\.cpp$",
    r"^newview/llpanelpreference[^/]*\.cpp$",
    r"^newview/llfloaterperformance\.cpp$",
    r"^newview/alfloaterlightbox[^/]*\.cpp$",
    r"^newview/alpanellightbox[^/]*\.cpp$",
    r"^newview/alfloaterquickprefs\.cpp$",
    r"^newview/llfloaterquickprefs\.cpp$",
    r"^newview/llpresetsmanager\.cpp$",
    r"^newview/llfeaturemanager\.cpp$",
    r"^newview/slviewerimport[^/]*\.cpp$",
]
# Code that runs the Vulkan frame (for the settings-mirroring check).
VK_LIVE = [r"^newview/vkgfx/", r"^newview/llclean", r"^newview/llappviewer\.cpp$", r"^vkclean/"]

WRITER_RE = r"\.\s*set(?:BOOL|S32|U32|F32|String|Value|Untyped|LLSD|Color4|Color3|Vector3|Vector3d|Rect)?\s*\(\s*\"{name}\""


def classify_file(rel):
    for pats, cls in ((GL_ONLY, "gl"), (UI_ONLY, "ui")):
        if any(re.search(p, rel) for p in pats):
            return cls
    return "live"


def load_settings():
    names = set()
    for fn in ("settings.xml", "settings_per_account.xml"):
        path = os.path.join(NEWVIEW, "app_settings", fn)
        if not os.path.isfile(path):
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        # Top-level keys of the settings map: "<key>Name</key>" followed by "<map>".
        names.update(re.findall(r"<key>([A-Za-z0-9_]+)</key>\s*<map>", text))
    return names


def scan_xui():
    """setting -> [(file, widget name, attribute)]; also the widget names per setting."""
    bound = defaultdict(list)
    ui_refs = defaultdict(list)   # enabled_control / disabled_control / visibility_control users
    for fn in sorted(os.listdir(XUI)):
        if not PANEL_RE.match(fn):
            continue
        text = open(os.path.join(XUI, fn), encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"<([A-Za-z_.]+)\b([^<>]*?)/?>", text, re.S):
            attrs = dict(re.findall(r"([A-Za-z_]+)\s*=\s*\"([^\"]*)\"", m.group(2)))
            widget = attrs.get("name", "?")
            for key in ("control_name", "ctrl_name"):
                if attrs.get(key):
                    bound[attrs[key]].append((fn, widget))
            # (LLUICtrl also accepts LL's misspelt visiblity_control / invisiblity_control.)
            for key in ("enabled_control", "disabled_control", "visibility_control", "invisibility_control",
                        "visiblity_control", "invisiblity_control"):
                if attrs.get(key):
                    ui_refs[attrs[key]].append((fn, widget, key))
    return bound, ui_refs


def index_sources():
    """All C++ string literals and identifiers: literal -> [(rel, line, text)]."""
    literals = defaultdict(list)
    idents = defaultdict(list)
    lit_re = re.compile(r"\"([A-Za-z_][A-Za-z0-9_]*)\"")
    id_re = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\b")
    indra = os.path.join(ROOT, "indra")
    for base, dirs, files in os.walk(indra):
        dirs[:] = [d for d in dirs if d not in ("skins", "app_settings", "tests", "test")]
        for fn in files:
            if not fn.endswith((".cpp", ".h", ".mm", ".inl")):
                continue
            path = os.path.join(base, fn)
            rel = os.path.relpath(path, indra).replace(os.sep, "/")
            vk = any(re.search(p, rel) for p in VK_LIVE)
            with open(path, encoding="utf-8", errors="replace") as f:
                for i, line in enumerate(f, 1):
                    if '"' in line:
                        for name in lit_re.findall(line):
                            literals[name].append((rel, i, line.strip()))
                    if vk:
                        for name in set(id_re.findall(line)):
                            idents[name].append((rel, i, line.strip()))
    return literals, idents


def audit():
    settings = load_settings()
    bound, ui_refs = scan_xui()
    literals, idents = index_sources()
    rows = []
    for name in sorted(bound):
        widgets = bound[name]
        hits = literals.get(name, [])
        readers = {"live": [], "gl": [], "ui": []}
        writers = []
        wre = re.compile(WRITER_RE.format(name=re.escape(name)))
        for rel, line, text in hits:
            if wre.search(text):
                writers.append((rel, line))
                continue
            readers[classify_file(rel)].append((rel, line))
        # LLPipeline mirrors many settings into same-named statics (RenderFoo =
        # gSavedSettings...("RenderFoo")) that VK code may read: follow them.
        mirrored = []
        if not readers["live"] and readers["gl"]:
            mirrored = [(rel, line) for rel, line, text in idents.get(name, [])
                        if '"' + name + '"' not in text]
        # Widgets the code drives by hand.
        code_widgets = sorted({w for _, w in widgets if w != "?" and literals.get(w)})
        if name not in settings:
            cls = "unknown"
        elif readers["live"] or mirrored:
            cls = "live"
        elif readers["gl"]:
            cls = "gl-only"
        elif readers["ui"] or writers:
            cls = "ui-only"
        else:
            cls = "none"
        rows.append({
            "setting": name,
            "class": cls,
            "controls": ["%s:%s" % w for w in widgets],
            "code_handled_widgets": code_widgets,
            "xui_dependents": ["%s:%s(%s)" % r for r in ui_refs.get(name, [])],
            "live": ["%s:%d" % r for r in readers["live"]],
            "mirrored": ["%s:%d" % r for r in mirrored],
            "gl": ["%s:%d" % r for r in readers["gl"]],
            "ui": ["%s:%d" % r for r in readers["ui"]],
            "writers": ["%s:%d" % r for r in writers],
        })
    return rows


def main():
    rows = audit()
    if "--json" in sys.argv:
        json.dump(rows, sys.stdout, indent=1)
        print()
        return
    show_all = "--all" in sys.argv
    order = ["unknown", "none", "ui-only", "gl-only", "live"]
    counts = defaultdict(int)
    for r in rows:
        counts[r["class"]] += 1
    for cls in order:
        if cls == "live" and not show_all:
            continue
        sel = [r for r in rows if r["class"] == cls]
        if not sel:
            continue
        print("==== %s (%d) ====" % (cls, len(sel)))
        for r in sel:
            flag = "  [code-handled: %s]" % ", ".join(r["code_handled_widgets"]) if r["code_handled_widgets"] else ""
            print("%s%s" % (r["setting"], flag))
            print("    controls: %s" % ", ".join(r["controls"]))
            for key in ("live", "mirrored", "gl", "ui", "writers", "xui_dependents"):
                if r[key]:
                    more = " ..." if len(r[key]) > 6 else ""
                    print("    %-9s %s%s" % (key + ":", ", ".join(r[key][:6]), more))
        print()
    print("summary: " + ", ".join("%s %d" % (c, counts[c]) for c in order))


if __name__ == "__main__":
    main()
