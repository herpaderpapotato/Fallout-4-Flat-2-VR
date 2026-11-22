"""Generate fo4flatvr's MCM menu and the in-VR config panel's row table from plugin/tools/config_spec.py.

Writes
  - plugin/dist/Data/MCM/Config/fo4flatvr/config.json and settings.ini (MCM's defaults; every ModSetting must be listed
    there, also those without an MCM control, so the VR panel can set them through MCM.SetModSetting*);
  - plugin/src/plugin/ConfigSpec.gen.inl (the C++ table ConfigUI draws; analysis/vr-config-ui.md).
The gesture key dropdown's names come from Gestures.cpp (between "MCM-KEYS-BEGIN" / "MCM-KEYS-END"), so the stored index
always matches the plugin's table. Run: python -I plugin/tools/gen_mcm_config.py

MCM stores changed values in Data/MCM/Settings/fo4flatvr.ini (under MO2: overwrite/MCM/Settings); the plugin layers that
file over fo4flatvr.ini (ConfigStore.h) and re-reads it within a second of a change.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import config_spec as spec  # noqa: E402

PLUGIN = os.path.dirname(HERE)
SRC = os.path.join(PLUGIN, "src", "plugin", "Gestures.cpp")
OUT = os.path.join(PLUGIN, "dist", "Data", "MCM", "Config", "fo4flatvr")
INL = os.path.join(PLUGIN, "src", "plugin", "ConfigSpec.gen.inl")


def key_names():
    text = open(SRC, encoding="utf-8").read()
    m = re.search(r"MCM-KEYS-BEGIN(.*?)MCM-KEYS-END", text, re.S)
    if not m:
        raise SystemExit("key table markers not found in Gestures.cpp")
    names = []
    for s in re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*0x[0-9A-Fa-f]+|\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*0\s*\}', m.group(1)):
        n = s[0] or s[1]
        names.append(n.encode().decode("unicode_escape"))
    if len(names) < 50 or names[0] != "(none)":
        raise SystemExit(f"key table parse failed ({len(names)} names)")
    return names


def note(text):
    return {"text": text, "type": "text"}


def section(text):
    return {"text": text, "type": "section"}


def mcm_control(r, keys):
    id_ = f"{r['key']}:{r['section']}"
    if r["type"] == "bool":
        return {"id": id_, "text": r["label"], "type": "switcher", "help": r["help"], "valueOptions": {"sourceType": "ModSettingBool"}}
    if r["type"] == "enum":
        opts = keys if r["options"] == ["@keys"] else r["options"]
        return {"id": id_, "text": r["label"], "type": "dropdown", "help": r["help"],
                "valueOptions": {"sourceType": "ModSettingInt", "options": opts}}
    if r["type"] in ("float", "int"):
        return {"id": id_, "text": r["label"], "type": "slider", "help": r["help"],
                "valueOptions": {"min": r["min"], "max": r["max"], "step": r["step"],
                                 "sourceType": "ModSettingFloat" if r["type"] == "float" else "ModSettingInt"}}
    return None


def ini_value(r):
    t, d = r["type"], r["default"]
    if t == "float":
        return f"{float(d):.4g}" if abs(float(d)) < 1e4 else f"{float(d)}"
    if t == "policy":
        return str(d)
    return str(int(d))


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def main():
    keys = key_names()
    rows = spec.rows()
    # ---- MCM pages: the gesture pages as before + one page per panel page (Quick is the panel's shortcut page) ----
    pages = []
    MCM_NOTE = ("These values are also in the VR settings panel (hold the left menu button, or the gesture). MCM shows its own "
                "default until a value is changed here or in the panel; the panel shows the value in effect "
                "(fo4flatvr.ini, then these MCM changes).")
    for page in spec.PAGES:
        if page == "Quick":
            continue
        prow = [r for r in rows if r["page"] == page]
        content = [note(MCM_NOTE)] if page not in ("Gestures", "Selfie") else []
        group = None
        hidden = 0
        for r in prow:
            ctl = mcm_control(r, keys) if r["mcm"] else None
            if not ctl:
                hidden += 1
                continue
            if r["group"] != group:
                group = r["group"]
                content.append(section(group))
            if not r["apply"] and "Restart:" not in ctl["help"]:
                ctl["help"] = (ctl["help"] + " " if ctl["help"] else "") + "(after a restart)"
            content.append(ctl)
        if hidden:
            content.append(note(f"{hidden} more settings of this page (camera views, keyboard keys) are only in the VR panel."))
        pages.append({"pageDisplayName": {"Height": "Height / roomscale", "Keys": "Keys / panel"}.get(page, page), "content": content})
    cfg = {
        "modName": "fo4flatvr",
        "displayName": "VR (fo4flatvr)",
        "minMcmVersion": 2,
        "content": [note("fo4flatvr: flat Fallout 4 in VR. Live settings apply within a second; the others after a restart. "
                         "The same settings are in the VR panel: hold the left menu button for 1 s, Ctrl+F2, or the "
                         "right stick Up-then-down gesture.")],
        "pages": pages,
    }
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "config.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(cfg, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    # ---- settings.ini: every key once, in section order of first appearance ----
    sections, seen = {}, set()
    for r in rows:
        k = (r["section"], r["key"])
        if k in seen:
            continue
        seen.add(k)
        sections.setdefault(r["section"], []).append(f"{r['key']}={ini_value(r)}")
    with open(os.path.join(OUT, "settings.ini"), "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write("; MCM defaults for fo4flatvr (generated by plugin/tools/gen_mcm_config.py from config_spec.py - edit that, not this).\n")
        fh.write("; Your changes are stored by MCM in Data\\MCM\\Settings\\fo4flatvr.ini.\n")
        for s, lines in sections.items():
            fh.write(f"[{s}]\n" + "\n".join(lines) + "\n\n")
    # ---- C++ row table ----
    tmap = {"bool": "kBool", "int": "kInt", "float": "kFloat", "enum": "kEnum", "policy": "kPolicy", "vk": "kVk"}
    with open(INL, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("// Generated by plugin/tools/gen_mcm_config.py from plugin/tools/config_spec.py - edit those, not this.\n")
        fh.write("// { page, group, section, key, type, default, min, max, step, options ('|'), label, help, apply, mcm }\n")
        for r in rows:
            opts = "|".join(r["options"])
            fh.write("{ " + ", ".join([c_str(r["page"]), c_str(r["group"]), c_str(r["section"]), c_str(r["key"]),
                                       "RowType::" + tmap[r["type"]], c_str(ini_value(r)), f"{float(r['min'])}f", f"{float(r['max'])}f",
                                       f"{float(r['step'])}f", c_str(opts), c_str(r["label"]), c_str(r["help"]), c_str(r["apply"]),
                                       "true" if r["mcm"] else "false"]) + " },\n")
    with open(INL.replace("ConfigSpec", "ConfigKeys"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("// Generated by plugin/tools/gen_mcm_config.py from Gestures.cpp's key table (index = MCM dropdown index).\n")
        for k in keys:
            fh.write(c_str(k) + ",\n")
    print(f"wrote {OUT} and {INL}: {len(rows)} rows, {len(seen)} settings, {len(pages)} MCM pages, {len(keys)} keys")


if __name__ == "__main__":
    sys.exit(main())
