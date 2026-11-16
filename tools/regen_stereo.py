"""Regenerate the stereo shader data from the game's shader archive (needed after a game update changes the shaders).

    python -I tools/regen_stereo.py --game "<Fallout 4 install dir>" [--work work/stereo-regen] [--install]

Chain (all stdlib Python, Windows; dump.py disassembles through d3dcompiler_47.dll, present on Windows 10/11):
  1. analysis/tools/shaders/ba2.py   "<game>/Data/Fallout4 - Shaders.ba2" -> <work>/ShadersFX/Shaders011.fxp
  2. analysis/tools/shaders/dump.py  Shaders011.fxp -> <work>/index.json + <work>/disasm/
  3. analysis/tools/shaders/vs_pos.py <work>         -> <work>/vs_pos.json
  4. proto/stereo/tools/gen_stereo_vs.py / gen_stereo_ps.py / gen_stereo_cs.py / gen_stereo_tbl.py -> <work>/out/
     (StereoVS.pack, StereoPS.pack, StereoCS.pack, StereoShaders.tbl) and gen_targets.py -> <work>/out/StereoTargets.inl
Then compares <work>/out with the committed src/plugin/stereo_shaders/* and src/plugin/StereoTargets.inl.
--install copies changed outputs over the committed files (the old ones are renamed to <name>.old-<stamp>).
The .hlsl/.hlsli files in stereo_shaders are hand-written, not generated.
"""
import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SH = os.path.join(REPO, "analysis", "tools", "shaders")
ST = os.path.join(REPO, "proto", "stereo", "tools")
COMMITTED = os.path.join(REPO, "src", "plugin", "stereo_shaders")


def run(*args):
    print(">", " ".join(os.path.relpath(a, REPO) if os.path.isabs(a) and a.startswith(REPO) else a for a in args),
          flush=True)
    subprocess.run([sys.executable, "-I"] + list(args), check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="Fallout 4 install dir (has Data/Fallout4 - Shaders.ba2)")
    ap.add_argument("--work", default=os.path.join(REPO, "work", "stereo-regen"))
    ap.add_argument("--install", action="store_true")
    a = ap.parse_args()
    ba2 = os.path.join(a.game, "Data", "Fallout4 - Shaders.ba2")
    if not os.path.isfile(ba2):
        raise SystemExit(f"not found: {ba2}")
    work = os.path.abspath(a.work)
    fxp_dir = os.path.join(work, "ShadersFX")
    fxp = os.path.join(fxp_dir, "Shaders011.fxp")
    out = os.path.join(work, "out")
    os.makedirs(out, exist_ok=True)
    if not os.path.isfile(fxp):
        ext = os.path.join(work, "ba2")
        os.makedirs(ext, exist_ok=True)
        if os.listdir(ext):
            raise SystemExit(f"{ext} is not empty and has no Shaders011.fxp; use a fresh --work dir")
        run(os.path.join(SH, "ba2.py"), ba2, ext)
        found = [os.path.join(dp, f) for dp, _, fs in os.walk(ext) for f in fs if f.lower() == "shaders011.fxp"]
        if not found:
            raise SystemExit("Shaders011.fxp not in the archive")
        os.makedirs(fxp_dir, exist_ok=True)
        shutil.copy2(found[0], fxp)
    if not os.path.isfile(os.path.join(work, "index.json")):
        run(os.path.join(SH, "dump.py"), fxp, work)
    if not os.path.isfile(os.path.join(work, "vs_pos.json")):
        run(os.path.join(SH, "vs_pos.py"), work)
    pos = os.path.join(work, "vs_pos.json")
    run(os.path.join(ST, "gen_stereo_vs.py"), fxp, pos, os.path.join(out, "StereoVS.pack"))
    run(os.path.join(ST, "gen_stereo_ps.py"), fxp, os.path.join(out, "StereoPS.pack"))
    run(os.path.join(ST, "gen_stereo_cs.py"), fxp, os.path.join(out, "StereoCS.pack"))
    run(os.path.join(ST, "gen_stereo_tbl.py"), fxp, os.path.join(out, "StereoShaders.tbl"))
    run(os.path.join(ST, "gen_targets.py"), fxp, pos, os.path.join(out, "StereoTargets.inl"))

    pairs = [(os.path.join(out, n), os.path.join(COMMITTED, n))
             for n in ("StereoVS.pack", "StereoPS.pack", "StereoCS.pack", "StereoShaders.tbl")]
    pairs.append((os.path.join(out, "StereoTargets.inl"), os.path.join(REPO, "src", "plugin", "StereoTargets.inl")))
    def body(p):   # StereoTargets.inl: the two '//' header lines were edited by hand; compare the table only
        with open(p, encoding="utf-8") as f:
            return [ln for ln in f.read().splitlines() if not ln.startswith("//")]

    changed = []
    for new, old in pairs:
        if old.endswith(".inl"):
            same = os.path.isfile(old) and body(new) == body(old)
        else:
            same = os.path.isfile(old) and filecmp.cmp(new, old, shallow=False)
        print(f"{'same   ' if same else 'CHANGED'} {os.path.relpath(old, REPO)}")
        if not same:
            changed.append((new, old))
    if changed and a.install:
        stamp = time.strftime("%Y%m%d-%H%M")
        for new, old in changed:
            if os.path.isfile(old):
                os.replace(old, f"{old}.old-{stamp}")
            shutil.copy2(new, old)
            print("installed", os.path.relpath(old, REPO))
    return 1 if changed and not a.install else 0


if __name__ == "__main__":
    sys.exit(main())
