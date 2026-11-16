"""Edit the [Stereo] section of a fo4flatvr.ini in place, per line (other agents edit other lines/sections).

    python -I tools/stereo_ini.py <ini> [--default-on | --default-off]

Adds the lighting-round keys (with their comment lines) if missing, sets iScope=1, and with --default-on sets
iMode=3 + bInstallHooks=1 (--default-off: iMode=0, bInstallHooks=0). Only the [Stereo] lines named here change;
the file is written to a temp file and swapped in with os.replace (never truncated), line endings kept.
"""
import os
import re
import sys

NEW_KEYS = [  # key, value, comment lines (inserted before the key when the key is missing)
    ("bSplitDraws", "1", ["; Lighting round: shaders that read per-eye rows of the per-frame buffer (deferred lights, composite, water,",
                          "; decals, screen-space passes) are drawn once per half with a per-eye copy of b12 and a scissor."]),
    ("bSaoPerEye", "1", ["; SAO's RawAO pass split per half with a per-eye projInfo (its cb2[0])."]),
    ("bSaoPixelShader", "1", ["; Stereo frames: SAO through its pixel-shader variant (splittable) instead of the compute variant."]),
    ("bTaaPerEye", "1", ["; Per-eye TAA: the GBuffer motion vectors are converted per eye right before the TAA resolve (history taps",
                         "; clamped to the eye's half) and each eye gets the engine's TAA jitter. 0 = mono motion vectors, no eye jitter."]),
    ("bCutTaa", "1", ["; Cut list, each applied only while frames are rendered in stereo (mono keeps the effect):",
                      "; TAA off (FXAA on): its motion vectors are centre-eye."]),
    ("bCutMotionBlur", "1", ["; Motion blur off (mono motion vectors; comfort)."]),
    ("bCutDof", "1", ["; Depth of field off (mono focus ray, kernels cross the seam)."]),
    ("bCutLensFlare", "1", ["; Lens flare off (sprites at mono screen positions)."]),
    ("bCutGodrays", "1", ["; NVIDIA godrays off (closed library, mono camera)."]),
    ("bCutSsr", "1", ["; Screen-space reflections off (mono rays over the double-wide image)."]),
    ("bCutHbao", "1", ["; HBAO+ off (closed library; SAO stays)."]),
    ("bCutTiledLighting", "1", ["; Tiled (compute) deferred lighting off: lights take the per-volume path, which is split per eye."]),
]


def edit(path, mode=None):
    with open(path, encoding="utf-8", newline="") as fh:
        text = fh.read()
    if not text.strip():
        raise SystemExit(f"{path} is empty - not touching it")
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    try:
        s = next(i for i, l in enumerate(lines) if l.strip() == "[Stereo]")
    except StopIteration:
        raise SystemExit(f"{path}: no [Stereo] section")
    e = next((i for i in range(s + 1, len(lines)) if lines[i].startswith("[")), len(lines))
    while e > s + 1 and not lines[e - 1].strip():
        e -= 1  # insert before trailing blank lines of the section

    def find(key):
        return next((i for i in range(s + 1, e) if re.match(rf"{re.escape(key)}=", lines[i])), None)

    def set_key(key, value, comments=None):
        nonlocal e
        i = find(key)
        if i is not None:
            lines[i] = f"{key}={value}"
        else:
            block = (comments or []) + [f"{key}={value}"]
            lines[e:e] = block
            e += len(block)

    set_key("iScope", "1", ["; 0 = convert GBuffer writers + z-prepass; 1 = every world/1st-person camera draw whose VS has a twin."])
    for k, v, c in NEW_KEYS:
        if find(k) is None:
            set_key(k, v, c)
    if mode == "on":
        set_key("iMode", "3")
        set_key("bInstallHooks", "1")
    elif mode == "off":
        set_key("iMode", "0")
        set_key("bInstallHooks", "0")
    new = nl.join(lines)
    if new == text:
        print(f"{path}: unchanged")
        return
    tmp = path + ".tmp-stereo_ini"
    with open(tmp, "w", encoding="utf-8", newline="") as fh:
        fh.write(new)
    os.replace(tmp, path)
    print(f"{path}: [Stereo] updated (mode={mode})")


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(2)
    edit(a[0], "on" if "--default-on" in a else "off" if "--default-off" in a else None)
