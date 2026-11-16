"""Test harness for unattended in-game runs. Short timeouts, polling instead of fixed sleeps, a
progress line every few seconds, and the game window kept behind the user's windows.

    from fo4test import Game
    with Game(runtime_json=None) as g:        # launches (unless already running -> refuses)
        g.coc("SanctuaryExt")                  # waits until in-world (sessions use `console load vrtest_sanctuary`)
        print(g.cmd("status"))
    # on exit: qqq, then kill after a short grace period (only the game this harness launched)
    # Game(attach=True) uses an already-running game's pipe instead of refusing (never quits it).
    # Game(frik=True) also requires the FRIK-flat MO2 mod to be enabled.

Mod Organizer 2 (since 15 Nov 21:16): fo4flatvr and FRIK-flat are MO2 mods of the global instance "Fallout 4",
profile TestProfile; they are NOT in the base game's Data folder any more.
  - deploy = copy into the MO2 mod folders (PLUG / FRIK_PLUG below) with deploy_files() / deploy_plugin() /
    deploy_frik(); the previous file goes to work/deploy-backup/ first. MO2's VFS maps the folder at every launch.
  - per-line ini edits (_set_ini_key, _get_ini_key) go to effective_ini(): the copy that wins in the VFS (normally
    the mod folder's fo4flatvr.ini; MO2's overwrite/F4SE/Plugins/fo4flatvr.ini if one exists - warned).
  - launch = `ModOrganizer.exe -i "Fallout 4" -p TestProfile run -e F4SE` (an open MO2 window gets the request
    forwarded and runs it; MO2 is never closed). The OpenXR runtime choice goes through the ini key
    [OpenXR] sRuntimeJson (the plugin sets XR_RUNTIME_JSON itself), so it survives MO2's hand-off.
  - preflight (every launch): fo4flatvr (and FRIK-flat for frik=True) enabled ('+') in the profile's modlist.txt
    (never changed here), no fo4flatvr.dll / FRIK.dll in the base Data/F4SE/Plugins, no other mod or overwrite
    shadowing the DLLs, profile saves contain the vrtest_* saves.

CLI:  python -I fo4test.py smoke [--steamvr | --no-xr] [--load vrtest_sanctuary]   (launch via MO2, report where the
      DLLs were loaded from + the OpenXR runtime lines, load the save, quit)
      python -I fo4test.py preflight [--frik]   (the MO2 checks only, no launch)
"""
import filecmp
import os
import re
import shutil
import subprocess
import sys
import time

PIPE = r"\\.\pipe\fo4flatvr"
GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 4"
BASE_PLUG = os.path.join(GAME, "Data", "F4SE", "Plugins")   # must NOT contain fo4flatvr.dll / FRIK.dll any more
LOG = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\F4SE\fo4flatvr.log")
NO_XR_JSON = r"C:\nonexistent\xr_disabled_for_test.json"
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# -- Mod Organizer 2 ------------------------------------------------------------------------------------------
MO2_EXE = r"C:\Modding\MO2\ModOrganizer.exe"
MO2_INSTANCE = "Fallout 4"                       # global instance
MO2_DIR = os.path.expandvars(r"%LOCALAPPDATA%\ModOrganizer\Fallout 4")
MO2_PROFILE = "TestProfile"   # MO2 "test profile": the ONLY profile used for testing (user, 16 Nov); never Default
MO2_EXECUTABLE = "F4SE"                          # custom executable (f4se_loader.exe) in ModOrganizer.ini
MODS = os.path.join(MO2_DIR, "mods")
OVERWRITE = os.path.join(MO2_DIR, "overwrite")
PROFILE_DIR = os.path.join(MO2_DIR, "profiles", MO2_PROFILE)
MODLIST = os.path.join(PROFILE_DIR, "modlist.txt")
MOD_FO4FLATVR = "fo4flatvr"
MOD_FRIK = "FRIK-flat"
MOD_DIR = os.path.join(MODS, MOD_FO4FLATVR)
FRIK_MOD_DIR = os.path.join(MODS, MOD_FRIK)
PLUG = os.path.join(MOD_DIR, "F4SE", "Plugins")            # = Data/F4SE/Plugins in the VFS (fo4flatvr.dll/.ini, flag files)
FRIK_PLUG = os.path.join(FRIK_MOD_DIR, "F4SE", "Plugins")   # FRIK.dll/.pdb
MOD_INI = os.path.join(PLUG, "fo4flatvr.ini")
INI_REL = r"F4SE\Plugins\fo4flatvr.ini"
DEPLOY_BACKUP = os.path.join(ROOT, "work", "deploy-backup")
BUILD = os.path.join(ROOT, "plugin", "build", "windows", "x64", "releasedbg")
FRIK_BUILD = os.path.join(ROOT, "frik-flat", "build", "windows", "x64", "releasedbg")
DOC_SAVES = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\Saves")
TEST_SAVES = ("vrtest_sanctuary", "vrtest_container")


def say(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def game_pids():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Fallout4.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    return [int(p) for p in re.findall(r'"Fallout4\.exe","(\d+)"', out)]


def game_pid():
    p = game_pids()
    return p[0] if p else None


def pid_alive(pid):
    return bool(pid) and pid in game_pids()


def xmake_running():
    return "xmake" in subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()


def mo2_running():
    return "modorganizer.exe" in subprocess.run(["tasklist", "/FI", "IMAGENAME eq ModOrganizer.exe"],
                                                capture_output=True, text=True).stdout.lower()


# -- MO2 profile / VFS view (read only: the harness never changes the user's profile or mod list) ----------------
def modlist():
    """[(enabled, name)] from the profile's modlist.txt, highest priority first (MO2 writes the top line = highest).
    Separators and unmanaged ('*') entries are skipped."""
    out = []
    with open(MODLIST, encoding="utf-8-sig", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if not line or line[0] not in "+-" or line.endswith("_separator"):
                continue
            out.append((line[0] == "+", line[1:]))
    return out


def mod_enabled(name):
    return any(en and n.lower() == name.lower() for en, n in modlist())


def profile_setting(key, default=None):
    try:
        with open(os.path.join(PROFILE_DIR, "settings.ini"), encoding="utf-8", errors="replace") as fh:
            m = re.search(rf"(?mi)^{re.escape(key)}=([^\r\n]*)", fh.read())
        return m.group(1).strip() if m else default
    except OSError:
        return default


def saves_dir():
    """Where the game reads/writes saves under MO2: the profile's saves folder with LocalSaves=true."""
    return os.path.join(PROFILE_DIR, "saves") if profile_setting("LocalSaves", "false").lower() == "true" else DOC_SAVES


def vfs_sources(rel):
    """Real files that provide Data/<rel> in the VFS, winner first: overwrite, enabled mods (priority order), base Data."""
    out = []
    p = os.path.join(OVERWRITE, rel)
    if os.path.isfile(p):
        out.append(p)
    for en, name in modlist():
        p = os.path.join(MODS, name, rel)
        if en and os.path.isfile(p):
            out.append(p)
    p = os.path.join(GAME, "Data", rel)
    if os.path.isfile(p):
        out.append(p)
    return out


_ini_warned = set()


def effective_ini():
    """The fo4flatvr.ini the game reads through the VFS (per-line edits go there). Normally the mod folder's; if MO2's
    overwrite (or another mod) holds a copy, that one wins: it is returned and a warning printed once."""
    try:
        src = vfs_sources(INI_REL)
    except OSError:
        src = []
    ini = src[0] if src else MOD_INI
    if os.path.normcase(ini) != os.path.normcase(MOD_INI) and ini not in _ini_warned:
        _ini_warned.add(ini)
        print(f"[fo4test] WARNING: the effective fo4flatvr.ini is {ini} (it overrides the mod folder's copy in MO2's VFS); "
              f"ini edits go there. Move it back into the mod folder in MO2 when convenient.", file=sys.stderr, flush=True)
    return ini


INI = effective_ini()   # compatibility for scripts that import INI; functions below re-resolve it each time


def _rel_under(path, folder):
    return os.path.normcase(os.path.abspath(path)).startswith(os.path.normcase(os.path.abspath(folder)) + os.sep)


def check_base_clean():
    """Refuse when the old deploy path is used again: a fo4flatvr.dll/FRIK.dll in the base Data/F4SE/Plugins would be
    loaded next to (or shadow) the MO2 mod's copy."""
    try:
        names = os.listdir(BASE_PLUG)
    except OSError:
        return
    bad = [n for n in names if n.lower() in ("fo4flatvr.dll", "frik.dll")]
    if bad:
        raise SystemExit(f"REFUSING: {', '.join(bad)} found in the base game folder {BASE_PLUG}. Since 15 Nov 21:16 the "
                         f"plugins live only in the MO2 mods ({MOD_DIR}, {FRIK_MOD_DIR}); a base copy would load twice or "
                         f"shadow the mod. Move it out (rename, do not delete) before launching.")


def preflight(frik=False):
    """Checks before any launch through MO2 (raises SystemExit with the reason). Read-only except ensure_test_saves()."""
    for p, what in ((MO2_EXE, "ModOrganizer.exe"), (MODLIST, "MO2 profile modlist.txt"), (MOD_DIR, "fo4flatvr mod folder")):
        if not os.path.exists(p):
            raise SystemExit(f"REFUSING: {what} not found at {p}")
    try:
        with open(os.path.join(MO2_DIR, "ModOrganizer.ini"), encoding="utf-8", errors="replace") as fh:
            m = re.search(r"(?m)^selected_profile=(?:@ByteArray\()?([^)\r\n]*)\)?", fh.read())
        if m and m.group(1) != MO2_PROFILE:
            raise SystemExit(f"REFUSING: MO2's selected profile is '{m.group(1)}', the harness uses '{MO2_PROFILE}' "
                             f"(an open MO2 would refuse the launch). Switch profiles in MO2 yourself.")
    except OSError:
        pass
    need = [MOD_FO4FLATVR] + ([MOD_FRIK] if frik else [])
    off = [n for n in need if not mod_enabled(n)]
    if off:
        raise SystemExit(f"REFUSING: MO2 mod(s) {', '.join(off)} not enabled in profile {MO2_PROFILE} ({MODLIST}). "
                         f"Enable them in MO2 (left pane checkbox); the harness never changes the mod list.")
    check_base_clean()
    dlls = [(r"F4SE\Plugins\fo4flatvr.dll", MOD_DIR)]
    if frik or mod_enabled(MOD_FRIK):
        dlls.append((r"F4SE\Plugins\FRIK.dll", FRIK_MOD_DIR))
    for rel, folder in dlls:
        src = vfs_sources(rel)
        if not src:
            raise SystemExit(f"REFUSING: no {rel} in the VFS (expected in {folder})")
        if not _rel_under(src[0], folder):
            raise SystemExit(f"REFUSING: {rel} in the VFS comes from {src[0]}, not from {folder} (another mod or "
                             f"MO2's overwrite shadows it). Fix that in MO2.")
    effective_ini()   # warns if the overwrite copy wins
    ensure_test_saves()


def ensure_test_saves():
    """With profile-local saves the game only sees PROFILE_DIR/saves: copy the harness's vrtest_* saves (.fos + .f4se)
    there from Documents/My Games/Fallout4/Saves if they are missing. Never overwrites, never removes."""
    dst = saves_dir()
    if os.path.normcase(dst) == os.path.normcase(DOC_SAVES) or not os.path.isdir(DOC_SAVES):
        return
    os.makedirs(dst, exist_ok=True)
    have = {f.lower() for f in os.listdir(dst)}
    for f in sorted(os.listdir(DOC_SAVES)):
        stem, ext = os.path.splitext(f)
        if stem.lower() in TEST_SAVES and ext.lower() in (".fos", ".f4se") and f.lower() not in have:
            shutil.copy2(os.path.join(DOC_SAVES, f), os.path.join(dst, f))
            say(f"test save {f} copied into the MO2 profile saves ({dst})")


# Cross-agent launch lock: several agents share one game install, one log and one pipe. The lock is
# held from the "is a game running?" check until the plugin pipe is up and the ini is restored, so
# two harnesses can never launch in the same window. A lock older than LOCK_STALE_S is taken over.
# Re-entrant within one process (deploy_files() inside a caller's own lock).
LOCK = os.path.join(ROOT, "work", "fo4test.launch.lock")
LOCK_STALE_S = 150
# User headset-session hold: while this file exists, no harness launches or attaches to the game
# (the user is playing on a frozen, suite-checked build). Only the coordinator's own suite run sets
# FO4TEST_IGNORE_HOLD=1 to verify that build before handing it over.
HOLD = os.path.join(ROOT, "work", "fo4test.hold")
_lock_depth = 0


def _check_hold():
    if os.path.exists(HOLD) and os.environ.get("FO4TEST_IGNORE_HOLD") != "1":
        raise SystemExit("HOLD: the user has the game for a headset session (work/fo4test.hold exists) - "
                         "do not launch, attach or deploy; retry later.")


def _check_vremu(runtime_json):
    """A SteamVR launch is only allowed while SteamVR runs the vremu emulator (its HTTP API answers). If it doesn't,
    the user may be using a real headset (Virtual Desktop / normal SteamVR): refuse instead of launching into it."""
    if not runtime_json or "steamvr" not in runtime_json.lower():
        return
    import urllib.request
    try:
        urllib.request.urlopen("http://127.0.0.1:8766/state", timeout=2).read()
    except OSError:
        raise SystemExit("vremu emulator not answering on 127.0.0.1:8766 - SteamVR is not in emulator mode or not running; "
                         "the user may be in the headset. Not launching; retry later (do not change SteamVR's mode).")


def _acquire_launch_lock(timeout=0):
    global _lock_depth
    if _lock_depth > 0:
        _lock_depth += 1
        return True
    t0 = time.time()
    while True:
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, f"{os.getpid()} {time.time():.0f}\n".encode())
            os.close(fd)
            _lock_depth = 1
            return True
        except FileExistsError:
            try:
                age = time.time() - os.path.getmtime(LOCK)
            except OSError:
                continue
            if age > LOCK_STALE_S:
                try:
                    os.replace(LOCK, LOCK + f".stale-{time.strftime('%Y%m%d-%H%M%S')}")
                except OSError:
                    pass
                continue
            if time.time() - t0 >= timeout:
                return False
            time.sleep(1.0)


def _release_launch_lock():
    global _lock_depth
    if _lock_depth > 1:
        _lock_depth -= 1
        return
    _lock_depth = 0
    try:
        os.replace(LOCK, LOCK + ".released")
    except OSError:
        pass


# -- deploy into the MO2 mod folders -------------------------------------------------------------------------
def deploy_files(files, dest_dir=PLUG, tag=""):
    """Copy `files` (source paths, or (source, name) pairs) into dest_dir (an MO2 mod folder, never the base Data).
    The file already there is backed up to work/deploy-backup/<name>.<yyyyMMdd-HHmmss>[-tag] first (skipped when
    identical). Refuses while Fallout4 runs, the hold file exists, an xmake build runs or another harness launches."""
    if not _rel_under(dest_dir, MODS):
        raise SystemExit(f"deploy target {dest_dir} is not an MO2 mod folder - refusing (base Data is off limits)")
    _check_hold()
    if game_pid():
        raise SystemExit("Fallout4 running - not deploying")
    if xmake_running():
        raise SystemExit("an xmake build is running - not deploying")
    if not _acquire_launch_lock(timeout=0):
        raise SystemExit("another harness is launching Fallout4 right now - not deploying")
    try:
        if game_pid():
            raise SystemExit("Fallout4 started meanwhile - not deploying")
        os.makedirs(dest_dir, exist_ok=True)
        os.makedirs(DEPLOY_BACKUP, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S") + (f"-{tag}" if tag else "")
        done = []
        for item in files:
            src, name = item if isinstance(item, tuple) else (item, os.path.basename(item))
            dst = os.path.join(dest_dir, name)
            if os.path.isfile(dst):
                if filecmp.cmp(src, dst, shallow=False):
                    done.append(f"{name} (unchanged)")
                    continue
                shutil.copy2(dst, os.path.join(DEPLOY_BACKUP, f"{name}.{stamp}"))
            tmp = dst + ".tmp-deploy"
            shutil.copy2(src, tmp)
            os.replace(tmp, dst)
            done.append(name)
        say(f"deployed to {dest_dir}: {', '.join(done)} (previous files in work/deploy-backup/*.{stamp})")
    finally:
        _release_launch_lock()


def deploy_plugin(build=BUILD, need=None, tag=""):
    """fo4flatvr.dll + .pdb from the build dir into the fo4flatvr mod. need = bytes the built dll must contain."""
    dll = os.path.join(build, "fo4flatvr.dll")
    if need and need not in open(dll, "rb").read():
        raise SystemExit(f"the built dll has no {need!r} - rebuild first")
    deploy_files([dll, os.path.join(build, "fo4flatvr.pdb")], PLUG, tag)


def deploy_frik(build=FRIK_BUILD, tag=""):
    """FRIK.dll + .pdb into the FRIK-flat mod."""
    deploy_files([os.path.join(build, "FRIK.dll"), os.path.join(build, "FRIK.pdb")], FRIK_PLUG, tag)


# -- ini per-line edits (the effective copy in the VFS) -----------------------------------------------------------
def _set_ini_key(key, value):
    """Change one key's line in the deployed ini, keeping every other line (and the line endings) as
    they are on disk now. Written to a temp file then swapped in, so the ini is never empty/truncated."""
    ini = effective_ini()
    with open(ini, encoding="utf-8", newline="") as fh:
        text = fh.read()
    new, n = re.subn(rf"(?m)^{re.escape(key)}=[^\r\n]*", lambda _: f"{key}={value}", text)
    if n == 0 or new == text:
        return
    tmp = ini + ".tmp-fo4test"
    with open(tmp, "w", encoding="utf-8", newline="") as fh:
        fh.write(new)
    os.replace(tmp, ini)


def _get_ini_key(key):
    with open(effective_ini(), encoding="utf-8", newline="") as fh:
        m = re.search(rf"(?m)^{re.escape(key)}=([^\r\n]*)", fh.read())
    return m.group(1) if m else None


def parse_kv(line):
    """'a=1 b=two c='x y'' -> {'a': '1', 'b': 'two', 'c': 'x y'}. Tolerates non key=value words."""
    return {m.group(1): m.group(3) if m.group(3) is not None else m.group(2)
            for m in re.finditer(r"(\w+)=('([^']*)'|\S*)", line)}


def launch_mo2():
    """Start the F4SE executable through MO2 (VFS applied). With MO2's window open the request is forwarded to it
    (its -i/-p are checked there) and this helper process exits at once; without one, this MO2 process stays until
    the game exits. Returns the Popen of the helper. Never waits for it, never closes MO2."""
    args = [MO2_EXE, "-i", MO2_INSTANCE, "-p", MO2_PROFILE, "run", "-e", MO2_EXECUTABLE]
    flags = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP: survives this script
    return subprocess.Popen(args, cwd=os.path.dirname(MO2_EXE), creationflags=flags, close_fds=True,
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Game:
    def __init__(self, runtime_json=None, no_xr=False, attach=False, quiet=False, frik=False):
        """attach=True: if Fallout4 is already running, use its pipe instead of refusing (it is then
        never quit/killed by this harness). quiet=True: cmd() does not echo commands/replies.
        frik=True: the launch also requires the FRIK-flat MO2 mod to be enabled."""
        self.runtime_json = NO_XR_JSON if no_xr else runtime_json
        self.attach = attach
        self.quiet = quiet
        self.frik = frik
        self.f = None
        self.launched = False
        self.attached = False
        self.pid = None         # Fallout4.exe started by this launch (the only process quit()/kill() may touch)
        self.exit_info = None   # set by quit(): {"exit_s": float|None, "killed": bool}

    # -- lifecycle -------------------------------------------------------------------------
    def __enter__(self):
        _check_hold()
        if not self.attach:
            _check_vremu(self.runtime_json)
            preflight(frik=self.frik)
        if not self.attach and not _acquire_launch_lock(timeout=0):
            raise SystemExit("another harness is launching Fallout4 right now - not launching.")
        locked = not self.attach
        try:
            if game_pid():
                if not self.attach:
                    raise SystemExit("Fallout4 is already running - not launching (someone may be using it).")
                self._wait("plugin pipe (attach)", 10, self._try_open)
                self.attached = True
                return self
            if self.attach:   # nothing to attach to: this becomes a launch, with the launch checks
                _check_vremu(self.runtime_json)
                preflight(frik=self.frik)
                if not _acquire_launch_lock(timeout=0):
                    raise SystemExit("another harness is launching Fallout4 right now - not launching.")
                locked = True
            rt_orig = _get_ini_key("sRuntimeJson")
            if self.runtime_json is not None and rt_orig is None:
                raise SystemExit(f"{effective_ini()} has no sRuntimeJson= line - cannot select the OpenXR runtime; not launching")
            try:
                if self.runtime_json is not None:
                    _set_ini_key("sRuntimeJson", self.runtime_json)
                    say(f"[OpenXR] sRuntimeJson={self.runtime_json} for this launch ({effective_ini()})")
                log_mark = "launch"
                self._launch_time = time.time() - 1
                say(f"launching through MO2: {MO2_EXECUTABLE}, profile {MO2_PROFILE} "
                    f"({'forwarded to the open MO2 window' if mo2_running() else 'MO2 not open: started for this run'})")
                try:
                    self._mo2 = launch_mo2()
                except OSError as e:
                    raise SystemExit(f"MO2 launch failed: {e}")
                self.launched = True
                self._wait("Fallout4.exe (via MO2)", 120, self._find_game)
                self._wait("plugin pipe", 90, self._try_open)
            finally:
                # the plugin reads the ini at load; restore our one line as soon as the pipe is up
                # (or on failure). Other lines are left exactly as they are on disk.
                if self.runtime_json is not None and rt_orig is not None:
                    _set_ini_key("sRuntimeJson", rt_orig)
        except BaseException:
            if self.launched:
                self.quit()   # __exit__ is not called when __enter__ fails: don't leave a launched game behind
            raise
        finally:
            if locked:
                _release_launch_lock()
        try:
            self.cmd("background")
            if self.runtime_json and "steamvr" in self.runtime_json.lower():
                self._vremu_focus()
            self._wait("renderer + XR init", 30, lambda: "GameDataReady" in self._log_since(log_mark) and
                       ("OpenXR up" in self._log_since(log_mark) or "OpenXR init failed" in self._log_since(log_mark)))
        except BaseException:
            # __exit__ is not called when __enter__ fails: don't leave a launched game behind
            self.quit()
            raise
        return self

    def _find_game(self):
        """Fallout4.exe that appeared after the launch (none ran before: checked under the lock)."""
        pids = game_pids()
        if pids:
            self.pid = pids[0]
            say(f"Fallout4.exe pid {self.pid}")
            return True
        rc = self._mo2.poll() if getattr(self, "_mo2", None) else None
        if rc not in (None, 0):
            raise SystemExit(f"ModOrganizer.exe exited with code {rc} before the game started (see MO2's logs in {MO2_DIR}\\logs)")
        return False

    def _vremu_focus(self, timeout=20):
        """SteamVR + vremu: the SteamVR dashboard opens over a new app and holds input focus (XR session
        state 4 = VISIBLE). Press the emulated left system button until the session is FOCUSED (5)."""
        import urllib.request
        t0 = time.time()
        pressed = 0
        while time.time() - t0 < timeout:
            st = parse_kv(self.cmd("status", quiet=True))
            if st.get("state") == "5":
                say(f"XR session focused (system presses: {pressed})")
                return True
            if st.get("state") == "4" and pressed < 3:
                try:
                    urllib.request.urlopen("http://127.0.0.1:8766/press?dev=left&btn=system&ms=150", timeout=2).read()
                    pressed += 1
                except OSError as e:
                    say(f"vremu press failed: {e}")
                time.sleep(2.0)
                continue
            time.sleep(0.5)
        say("XR session did not reach FOCUSED")
        return False

    def __exit__(self, *exc):
        self.quit()

    def quit(self, grace=15):
        """qqq, then kill after `grace` s. Only for the game this harness launched (self.pid). Returns seconds from
        qqq to process exit (None if it had to be killed or was not running)."""
        if not self.launched:
            return None
        pid = self.pid
        self.exit_info = {"exit_s": None, "killed": False}
        if pid and pid_alive(pid):
            try:
                if self.f:
                    self.cmd("console qqq", quiet=True)
            except Exception:
                pass
            t0 = time.time()
            while pid_alive(pid) and time.time() - t0 < grace:
                time.sleep(0.25)
            if pid_alive(pid):
                say(f"game did not exit within {grace}s of qqq - killing pid {pid}")
                self.kill()
            else:
                self.exit_info["exit_s"] = round(time.time() - t0, 2)
                say(f"game exited after {time.time() - t0:.1f}s")
        elif not pid:
            say("no Fallout4.exe was seen for this launch - nothing to quit")
        self._close()
        self.launched = False
        return self.exit_info["exit_s"]

    def kill(self):
        """taskkill /F the game (only the pid this harness launched)."""
        if not self.launched or not self.pid:
            return
        if pid_alive(self.pid):
            subprocess.run(["taskkill", "/PID", str(self.pid), "/F"], capture_output=True)
            if self.exit_info is None:
                self.exit_info = {"exit_s": None, "killed": True}
            self.exit_info["killed"] = True

    def _close(self):
        try:
            if self.f:
                self.f.close()
        except OSError:
            pass
        self.f = None

    # -- pipe ------------------------------------------------------------------------------
    def _try_open(self):
        try:
            self.f = open(PIPE, "r+b", buffering=0)
            return True
        except OSError:
            # only "disappeared" once our game process has actually been seen
            if self.launched and self.pid and not pid_alive(self.pid):
                raise SystemExit("game process disappeared during startup")
            return False

    def cmd(self, c, quiet=False):
        self.f.write((c + "\n").encode())
        r = b""
        while not r.endswith(b"\n"):
            chunk = self.f.read(4096)
            if not chunk:
                raise RuntimeError("pipe closed")
            r += chunk
        r = r.decode(errors="replace").strip()
        if not (quiet or self.quiet):
            say(f"> {c}\n    {r}")
        return r

    # -- waiting ---------------------------------------------------------------------------
    def _wait(self, what, timeout, pred, every=5):
        t0 = last = time.time()
        while True:
            if pred():
                say(f"{what}: ok after {time.time() - t0:.1f}s")
                return
            now = time.time()
            if now - t0 > timeout:
                raise SystemExit(f"{what}: timed out after {timeout}s")
            if now - last >= every:
                say(f"waiting for {what} ... {now - t0:.0f}s")
                last = now
            time.sleep(0.25)

    @staticmethod
    def log_mark():
        """Current end of the plugin log; pass to log_since() to read only newer lines."""
        try:
            return os.path.getsize(LOG)
        except OSError:
            return 0

    def log_since(self, mark):
        return self._log_since(mark)

    def _log_since(self, mark):
        # The plugin truncates its log at every game start, so a byte offset from before the launch
        # is meaningless. mark == "launch" means: the whole log, but only once it is the new run's
        # (its first timestamp is not earlier than the launch time).
        if mark == "launch":
            try:
                with open(LOG, "rb") as fh:
                    text = fh.read().decode("utf-8", errors="replace")
            except OSError:
                return ""
            m = re.match(r"\[(\d\d):(\d\d):(\d\d)", text)
            if not m:
                return ""
            first = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
            lt = time.localtime(self._launch_time)
            launch = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec
            return text if (first - launch) % 86400 < 43200 else ""
        try:
            # binary seek: a byte offset is not a valid text-mode seek cookie in general
            with open(LOG, "rb") as fh:
                fh.seek(mark if os.path.getsize(LOG) >= mark else 0)
                return fh.read().decode("utf-8", errors="replace")
        except OSError:
            return ""

    def ready(self):
        return parse_kv(self.cmd("ready", quiet=True))

    def status(self):
        return parse_kv(self.cmd("status", quiet=True))

    def coc(self, cell, timeout=60):
        self.cmd(f"console coc {cell}")
        time.sleep(1.0)  # let the loading menu open
        self._wait(f"in-world at {cell}", timeout, lambda: self.ready().get("ingame") == "1")


def module_files(pid, names=("fo4flatvr.dll", "FRIK.dll")):
    """{dll: (path the loader reports, real file mapped into the process)} for a running process. The mapped path is
    the file the kernel actually opened, i.e. the MO2 mod folder when the VFS supplied it."""
    import ctypes
    from ctypes import wintypes
    k32, psapi = ctypes.WinDLL("kernel32", use_last_error=True), ctypes.WinDLL("psapi", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    h = k32.OpenProcess(0x0400 | 0x0010, False, pid)   # QUERY_INFORMATION | VM_READ
    if not h:
        return {}
    out = {}
    try:
        mods = (wintypes.HMODULE * 2048)()
        need = wintypes.DWORD()
        psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
        psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
        psapi.GetMappedFileNameW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPWSTR, wintypes.DWORD]
        if not psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3):
            return {}
        devmap = {}
        buf = ctypes.create_unicode_buffer(1024)
        for d in "CDEFGHIJ":
            if k32.QueryDosDeviceW(f"{d}:", buf, 1024):
                devmap[buf.value] = f"{d}:"
        for m in mods[:need.value // ctypes.sizeof(wintypes.HMODULE)]:
            fn = ctypes.create_unicode_buffer(1024)
            psapi.GetModuleFileNameExW(h, m, fn, 1024)
            base = os.path.basename(fn.value)
            if base.lower() not in [n.lower() for n in names]:
                continue
            mp = ctypes.create_unicode_buffer(1024)
            psapi.GetMappedFileNameW(h, m, mp, 1024)
            real = mp.value
            for dev, letter in devmap.items():
                if real.lower().startswith(dev.lower() + "\\"):
                    real = letter + real[len(dev):]
            out[base] = (fn.value, real)
    finally:
        k32.CloseHandle(h)
    return out


STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"


def main():
    args = sys.argv[1:]
    if args and args[0] == "preflight":
        preflight(frik="--frik" in args)
        print(f"preflight OK: mods enabled, base Data clean, ini = {effective_ini()}, saves = {saves_dir()}, "
              f"MO2 window open: {mo2_running()}")
        return
    if not args or args[0] != "smoke":
        print(__doc__)
        return
    # smoke [--no-xr | --steamvr] [--load SAVE]: launch via MO2, load the save, pipe check, where the DLLs came from, quit
    save = args[args.index("--load") + 1] if "--load" in args else "vrtest_sanctuary"
    kw = {"no_xr": True} if "--no-xr" in args else ({"runtime_json": STEAMVR_JSON} if "--steamvr" in args else {})
    with Game(**kw) as g:
        g.cmd("status")
        for dll, (path, real) in module_files(g.pid).items():
            say(f"module {dll}: loader path {path} | mapped file {real}")
        log = g.log_since("launch")
        for ln in log.splitlines():
            if "XR_RUNTIME_JSON" in ln or "OpenXR up" in ln or "OpenXR init failed" in ln:
                say(f"log: {ln.strip()[:200]}")
        g.cmd(f"console load {save}")
        time.sleep(2.0)
        g._wait(f"in-world ({save})", 90, lambda: g.ready().get("ingame") == "1")
        g.cmd("ready")
        g.cmd("status")


if __name__ == "__main__":
    main()
