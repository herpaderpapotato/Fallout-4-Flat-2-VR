"""Install the controller stand-in models (FloatingGun.h, [Weapon] bControllerModels) into the fo4flatvr MO2 mod folder
(%LOCALAPPDATA%/ModOrganizer/Fallout 4/mods/fo4flatvr = the game's Data in MO2's VFS; never the base Data folder).

FO4VR draws the SteamVR render model of each controller and falls back to Data/Meshes/Vive_Wand.nif (VR 140C61D80).
This copies FO4VR's own Vive_Wand.nif (from the user's Fallout 4 VR install, 'Fallout4_VR - Main.ba2') as one file per
controller (the model DB caches one node per path) plus its two textures ('Fallout4_VR - Textures.ba2', DX10 -> DDS):

    Data/Meshes/fo4flatvr/ControllerR.nif, Data/Meshes/fo4flatvr/ControllerL.nif
    Data/Textures/vive_dif.dds, Data/Textures/vive_spec.dds   (paths as referenced by the NIF)

    python -I install_controller_models.py [--force]

Refuses while Fallout4 runs. Existing files are kept unless --force (then renamed to <name>.old-<stamp>, never deleted).
"""
import os
import struct
import subprocess
import sys
import time
import zlib

VR_DATA = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 4 VR\Data"
GAME_DATA = os.path.expandvars(r"%LOCALAPPDATA%\ModOrganizer\Fallout 4\mods\fo4flatvr")  # MO2 mod folder (since 15 Nov 21:16)


def names_at(f, n, nameoff):
    f.seek(nameoff)
    data = f.read()
    o, out = 0, []
    for _ in range(n):
        ln, = struct.unpack_from("<H", data, o)
        out.append(data[o + 2:o + 2 + ln].decode("latin1").lower())
        o += 2 + ln
    return out


def gnrl(path, want):
    with open(path, "rb") as f:
        _, _, typ, n, nameoff = struct.unpack("<4sI4sIQ", f.read(24))
        assert typ == b"GNRL", typ
        recs = [struct.unpack("<I4sIIQIII", f.read(36)) for _ in range(n)]
        out = {}
        for nm, r in zip(names_at(f, n, nameoff), recs):
            if nm in want:
                off, packed, unpacked = r[4], r[5], r[6]
                f.seek(off)
                b = f.read(packed or unpacked)
                out[nm] = zlib.decompress(b) if packed else b
        return out


FOURCC = {71: b"DXT1", 72: b"DXT1", 74: b"DXT3", 77: b"DXT5", 83: b"ATI2"}
BLOCK = {71: 8, 72: 8, 74: 16, 77: 16, 80: 8, 83: 16, 98: 16, 99: 16}


def dx10(path, want):
    with open(path, "rb") as f:
        _, _, typ, n, nameoff = struct.unpack("<4sI4sIQ", f.read(24))
        assert typ == b"DX10", typ
        recs = []
        for _ in range(n):
            h = struct.unpack("<I4sIBBHHHBBH", f.read(24))
            recs.append((h, [struct.unpack("<QIIHHI", f.read(24)) for _ in range(h[4])]))
        out = {}
        for nm, (h, chunks) in zip(names_at(f, n, nameoff), recs):
            if nm not in want:
                continue
            height, width, mips, fmt = h[6], h[7], h[8], h[9]
            body = b""
            for off, packed, unpacked, _, _, _ in chunks:
                f.seek(off)
                b = f.read(packed or unpacked)
                body += zlib.decompress(b) if packed else b
            pitch = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * BLOCK.get(fmt, 16)
            if fmt in FOURCC:
                pf, ext = struct.pack("<II4s5I", 32, 0x4, FOURCC[fmt], 0, 0, 0, 0, 0), b""
            else:
                pf, ext = struct.pack("<II4s5I", 32, 0x4, b"DX10", 0, 0, 0, 0, 0), struct.pack("<5I", fmt, 3, 0, 1, 0)
            hdr = (b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, height, width, pitch, 0, mips) + b"\0" * 44 + pf +
                   struct.pack("<5I", 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0) + ext)
            out[nm] = hdr + body
        return out


def put(rel, data, force):
    dst = os.path.join(GAME_DATA, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        if open(dst, "rb").read() == data:
            print("unchanged", rel)
            return
        if not force:
            print("exists (keep; --force to replace)", rel)
            return
        os.rename(dst, dst + time.strftime(".old-%Y%m%d-%H%M"))
    open(dst, "wb").write(data)
    print("installed", rel, len(data))


def main():
    force = "--force" in sys.argv
    if "fallout4.exe" in subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower():
        raise SystemExit("Fallout4 is running - not installing")
    mesh = gnrl(os.path.join(VR_DATA, "Fallout4_VR - Main.ba2"), {r"meshes\vive_wand.nif"})[r"meshes\vive_wand.nif"]
    tex = dx10(os.path.join(VR_DATA, "Fallout4_VR - Textures.ba2"), {r"textures\vive_dif.dds", r"textures\vive_spec.dds"})
    put(r"Meshes\fo4flatvr\ControllerR.nif", mesh, force)
    put(r"Meshes\fo4flatvr\ControllerL.nif", mesh, force)
    for k, v in tex.items():
        put(k.replace("textures", "Textures", 1), v, force)


if __name__ == "__main__":
    main()
