"""fo4flatvr settings table: the one source for the in-VR config panel (ConfigUI) and the MCM menu.

gen_mcm_config.py reads this and writes
  - plugin/dist/Data/MCM/Config/fo4flatvr/config.json + settings.ini (MCM pages; every key below is an MCM ModSetting,
    so the panel can set it through MCM's own API);
  - plugin/src/plugin/ConfigSpec.gen.inl (the C++ row table the panel draws).

A row: page, group, ini section, key, type, code default, range/step or options, label, help, apply.
  type     bool | int | float | enum (int index into options) | policy (string, camera policy) | vk (virtual-key code)
  default  the CODE default (what the plugin uses when neither fo4flatvr.ini nor MCM has the key)
  apply    "" = after a restart (then restart= must give the reason; it is shown in the help as "Restart: ...");
           else a command run when the value changes (preview, discard, save, file edit):
           a pipe command (CommandChannel) with {v} = the value, {onoff} = on/off, {enum:a,b,c} = the option word,
           or an internal action: @height (head height offset), @present (Presenter hotkeys), @configui (the panel's own),
           "gesture reload" (gestures / selfie read the file again),
           @reload (20 Nov, live settings: the module that owns the key re-reads it when the merged file changes,
           cfgstore::Subscribe in ConfigStore.h; FRIK-flat keys through the bridge v7 settings revision).
  mcm      False = not shown on an MCM page (strings and key codes MCM cannot show as a control); still an MCM ModSetting.
Analysis: analysis/vr-config-ui.md. Run python -I plugin/tools/gen_mcm_config.py after editing.
"""

PAGES = ["Quick", "Camera", "Comfort", "Render", "Height", "Gestures", "Selfie", "FRIK", "Keys"]

SLOTS = [  # (ini name, label) in VRIK's order (Gestures.h)
    ("Tap", "Press (no movement)"),
    ("Up", "Up"), ("Down", "Down"), ("Left", "Left"), ("Right", "Right"), ("Back", "Back (toward you)"),
    ("Fwd", "Forward"),
    ("UpDown", "Up, then down"), ("DownUp", "Down, then up"), ("LeftRight", "Left, then right"),
    ("RightLeft", "Right, then left"), ("BackFwd", "Back, then forward"), ("FwdBack", "Forward, then back"),
]
GESTURE_ACTIONS = ["None", "Key press", "Recentre view", "First / third person", "Camera view cycle (SCENE / TRACK / CINEMA)",
                   "VR settings panel (open / close)"]
GESTURE_DEFAULTS = {("R", "UpDown"): 5}  # right Up+Down opens the panel (20 Nov)
MODS = ["No modifier", "Shift", "Ctrl", "Alt"]

POLICIES = ["TRACK", "SCENE", "FADE_SNAP", "LOCK", "CINEMA"]
POLICIES_FP = ["TRACK", "FADE_SNAP", "LOCK", "CINEMA"]  # SCENE is refused for FirstPerson / IronSights
CAMERA_STATES = [  # (state, code default, label)
    ("FirstPerson", "TRACK", "First person"), ("IronSights", "TRACK", "Iron sights"), ("Furniture", "TRACK", "Sitting / furniture"),
    ("ThirdPerson", "SCENE", "Third person"), ("Dialogue", "SCENE", "Dialogue camera"), ("Free", "SCENE", "Free camera (AAF, tfc)"),
    ("AutoVanity", "SCENE", "Idle orbit"), ("Transition", "SCENE", "Transition"), ("Tween", "SCENE", "Tween"),
    ("Animated", "SCENE", "Animated camera"), ("Mount", "SCENE", "Mount"), ("Bleedout", "SCENE", "Bleedout"),
    ("VATS", "CINEMA", "VATS"),
]


def row(page, group, section, key, typ, default, label, help_="", mn=0.0, mx=1.0, step=0.1, options=None, apply="", mcm=True,
        restart=""):
    if restart:
        assert not apply, key
        help_ = (help_ + " " if help_ else "") + "Restart: " + restart.rstrip(".") + "."
    assert apply or restart, f"{section}/{key}: a restart row needs restart='<reason>'"
    return dict(page=page, group=group, section=section, key=key, type=typ, default=default, label=label, help=help_,
                min=mn, max=mx, step=step, options=options or [], apply=apply, mcm=mcm)


def rows():
    R = []
    # ---- Quick (actions are drawn by the panel itself; these are the most-used values) ----
    R.append(row("Quick", "View", "General", "fHeightOffsetCm", "float", 0.0, "Height offset (cm)",
                 "Raises (+) or lowers (-) the view on top of your real head height.", -60, 60, 1, apply="@height"))
    R.append(row("Quick", "View", "Comfort", "iTurnMode", "enum", 0, "Turning", "Right stick turning.",
                 options=["Smooth", "Snap", "Game (camera look)"], apply="comfort turn {enum:smooth,snap,game}"))
    R.append(row("Quick", "View", "Comfort", "bVignette", "bool", 1, "Comfort vignette",
                 "Dark edges while moving or turning with the stick.", apply="comfort vignette {onoff}"))
    R.append(row("Quick", "Aim", "Controllers", "bAimThrowsWithController", "bool", 1, "Throw where the controller points",
                 "Grenades and mines arc along the aim hand's pointing direction (0 = the game's facing).",
                 apply="aim throw {onoff}"))
    # 20 Nov (VATS agent): VATS playback shots from a player-relative first-person muzzle re-aimed from the real one
    R.append(row("Quick", "Aim", "Controllers", "bVatsShotFix", "bool", 1, "VATS shots from the real muzzle",
                 "A V.A.T.S. shot whose origin came out in the first-person rig's player-relative space starts at the real muzzle and is re-aimed at the target by the game. Live.",
                 apply="aim vatsshot {onoff}"))
    # 20 Nov (controls agent): FO4VR workshop controls + glyph remap; live since the live-settings round (cache re-read)
    R.append(row("Quick", "Controls", "Controllers", "bWorkshopFO4VR", "bool", 1, "Workshop controls as FO4VR",
                 "Hold Y enters workshop mode, Y leaves, X minimises the build menu, sticks navigate / rotate / push (0 = earlier mapping).",
                 apply="@reload"))
    R.append(row("Quick", "Controls", "Controllers", "bWorkshopStickUpFarther", "bool", 1, "Workshop: stick up pushes away",
                 "Right stick up moves a held workshop object away, down pulls it closer (0 = the reverse).", apply="@reload"))
    R.append(row("Quick", "Controls", "Controllers", "bPipboyLightDirect", "bool", 1, "Pip-Boy light through the engine",
                 "Hold the left grip: the engine's own Pip-Boy light toggle (0 = a 1.2 s Pip-Boy button hold on the pad).", apply="@reload"))
    # 21 Nov (controls agent): crosshair focus tick filter (UiSoundDiag.cpp; ticking report)
    R.append(row("Quick", "Controls", "Controllers", "iPickFocusSound", "enum", 0, "Focus tick on new objects",
                 "The tick the game plays when the activate pick moves to a new object. Filtered: an object ticks only when it was "
                 "not under the pick in the last few seconds (the hand ray flickers between neighbours).",
                 options=["Off (as FO4VR)", "Filtered", "Every change (flat game)"], apply="@reload"))
    R.append(row("Quick", "Controls", "Controllers", "fPickFocusSoundRepeatS", "float", 1.5, "Focus tick repeat window (s)",
                 "Filtered focus tick: an object ticks again only after this long away from the pick.", 0.0, 10.0, 0.1, apply="@reload"))
    # 21 Nov (controls agent): every engine sound start counted (UiSoundDiag.cpp; ticking after VATS)
    R.append(row("Quick", "Controls", "Controllers", "bSoundDiag", "bool", 1, "Sound diagnostic log",
                 "Logs sounds that repeat, stack or keep playing, with the code that started them (pass-through only).",
                 restart="the sound hooks are installed at game start"))
    R.append(row("Quick", "Controls", "Menus", "fWorkshopStatsScale", "float", 1.75, "Workshop stats panel size",
                 "Hand panel size while the left stick click is held (settlement stats).", 1.0, 3.0, 0.05, apply="@reload"))
    # 21 Nov (workshop agent): FO4VR wand placement + build area on the hand panel (WorkshopHand.h, Presenter.cpp)
    R.append(row("Quick", "Controls", "Controllers", "bWorkshopHandPointer", "bool", 1, "Workshop: place with the hand",
                 "Highlight and placement of workshop objects follow the aim hand's pointer, as FO4VR (0 = the camera ray). Live.",
                 apply="@reload"))
    R.append(row("Quick", "Controls", "Menus", "bWorkshopHandCrop", "bool", 1, "Workshop: build area on the hand",
                 "The hand panel shows only the build area of the workshop menu, readable (0 = the whole menu image). Live.",
                 apply="@reload"))
    R.append(row("Quick", "Controls", "Menus", "fWorkshopCropTop", "float", 0.45, "Workshop build area start",
                 "Where the build area starts, as a fraction of the menu height from the top.", 0.0, 0.9, 0.05, apply="@reload"))
    R.append(row("Quick", "Controls", "Controllers", "bGlyphRemap", "bool", 1, "Glyphs match the Touch buttons",
                 "Button glyphs in menus, HUD prompts and messages show the Touch control that does the action (0 = flat Xbox glyphs). "
                 "A menu already open keeps its glyphs until it opens again.", apply="@reload"))
    # ---- Camera ----
    for st, dflt, label in CAMERA_STATES:
        help_ = ("TRACK = head-tracked first-person world; SCENE = the game camera position + your head (AAF method); "
                 "FADE_SNAP = fade, snap; LOCK = hold; CINEMA = the flat screen panel.")
        if st == "Free":
            help_ += " With bFreeCamScene=1 the free camera is always SCENE."
        R.append(row("Camera", "View per camera state", "Camera", f"sPolicy{st}", "policy", dflt, label, help_,
                     options=POLICIES_FP if st in ("FirstPerson", "IronSights") else POLICIES,
                     apply=f"camdir policy {st} {{v}}", mcm=False))
    R.append(row("Camera", "Fades", "Camera", "fFadeMs", "float", 100.0, "Fade length (ms)", "Every camera fade to black.", 0, 1000, 10,
                 apply="camdir cfg fadems {v}"))
    R.append(row("Camera", "Fades", "Camera", "fFadeTimeoutS", "float", 0.1, "Longest black hold (s)", "", 0, 10, 0.05,
                 apply="camdir cfg timeout {v}"))
    R.append(row("Camera", "Fades", "Camera", "fJumpFadeUnits", "float", 50.0, "Jump blink (units)",
                 "A game camera jump larger than this per update blinks (0 = off).", 0, 300, 5, apply="camdir cfg jump {v}"))
    R.append(row("Camera", "Third person", "Camera", "bThirdPersonReturn", "bool", 1, "Return to first person",
                 "A script's third person (AAF, Force3rdPerson) goes back to first person once you are in control again.",
                 apply="camdir cfg return {v}"))
    R.append(row("Camera", "Third person", "Camera", "fThirdPersonReturnS", "float", 1.0, "Return after (s)", "", 0, 10, 0.25,
                 apply="camdir cfg returns {v}"))
    R.append(row("Camera", "Third person", "Camera", "bPovHeadHide", "bool", 1, "Hide the head the view sits in",
                 "Free-camera scene view (AAF POV): the head at the camera is hidden; with Camera stays on the body, your own head.", apply="camdir cfg headhide {v}"))
    R.append(row("Camera", "Third person", "Camera", "bFreeCamScene", "bool", 1, "Free camera = scene view",
                 "The free camera (AAF scenes, tfc) is head-tracked like a scene (0 = sPolicyFree). Live: the free-camera hook stays "
                 "installed and switches.", apply="@reload"))
    # 20 Nov: camera-lock comparison modes, HUD notifications, same controls in every view (CameraDirector.h)
    R.append(row("Camera", "Camera locks (compare)", "Camera", "bLockFirstPerson", "bool", 0, "Always first person",
                 "Refuse every game request to leave first person (third person, free camera, furniture, animated, bleedout, "
                 "mount, dialogue camera, kill cams); VATS excepted.", apply="camdir cfg lockfp {v}"))
    R.append(row("Camera", "Camera locks (compare)", "Camera", "bBodyCamera", "bool", 0, "Camera stays on the body",
                 "Every camera state (VATS excepted) is seen from your first-person eye on the player body.",
                 apply="camdir cfg body {v}"))
    # 21 Nov (camera agent): the body camera follows the animated body in scenes; AAF POV on another actor wins; free camera
    # position read after AAF's write (all live)
    R.append(row("Camera", "Camera locks (compare)", "Camera", "bBodyCameraFollowsAnimation", "bool", 1, "Body camera follows the animation",
                 "With Camera stays on the body: in scenes, furniture and paired animations the eye rides your animated head instead of the spot your body stood on (0 = always the capsule position). Live.", apply="@reload"))
    R.append(row("Camera", "Camera locks (compare)", "Camera", "fBodyCameraEyeForward", "float", 8.0, "Body camera eye in front of the head",
                 "Units from the animated Head bone to the eye, along the face. Live.", 0, 20, 1, apply="@reload"))
    R.append(row("Camera", "Camera locks (compare)", "Camera", "bBodyCameraYieldsToPov", "bool", 1, "Body camera yields to AAF POV",
                 "With Camera stays on the body: AAF's POV on another actor's head is shown from that head (0 = always your own head). Live.", apply="@reload"))
    R.append(row("Camera", "Camera locks (compare)", "Camera", "bCameraHudNotify", "bool", 1, "HUD message on camera changes",
                 "Camera state and view mode changes, and refused requests.", apply="camdir cfg notify {v}"))
    R.append(row("Camera", "Camera locks (compare)", "Camera", "bSameInputAllViews", "bool", 1, "Same controls in every view",
                 "Gameplay bindings and comfort in every camera view while no menu is open (0 = menu layout outside first person).",
                 apply="camdir cfg sameinput {v}"))
    # 20 Nov late (camera/AAF agent): AAF / defeat scenes, orbit cameras, AAF POV stick turn, Pip-Boy after scenes (all live)
    R.append(row("Camera", "AAF and defeat scenes", "Camera", "bLockAllowScenes", "bool", 1, "Scenes pass the camera locks",
                 "With Always first person on, AAF scenes and defeat flows (bleedout, then the script's third person) keep their "
                 "cameras; the lock comes back 2 s after the scene.", apply="@reload"))
    R.append(row("Camera", "AAF and defeat scenes", "Camera", "fDefeatWindowS", "float", 30.0, "Defeat window after a bleedout (s)",
                 "A script's third person this long after your bleedout counts as the defeat scene.", 0, 120, 5, apply="@reload"))
    R.append(row("Camera", "AAF and defeat scenes", "Camera", "bOrbitStatesScene", "bool", 1, "Orbit cameras: TRACK = SCENE",
                 "TRACK on third person, bleedout, mount, idle orbit or the dialogue camera is shown as SCENE (TRACK made the view "
                 "and the movement follow the head there).", apply="@reload"))
    R.append(row("Camera", "AAF and defeat scenes", "Camera", "bFreeCamOwnStickYaw", "bool", 1, "Free camera keeps the stick turn",
                 "The stick's turn in the free camera / AAF POV stays for the whole scene (AAF's POV auto mode no longer pulls it "
                 "back).", apply="@reload"))
    R.append(row("Camera", "AAF and defeat scenes", "Camera", "bFreeCamStatePosition", "bool", 1, "Free camera: this frame's position",
                 "The free camera's position is read after AAF writes it (POV head, glides), not the previous frame's. Live.",
                 apply="@reload"))
    R.append(row("Camera", "AAF and defeat scenes", "Camera", "bSceneAftercare", "bool", 1, "Pip-Boy after scenes",
                 "A refused Pip-Boy press shows why; what a finished scene left behind (AI control, input layers) is released.",
                 apply="@reload"))
    # ---- Comfort ----
    R.append(row("Comfort", "Turning", "Comfort", "iTurnMode", "enum", 0, "Turning", "Right stick turning.",
                 options=["Smooth", "Snap", "Game (camera look)"], apply="comfort turn {enum:smooth,snap,game}"))
    R.append(row("Comfort", "Turning", "Comfort", "fSnapTurnDeg", "float", 30.0, "Snap angle (deg)", "", 10, 90, 5,
                 apply="comfort cfg snapdeg {v}"))
    R.append(row("Comfort", "Turning", "Comfort", "fSmoothTurnDegPerSec", "float", 120.0, "Smooth turn speed (deg/s)", "", 30, 360, 10,
                 apply="comfort cfg rate {v}"))
    R.append(row("Comfort", "Turning", "Comfort", "fTurnDeadzone", "float", 0.15, "Turn dead zone", "", 0.0, 0.5, 0.05,
                 apply="comfort cfg deadzone {v}"))
    R.append(row("Comfort", "Moving", "Comfort", "iMoveDirection", "enum", 0, "Move direction", "Left stick forward follows ...",
                 options=["Head", "Off hand", "Body"], apply="comfort move {enum:head,hand,body}"))
    R.append(row("Comfort", "Moving", "Comfort", "bVignette", "bool", 1, "Comfort vignette",
                 "Dark edges while moving or turning with the stick.", apply="comfort vignette {onoff}"))
    R.append(row("Comfort", "Moving", "Comfort", "fVignetteStrength", "float", 0.6, "Vignette strength", "", 0.0, 1.0, 0.05,
                 apply="comfort cfg strength {v}"))
    R.append(row("Comfort", "Moving", "Comfort", "fVignetteFadeMs", "float", 100.0, "Vignette fade (ms)", "", 0, 500, 10,
                 apply="comfort cfg fade {v}"))
    R.append(row("Comfort", "Effects", "Comfort", "bNoCameraShake", "bool", 1, "No camera shake", "Explosions, power armour steps, scripts.",
                 apply="comfx shake {onoff}"))
    R.append(row("Comfort", "Effects", "Comfort", "bNoRadialBlur", "bool", 1, "No radial blur", "", apply="comfx radial {onoff}"))
    R.append(row("Comfort", "Effects", "Comfort", "bNoDoubleVision", "bool", 1, "No double vision", "", apply="comfx double {onoff}"))
    R.append(row("Comfort", "Effects", "Comfort", "bNoImodBlur", "bool", 1, "No full-screen blur", "", apply="comfx blur {onoff}"))
    R.append(row("Comfort", "Effects", "Comfort", "bNoDepthOfField", "bool", 0, "No depth of field",
                 "Also the world's permanent depth of field.", apply="comfx dof {onoff}"))
    # ---- 20 Nov: FO4VR HUD placement (HUD agent; HudLayer.h SplitConfig) - all live through "hud set <key> {v}" ----
    def hud(key, typ, dflt, label, help_="", mn=0.0, mx=1.0, step=0.1, options=None):
        return row("Render", "HUD (FO4VR placement)", "HUD", key, typ, dflt, label, help_, mn, mx, step, options,
                   apply=f"hud set {key} {{v}}")
    R.append(hud("bHandHud", "bool", 1, "Meters on the hand",
                 "Health, rads, AP, ammo, effects on a panel beside the aim hand, like FO4VR's wand HUD."))
    R.append(hud("fHandHudWidthM", "float", 0.13, "Hand panel width (m)", "", 0.05, 0.4, 0.01))
    R.append(hud("fHandHudX", "float", -0.09, "Hand panel X (m)", "Sideways from the controller (mirrored for the left hand).", -0.3, 0.3, 0.01))
    R.append(hud("fHandHudY", "float", -0.02, "Hand panel Y (m)", "Up from the controller.", -0.3, 0.3, 0.01))
    R.append(hud("fHandHudZ", "float", 0.0, "Hand panel Z (m)", "Back toward you from the controller.", -0.3, 0.3, 0.01))
    R.append(hud("fHandHudPitchDeg", "float", 0.0, "Hand panel tilt (deg)", "+ = top toward you.", -90, 90, 5))
    R.append(hud("bCompassChin", "bool", 1, "Compass under the chin",
                 "FO4VR's compass panel: follows your heading, look down to read it. Off = on the head-locked HUD."))
    R.append(hud("fCompassDistanceM", "float", 0.32, "Compass distance (m)", "", 0.1, 1.5, 0.02))
    R.append(hud("fCompassHeightM", "float", -0.55, "Compass height (m)", "Below the eyes (negative).", -1.0, 0.2, 0.02))
    R.append(hud("fCompassWidthM", "float", 0.55, "Compass width (m)", "", 0.1, 1.5, 0.05))
    R.append(hud("iCompassShow", "enum", 2, "Show compass",
                 "Slides in and out like VRIK's compass.",
                 options=["Always", "Never", "Hand raised (not high)", "Palm up (VRIK)"]))
    R.append(hud("iCompassHand", "enum", 0, "Compass hand", "", options=["Either", "Left", "Right"]))
    R.append(hud("fCompassRaiseLowM", "float", 0.45, "Raise: lowest (m below eyes)", "", 0.1, 1.0, 0.05))
    R.append(hud("fCompassRaiseHighM", "float", 0.05, "Raise: highest (m below eyes)", "Higher than this hides it again.", -0.5, 0.5, 0.05))
    R.append(hud("fCompassPalmUpDot", "float", 0.65, "Palm-up threshold", "VRIK: 0.65.", 0.0, 1.0, 0.05))
    R.append(hud("fCompassSlideM", "float", 0.714, "Compass slide (m)", "VRIK: 50 units.", 0.0, 2.0, 0.05))
    R.append(hud("fCompassSlideRate", "float", 10.0, "Compass slide rate", "VRIK: 10 (about 0.4 s).", 1.0, 40.0, 1.0))
    # sHandHudLeftClips / sHandHudRightClips / sCompassClips, [Menus] sPanelMenus / sNoPanelMenus: string lists, ini only (no string
    # row type here, like sWindowMenus); read at start-up.
    R.append(row("Render", "Menus", "Menus", "bPanelModMenus", "bool", 1, "Mod menus on the panel",
                 "Mod menus that take input (Silhouette, Screen Archer Menu, F4SE menus) on the menu panel with the pointer.",
                 apply="menupanel modmenus {onoff}"))
    # ---- Render ----
    RT = ("the game sizes its whole render-target set (about 100 targets, the post chains, the stereo seam guard) from the "
          "swapchain once, when its renderer starts; flat Fallout 4 has no in-game resolution change")
    R.append(row("Render", "Resolution", "Render", "iWidth", "int", 2880, "Render width (both eyes)",
                 "The game's render width; with stereo each eye gets half.", 1024, 8192, 64, restart=RT))
    R.append(row("Render", "Resolution", "Render", "iHeight", "int", 1620, "Render height", "", 512, 4320, 32, restart=RT))
    R.append(row("Render", "Resolution", "Render", "fRenderScale", "float", 1.0, "Render scale",
                 "Multiplies the size above (like FO4VR's fRenderTargetSizeMultiplier).", 0.5, 2.0, 0.05, restart=RT))
    R.append(row("Render", "Desktop window", "Window", "iWidth", "int", 1280, "Window width",
                 "The desktop mirror window; the render size is not affected.", 640, 3840, 64, apply="@reload"))
    R.append(row("Render", "Desktop window", "Window", "iHeight", "int", 720, "Window height", "", 360, 2160, 36, apply="@reload"))
    R.append(row("Render", "Stereo", "Stereo", "iMode", "enum", 3, "Stereo mode",
                 "Single-pass stereo. Switches live (the render size stays the one chosen at start; with bInstallHooks=0 and a mono "
                 "start it needs a restart).",
                 options=["0 Off (mono)", "1 Census (diagnostic)", "2 Debug (diagnostic)", "3 Stereo", "4 Identity (diagnostic)"],
                 apply="@reload"))
    # 22 Nov (hidden-area agent): FO4VR bUseHiddenAreaMesh port (HiddenArea.inl); live (ApplyStereoLive -> stereo::Configure)
    R.append(row("Render", "Stereo", "Stereo", "bHiddenAreaMask", "bool", 1, "Hidden-area mask",
                 "Skips shading the pixels the headset lenses never show (the runtime's visibility mask, stamped into depth before "
                 "the world draws), as FO4VR does. Nothing changes where you can see. Needs the runtime to offer it (logged).",
                 apply="@reload"))
    for key, dflt, label in [("bCutTaa", 0, "Cut TAA (FXAA instead)"), ("bCutMotionBlur", 0, "Cut motion blur"),
                             ("bCutDof", 0, "Cut depth of field"), ("bCutLensFlare", 0, "Cut lens flare"),
                             ("bCutGodrays", 1, "Cut god rays"), ("bCutSsr", 0, "Cut screen-space reflections"),
                             ("bCutHbao", 1, "Cut HBAO"), ("bCutSao", 1, "Cut SAO"), ("bCutTiledLighting", 0, "Cut tiled lighting")]:
        R.append(row("Render", "Stereo cuts (only while stereo)", "Stereo", key, "bool", dflt, label,
                     "Applies at the next frame (the engine reads these switches every frame).", apply="@reload"))
    # 20 Nov (threading audit, analysis/threading.md); live since the live-settings round (hook always installed, gated)
    R.append(row("Render", "Threading", "General", "bMainThreadQueue", "bool", 1, "Game work on the main thread",
                 "Our game / menu / scene changes run on the game's main thread between frames (where console commands run). "
                 "0 = F4SE's task queue (any thread) and polls at Present, as before 20 Nov.", apply="@reload"))
    # 20 Nov (frame-timing agent, work/handoff/head-timing.md "GPU frame queue"): live (Presenter re-reads [General])
    R.append(row("Render", "Threading", "General", "bGpuQueueLimit", "bool", 1, "Limit the GPU frame queue",
                 "When the GPU is the bottleneck, wait for it before the next head pose is predicted, so frames are not drawn with a "
                 "pose meant for an earlier display time (laggy / unstable head under GPU load). No wait when the GPU keeps up.",
                 apply="@reload"))
    R.append(row("Render", "Threading", "General", "iGpuQueueFrames", "int", 1, "GPU frames queued at most",
                 "Frames still unfinished on the GPU when the next head pose is predicted. 1 = default; 0 = lowest latency, lower "
                 "frame rate when GPU-bound; 2 = more frame rate headroom, more latency.", 0, 2, 1, apply="@reload"))
    # ---- DLSS + neural rendering (20 Nov, DLSS agent; plugin Dlss.cpp, analysis/dlss-in-fo4flatvr.md). Every row live:
    # Dlss.cpp re-reads [DLSS] / [NeuralRendering] on any change; features are rebuilt when a creation value changes.
    G1, G2, G3, G4, G5 = "DLSS", "DLSS: advanced", "Neural rendering", "Neural rendering: model", "Neural rendering: passes 2-4"
    G6 = "Neural rendering: look"
    R.append(row("Render", G1, "DLSS", "bEnabled", "bool", 1, "DLSS (per-eye DLAA)",
                 "NVIDIA DLSS at native resolution per eye, in place of the game's TAA (the TAA cut is lifted while it runs). "
                 "0 = the game's TAA / bCutTaa as before.", apply="@reload"))
    R.append(row("Render", G1, "DLSS", "bPerEye", "bool", 1, "One DLSS instance per eye",
                 "0 = one shared instance over both eyes (test: one history over the double-wide image).", apply="@reload"))
    R.append(row("Render", G1, "DLSS", "iPreset", "enum", 5, "DLSS preset",
                 "Render preset hint. K (default): NVIDIA's DLAA preset, measured 1.6 ms for both eyes and the steadiest. "
                 "L is what the DLSS 5 Feed sends (it found fewer NR speckles on bright text with L / M) but costs about 7.4 ms here.",
                 options=["Feed default (L)", "Driver default", "E", "F", "J", "K", "L", "M"], apply="@reload"))
    R.append(row("Render", G1, "DLSS", "iQuality", "enum", 0, "Quality mode",
                 "Sent at creation (DLSS accepts every mode at the eye size; measured 20 Nov). The input stays at the eye size: "
                 "rendering below it is not part of this build. A mode DLSS refuses falls back to DLAA (logged).",
                 options=["DLAA", "Ultra quality", "Quality", "Balanced", "Performance", "Ultra performance"], apply="@reload"))
    R.append(row("Render", G1, "DLSS", "fSharpness", "float", 0.0, "Sharpness",
                 "NGX sharpness (the Feed's work_sharpness); current DLSS presets ignore it.", 0.0, 1.0, 0.05, apply="@reload"))
    R.append(row("Render", G1, "DLSS", "bJitter", "bool", 1, "Sub-pixel jitter",
                 "The game's TAA camera jitter stays on so DLSS gets sub-pixel detail. 0 = no jitter (TAA stays active for DLSS).",
                 apply="@reload"))
    R.append(row("Render", G1, "DLSS", "bGpuTimers", "bool", 1, "GPU timers",
                 "GPU cost line in the log every 600 frames (DLSS, NR, D3D12 NR per eye).", apply="@reload"))
    R.append(row("Render", G2, "DLSS", "iJitterFlipX", "enum", 0, "Jitter sign X", "The Feed's jitter_sign, per axis.",
                 options=["Normal", "Flipped"], apply="@reload"))
    R.append(row("Render", G2, "DLSS", "iJitterFlipY", "enum", 0, "Jitter sign Y", "", options=["Normal", "Flipped"], apply="@reload"))
    R.append(row("Render", G2, "DLSS", "fMvScaleX", "float", 1.0, "Motion vector scale X", "The Feed's mv_scale_x (multiplier).",
                 -2.0, 2.0, 0.05, apply="@reload"))
    R.append(row("Render", G2, "DLSS", "fMvScaleY", "float", 1.0, "Motion vector scale Y", "The Feed's mv_scale_y (multiplier).",
                 -2.0, 2.0, 0.05, apply="@reload"))
    R.append(row("Render", G2, "DLSS", "iDepthInverted", "enum", 0, "Depth inverted",
                 "The Feed's depth_inverted (the game's depth is not inverted).",
                 options=["Auto (not inverted)", "Not inverted", "Inverted"], apply="@reload"))
    R.append(row("Render", G2, "DLSS", "bAutoExposure", "bool", 1, "Auto exposure", "NGX auto exposure flag.", apply="@reload"))
    R.append(row("Render", G2, "DLSS", "iCreateFlags", "int", -1, "Create flags override",
                 "The Feed's flags: raw NGX create flags, -1 = ours.", -1, 255, 1, apply="@reload"))
    R.append(row("Render", G2, "DLSS", "bBiasMask", "bool", 0, "TAA mask as bias mask",
                 "Pass the game's TAA mask (RT 0x1E) as DLSS's bias-current-colour mask.", apply="@reload"))
    R.append(row("Render", G2, "DLSS", "bCopyHistory", "bool", 1, "Write the TAA history too",
                 "Also copy the result into the game's TAA history target, so turning DLSS off resumes without a stale frame.",
                 apply="@reload"))
    R.append(row("Render", G2, "DLSS", "iResetEvery", "int", 0, "Reset every N frames", "The Feed's reset_every (0 = never).",
                 0, 600, 1, apply="@reload"))
    R.append(row("Render", G2, "DLSS", "iCreateDelay", "int", 0, "Create after N frames", "The Feed's create_delay.", 0, 600, 1,
                 apply="@reload"))
    R.append(row("Render", G2, "DLSS", "iWarmupRebuild", "int", 0, "Rebuild once after N frames", "The Feed's warmup_rebuild (0 = off).",
                 0, 1000, 10, apply="@reload"))
    R.append(row("Render", G2, "DLSS", "iLogFrames", "int", 3, "Log the first N evaluates", "The Feed's log_frames.", 0, 60, 1,
                 apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "bEnabled", "bool", 1, "Neural rendering (DLSS 5)",
                 "NVIDIA's neural rendering model per eye (RTX 50). Runs after DLSS (or on the game's TAA when DLSS is off).",
                 apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "iToggleKey", "vk", 0x75, "Toggle key",
                 "Turns neural rendering on/off for the session (not saved), game window focused. RenoDX: F6.", options=["@vk"],
                 apply="@reload", mcm=False))
    R.append(row("Render", G3, "NeuralRendering", "bPerEye", "bool", 1, "One NR instance per eye",
                 "0 = one shared instance over both eyes side by side (test for eye-to-eye consistency).", apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "fIntensity", "float", 1.0, "Intensity", "The model's intensity (NRIntensity).",
                 0.0, 2.0, 0.05, apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "fColorStrength", "float", 1.0, "Colour strength",
                 "1 = NR's colour and brightness, 0 = its brightness change only (NRColorStrength).", 0.0, 1.0, 0.05, apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "fTransferStrength", "float", 1.0, "Transfer strength",
                 "Fades NR's result toward the input (NRTransferStrength; 1 = all of it).", 0.0, 1.0, 0.05, apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "fResolutionScale", "float", 0.5, "NR resolution",
                 "Working size per eye (NRResolutionScale); the model's edit is carried back to full resolution.", 0.33, 1.0, 0.01,
                 apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "bFollowInputRes", "bool", 0, "NR at the render resolution",
                 "NRFollowInputRes: the eye size (= scale 1 here).", apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "iHookPoint", "enum", 0, "Where NR runs",
                 "After DLSS (RenoDX's default) or on the game's raw (jittered) colour before DLSS (NRHookPoint).",
                 options=["After DLSS", "Before DLSS"], apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "iSdrBlackRestore", "enum", 3, "Black level restore",
                 "Gives back NR's lift in near-black 32x32 blocks (NRSdrBlackRestore).",
                 options=["Off", "Auto (dark blocks)", "Every block", "Smooth (dark blocks, tapered)"], apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "iDetailStability", "enum", 0, "Detail stability",
                 "NRDetailStability: NR's edit is filtered over time (reprojected with the motion vectors, clipped to its neighbourhood). "
                 "Auto = on after DLSS.", options=["Auto", "Off", "Always"], apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "fDetailStabilityAlpha", "float", 0.15, "Detail stability weight",
                 "Share of the new frame's edit (RenoDX: 0.15; 1 = no filtering).", 0.02, 1.0, 0.01, apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "iDepthMode", "enum", 0, "Depth", "NRDepthMode.",
                 options=["Auto (not inverted)", "Normal", "Inverted"], apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "fMvScaleX", "float", 1.0, "NR motion vector scale X", "NRMVecScaleX.", -2.0, 2.0, 0.05,
                 apply="@reload"))
    R.append(row("Render", G3, "NeuralRendering", "fMvScaleY", "float", 1.0, "NR motion vector scale Y", "NRMVecScaleY.", -2.0, 2.0, 0.05,
                 apply="@reload"))
    R.append(row("Render", G4, "NeuralRendering", "iPreset", "enum", 0, "NR preset", "The runtime's model presets (NRPreset).",
                 options=["Default", "Preset #1", "Preset #2", "Preset #3"], apply="@reload"))
    R.append(row("Render", G4, "NeuralRendering", "iStyle", "enum", 0, "Style", "NRStyle.", options=["Default", "Natural", "Cinematic"],
                 apply="@reload"))
    R.append(row("Render", G4, "NeuralRendering", "fLocalTone", "float", 1.0, "Local tone", "NRLocalTone.", 0.0, 2.0, 0.05, apply="@reload"))
    R.append(row("Render", G4, "NeuralRendering", "fLocalStructure", "float", 1.0, "Structure", "NRLocalStructure.", 0.0, 2.0, 0.05,
                 apply="@reload"))
    R.append(row("Render", G4, "NeuralRendering", "fSkinStructure", "float", -1.0, "Character / skin",
                 "NRSkinStructure: -1 = stock; read only with the character mask.", -1.0, 1.0, 0.05, apply="@reload"))
    R.append(row("Render", G4, "NeuralRendering", "bAutoMask", "bool", 0, "Character mask", "NRAutoMask.", apply="@reload"))
    R.append(row("Render", G4, "NeuralRendering", "bSkinIndependent", "bool", 1, "Skin independent of structure", "NRSkinIndependent.",
                 apply="@reload"))
    R.append(row("Render", G4, "NeuralRendering", "bUiCorrection", "bool", 0, "UI correction", "NRUICorrection.", apply="@reload"))
    R.append(row("Render", G5, "NeuralRendering", "iPasses", "int", 1, "NR passes",
                 "NRPasses: chained NR passes (each one more model evaluate per eye).", 1, 4, 1, apply="@reload"))
    R.append(row("Render", G5, "NeuralRendering", "bChainedHistory", "bool", 1, "Passes 2-4 keep history",
                 "NRChainedHistory (0 = passes 2-4 reset every frame).", apply="@reload"))
    for n in (2, 3, 4):
        pre = f"Pass{n}"
        R.append(row("Render", G5, "NeuralRendering", pre + "bFollowPass1", "bool", 1, f"Pass {n}: same as pass 1",
                     "Model steering (style, tone, structure, skin, mask, UI) from pass 1.", apply="@reload"))
        for key, typ, dflt, label, mn, mx, st, opts in [
                ("fIntensity", "float", 1.0, "intensity", 0.0, 2.0, 0.05, None),
                ("fColorStrength", "float", 1.0, "colour strength", 0.0, 1.0, 0.05, None),
                ("fTransferStrength", "float", 1.0, "transfer strength", 0.0, 1.0, 0.05, None),
                ("iStyle", "enum", 0, "style", 0, 1, 0.1, ["Default", "Natural", "Cinematic"]),
                ("fLocalTone", "float", 1.0, "local tone", 0.0, 2.0, 0.05, None),
                ("fLocalStructure", "float", 1.0, "structure", 0.0, 2.0, 0.05, None),
                ("fSkinStructure", "float", -1.0, "character / skin", -1.0, 1.0, 0.05, None),
                ("bAutoMask", "bool", 0, "character mask", 0, 1, 0.1, None),
                ("bUiCorrection", "bool", 0, "UI correction", 0, 1, 0.1, None)]:
            R.append(row("Render", G5, "NeuralRendering", pre + key, typ, dflt, f"Pass {n}: {label}", f"NRPass{n} (RenoDX).", mn, mx,
                         st, options=opts, apply="@reload"))
    # RenoDX v8.5 look stage (look_stage.hpp): reshapes NR's edit before the resolve; every gain default = the identity
    R.append(row("Render", G6, "NeuralRendering", "bLook", "bool", 1, "Look stage",
                 "RenoDX's NR look stage: the gains below, the edge-aware upsampling and detail stability in the log domain. "
                 "Off = the earlier pipeline (area-mean input, bilinear edit, its own detail-stability pass).", apply="@reload"))
    R.append(row("Render", G6, "NeuralRendering", "bLookMode", "bool", 1, "Adjust NR's look",
                 "NRLookMode: applies the look gains below (all at their identity by default). Off = gains ignored; upsampling and "
                 "detail stability still run.", apply="@reload"))
    for key, dflt, label, help_, mn, mx, st in [
            ("fLookStrength", 1.0, "Strength", "NRLookStrength: multiplies NR's whole edit (stops).", 0.0, 2.0, 0.05),
            ("fLookBrighten", 1.0, "Brighten", "NRLookBrighten: gain on the parts NR brightens.", 0.0, 2.0, 0.05),
            ("fLookDarken", 1.0, "Darken", "NRLookDarken: gain on the parts NR darkens.", 0.0, 2.0, 0.05),
            ("fLookMaxBrighten", 0.0, "Max brighten (stops)", "NRLookMaxBrighten: soft limit on brightening, 0 = off.", 0.0, 4.0, 0.05),
            ("fLookMaxDarken", 0.0, "Max darken (stops)", "NRLookMaxDarken: soft limit on darkening, 0 = off.", 0.0, 4.0, 0.05),
            ("fLookColour", 1.0, "Colour", "NRLookColour: NR's saturation change (toward / away from grey).", 0.0, 2.0, 0.05),
            ("fLookHue", 1.0, "Hue", "NRLookHue: NR's tint change.", 0.0, 2.0, 0.05),
            ("fLookMaxColour", 0.0, "Max colour", "NRLookMaxColour: soft limit on the colour change, 0 = off.", 0.0, 2.0, 0.05),
            ("fLookShadows", 1.0, "Shadows", "NRLookShadows: edit gain in dark zones.", 0.0, 2.0, 0.05),
            ("fLookMidtones", 1.0, "Midtones", "NRLookMidtones: edit gain in mid zones.", 0.0, 2.0, 0.05),
            ("fLookHighlights", 1.0, "Highlights", "NRLookHighlights: edit gain in bright zones.", 0.0, 2.0, 0.05),
            ("fLookTone", 1.0, "Tone (large scale)", "NRLookTone: gain on the edit's broad band (guided filter split).", 0.0, 2.0, 0.05),
            ("fLookDetail", 1.0, "Detail (fine scale)", "NRLookDetail: gain on the edit's fine band.", 0.0, 2.0, 0.05),
            ("fLookDetailRadius", 1.0, "Detail radius (%)", "NRLookDetailRadius: band split radius, percent of the NR height.",
             0.25, 4.0, 0.05),
            ("fLookHalo", 0.0, "Halo suppression", "NRLookHalo: lifts NR's darkening next to brighter pixels.", 0.0, 1.0, 0.05),
            ("fLookHaloBrightSide", 0.0, "Halo suppression (bright side)",
             "NRLookHaloBrightSide: lifts the dark skirt on the bright side of dark objects.", 0.0, 1.0, 0.05)]:
        R.append(row("Render", G6, "NeuralRendering", key, "float", dflt, label, help_, mn, mx, st, apply="@reload"))
    R.append(row("Render", G6, "NeuralRendering", "iLookStabilize", "enum", 0, "Stabilize",
                 "NRLookStabilize: temporal filter on the edit's bands (time constant below). Off = detail stability decides.",
                 options=["Off", "Static", "Motion-compensated"], apply="@reload"))
    R.append(row("Render", G6, "NeuralRendering", "fLookStabilizeMs", "float", 60.0, "Stabilize time (ms)", "NRLookStabilizeMs.",
                 10.0, 250.0, 5.0, apply="@reload"))
    R.append(row("Render", G6, "NeuralRendering", "bLookStabilizeDetail", "bool", 0, "Stabilize the fine band too",
                 "NRLookStabilizeDetail (off = the broad band and colour only).", apply="@reload"))
    R.append(row("Render", G6, "NeuralRendering", "iLookUpsample", "enum", 0, "Upsampling",
                 "NRLookUpsample, when NR runs below the eye size: Auto / Edge-aware = joint-bilateral transport of the edit "
                 "with a point-sampled model input; Classic = bilinear edit; Area input = edge-aware with an area-mean input. "
                 "A change of input restarts NR's history.",
                 options=["Auto", "Edge-aware", "Classic", "Edge-aware (area input)"], apply="@reload"))
    # ---- Height / roomscale ----
    R.append(row("Height", "Height", "General", "fHeightOffsetCm", "float", 0.0, "Height offset (cm)",
                 "Raises (+) or lowers (-) the view on top of your real head height.", -60, 60, 1, apply="@height"))
    R.append(row("Height", "Height", "General", "fUnitsPerMeter", "float", 70.0, "World scale (units per metre)",
                 "How far the view moves per metre of head movement (head, hands, roomscale, stereo eyes, FRIK, scene views).", 50, 100, 1,
                 apply="@reload"))
    R.append(row("Height", "Height", "General", "bNoScaling", "bool", 0, "Game's default scale (no scaling)",
                 "Nothing of ours scales the body or the world: FRIK body at scale 1 (no PlayerHeight / auto body height / arm-length "
                 "scaling), 70 units per metre. Live.", apply="frik noscale"))
    R.append(row("Height", "Height", "OpenXR", "bRecentreOnPoseJump", "bool", 1, "Re-anchor on a start-up pose jump",
                 "VirtualDesktopXR's first pose is a placeholder; a > 0.5 m jump re-anchors until the first recentre.", apply="@reload"))
    R.append(row("Height", "Roomscale", "Roomscale", "bRoomscale", "bool", 1, "Roomscale walking",
                 "Walking in the room moves the player.", apply="rs {onoff}"))
    R.append(row("Height", "Roomscale", "Roomscale", "fDeadzoneCm", "float", 12.0, "Dead zone (cm)",
                 "Head movement within this stays a lean, beyond it the body follows.", 2, 40, 1, apply="rs cfg deadzone {v}"))
    R.append(row("Height", "Roomscale", "Roomscale", "fMaxResidualCm", "float", 25.0, "Wall residual (cm)",
                 "How far the view may stay ahead of a blocked body.", 5, 100, 5, apply="rs cfg max {v}"))
    R.append(row("Height", "Body turning", "Roomscale", "bBodyFollowsHead", "bool", 1, "Body follows the head", "",
                 apply="rs cfg body {v}"))
    R.append(row("Height", "Body turning", "Roomscale", "fBodyYawThresholdDeg", "float", 45.0, "Turn the body after (deg)", "",
                 10, 120, 5, apply="rs cfg yawthr {v}"))
    R.append(row("Height", "Body turning", "Roomscale", "fBodyYawRateDeg", "float", 180.0, "Body turn speed (deg/s)", "",
                 30, 720, 10, apply="rs cfg yawrate {v}"))
    # ---- Gestures ----
    R.append(row("Gestures", "Gestures", "Gestures", "bEnabled", "bool", 1, "Gestures", "Master switch.", apply="gesture reload"))
    R.append(row("Gestures", "Gestures", "Gestures", "bRightHand", "bool", 1, "Right stick gestures", "", apply="gesture reload"))
    R.append(row("Gestures", "Gestures", "Gestures", "bLeftHand", "bool", 0, "Left stick gestures",
                 "The left click is also sprint: sprint starts after the button delay.", apply="gesture reload"))
    for hand, hlabel in (("R", "Right"), ("L", "Left")):
        for name, text in SLOTS:
            g = f"{hlabel} stick: {text}"
            base = f"i{hand}{name}"
            R.append(row("Gestures", g, "Gestures", f"{base}Action", "enum", GESTURE_DEFAULTS.get((hand, name), 0), "Action", "",
                         options=GESTURE_ACTIONS, apply="gesture reload"))
            R.append(row("Gestures", g, "Gestures", f"{base}Key", "enum", 0, "Key", "Sent when the action is 'Key press'.",
                         options=["@keys"], apply="gesture reload"))
            R.append(row("Gestures", g, "Gestures", f"{base}Mod", "enum", 0, "Modifier", "", options=MODS, apply="gesture reload"))
    for key, dflt, label, mn, mx, st in [("fMoveCm", 11.0, "Movement threshold (cm)", 5.0, 30.0, 0.5),
                                         ("fDominance", 1.5, "Direction clarity", 1.0, 3.0, 0.1),
                                         ("fMaxHoldS", 2.0, "Longest press (s)", 0.5, 5.0, 0.1),
                                         ("fCooldownS", 0.3, "Cooldown (s)", 0.0, 2.0, 0.05),
                                         ("fButtonDelayS", 0.5, "Button delay (s)", 0.2, 2.0, 0.05),
                                         ("fKeyHoldMs", 100.0, "Key press length (ms)", 30.0, 500.0, 10.0),
                                         ("fHapticAmp", 0.5, "Pulse strength", 0.0, 1.0, 0.05),
                                         ("fHapticS", 0.06, "Pulse length (s)", 0.01, 0.3, 0.01)]:
        R.append(row("Gestures", "Detection", "Gestures", key, "float", dflt, label, "", mn, mx, st, apply="gesture reload"))
    R.append(row("Gestures", "Detection", "Gestures", "bHaptics", "bool", 1, "Haptic pulse", "", apply="gesture reload"))
    R.append(row("Gestures", "Detection", "Gestures", "bAllViews", "bool", 1, "Everywhere (views and menus)",
                 "Gestures everywhere: every camera view, menus, Pip-Boy, dialogue, VATS, workshop (0 = first-person gameplay only).",
                 apply="gesture reload"))
    # ---- Selfie ----
    R.append(row("Selfie", "Selfie", "Selfie", "bEnabled", "bool", 1, "Hands-up selfie", "Raising the hands toggles the mirror body.",
                 apply="gesture reload"))
    R.append(row("Selfie", "Selfie", "Selfie", "iHands", "enum", 2, "Hands to raise", "", options=["Off hand", "Main hand", "Both hands"],
                 apply="gesture reload"))
    R.append(row("Selfie", "Selfie", "Selfie", "bKeepInMenus", "bool", 1, "Keep in menus", "Pause, Pip-Boy and menus keep it.",
                 apply="gesture reload"))
    for key, dflt, label, mn, mx, st in [("fHandsUpCm", 25.0, "Height above the head (cm)", 10.0, 50.0, 1.0),
                                         ("fHoldS", 0.5, "Hold time (s)", 0.1, 2.0, 0.05),
                                         ("fDistanceM", 1.4, "Distance (m)", 0.6, 4.0, 0.05),
                                         ("fRollGain", 1.0, "Wrist turn gain", -3.0, 3.0, 0.1),
                                         ("fRollDeadzoneDeg", 0.0, "Wrist dead zone (deg)", 0.0, 20.0, 1.0)]:
        R.append(row("Selfie", "Selfie", "Selfie", key, "float", dflt, label, "", mn, mx, st, apply="gesture reload"))
    # ---- FRIK ----
    R.append(row("FRIK", "FRIK-flat", "FRIK", "bEnable", "bool", 1, "FRIK-flat", "FRIK.dll active at all.",
                 restart="FRIK.dll asks for the bridge and installs its hooks and code patches when it loads"))
    R.append(row("FRIK", "FRIK-flat", "FRIK", "bBody", "bool", 1, "FRIK body", "0 = smooth movement only.",
                 restart="FRIK.dll installs its body hooks and first-person arm patches and builds its skeleton when it loads; it has "
                         "no path back to the engine's own first-person body while running"))
    R.append(row("FRIK", "Movement", "FRIK", "bSmoothMovement", "bool", 1, "Smooth movement", "FRIK's camera height smoothing.",
                 apply="frik smooth {onoff}"))
    R.append(row("FRIK", "Movement", "FRIK", "bSmoothHorizontal", "bool", 0, "Smooth horizontally too",
                 "Fights roomscale; off = height only.", apply="frik smooth xy {onoff}"))
    R.append(row("FRIK", "Body", "FRIK", "bPausedFrame", "bool", 1, "Body runs while paused", "Pip-Boy, Pause, terminals.",
                 apply="frik paused {onoff}"))
    R.append(row("FRIK", "Body", "FRIK", "bAutoBodyHeight", "bool", 1, "Body height follows the camera", "", apply="@reload"))
    R.append(row("FRIK", "Body", "FRIK", "bBodyHeightOnRecentre", "bool", 1, "Re-seed body height on recentre",
                 "On: measured at the next recentre.", apply="@reload"))
    R.append(row("FRIK", "Body", "FRIK", "bLegChainIK", "bool", 1, "Leg IK over skeleton chains", "CBBE / DFS leg bones.", apply="@reload"))
    R.append(row("FRIK", "Body", "FRIK", "bBodyGraph", "bool", 1, "Body animation graph in first person",
                 "Runs the game's third-person locomotion (walk, run, strafe, sneak) on the FRIK body in first person. Needed by VRIK-style legs. Live.",
                 apply="frik reread"))
    R.append(row("FRIK", "Body", "FRIK", "bBodyGraphSpeedContour", "bool", 1, "Body graph: own walk speed",
                 "The body animation gets its own speed and the game's walk / run pacing (speed contour), as in third person. Off: it reuses the first-person value (walk cycle can play too slowly). Live.",
                 apply="frik reread"))
    # 20 Nov (VATS agent): C44 1st / 3rd person switch sync (FRIK-flat PovSwitchSync.h)
    R.append(row("FRIK", "Body", "FRIK", "bPovSwitchSync", "bool", 1, "Keep view switches in step with the body",
                 "VATS playback / third person switches run completely with the FRIK body shown (animated VATS body, VATS shots from the real muzzle, no stuck crouch height after VATS). Live.",
                 apply="frik reread"))
    R.append(row("FRIK", "Body", "FRIK", "bAnimatedLegs", "bool", 1, "VRIK-style legs",
                 "Moving: the legs follow the game's walk / run / strafe animation (needs Body animation graph). Standing: planted feet with short steps. Off: FRIK's walk.",
                 apply="frik reread"))
    R.append(row("FRIK", "Body", "FRIK", "bHeadlessFirstPerson", "bool", 1, "Headless first person", "", apply="@reload"))
    R.append(row("FRIK", "Body", "FRIK", "bRestoreEngineBody", "bool", 1, "Restore the engine body pose", "", apply="@reload"))
    R.append(row("FRIK", "Body", "General", "bNoScaling", "bool", 0, "Game's default scale (no scaling)",
                 "FRIK body at scale 1: no PlayerHeight, no auto body height, no arm-length scaling; 70 units per metre. Live.",
                 apply="frik noscale"))
    R.append(row("FRIK", "Body", "FRIK", "bFrikPipboy", "bool", 0, "FRIK's wrist Pip-Boy opening",
                 "Look at the wrist / FRIK's button opens the game's Pip-Boy (off: fo4flatvr's Pip-Boy panel only).",
                 apply="frik reread"))
    R.append(row("FRIK", "Body", "FRIK", "bConfigShortcut", "bool", 0, "FRIK's both-sticks config shortcut",
                 "FRIK's own config does not render on flat yet: leave off.", apply="@reload"))
    # ---- ROCK-flat (21 Nov, ROCK agent; analysis/rock-flat.md; keys read by fo4flatvr and by ROCK.dll through the bridge) ----
    R.append(row("FRIK", "ROCK-flat", "ROCK", "bEnable", "bool", 1, "ROCK-flat (physics hands)", "ROCK.dll active at all (needs FRIK.dll).",
                 restart="ROCK.dll decides at load whether it installs its hooks and patches and registers with FRIK-flat"))
    R.append(row("FRIK", "ROCK-flat", "ROCK", "bInputClaims", "bool", 1, "ROCK input claims",
                 "Inputs ROCK takes (grip to grab ...) are kept from the game, gestures and glyphs in gameplay; menus keep them. Live.",
                 apply="bindings claims {onoff}"))
    R.append(row("FRIK", "ROCK-flat", "ROCK", "bGrab", "bool", 1, "ROCK grabbing",
                 "ROCK's hands grab, pull and catch objects (grip). Off: no new grab starts and the grip goes back to the game; a held object stays until released. Live.",
                 apply="@reload"))
    # ---- Keys + the panel ----
    for key, dflt, label in [("iRecenter", 0x77, "Recentre"), ("iCameraCycle", 0x71, "Camera view cycle"),
                             ("iPovToggle", 0x72, "First / third person"), ("iToggle", 0x79, "VR submission on/off"),
                             ("iConfigMenu", 0x71, "VR settings panel (with the modifier)")]:
        R.append(row("Keys", "Keyboard (game window focused)", "Keys", key, "vk", dflt, label,
                     "F9 is the game's Quickload, F5 Quicksave." if key == "iRecenter" else "", options=["@vk"],
                     apply="@present", mcm=False))
    R.append(row("Keys", "Keyboard (game window focused)", "Keys", "iConfigMenuMod", "enum", 2, "Panel key modifier", "",
                 options=MODS, apply="@configui"))
    R.append(row("Keys", "VR settings panel", "ConfigUI", "bMenuHold", "bool", 1, "Hold the left menu button to open",
                 "A tap still opens Pause (on release).", apply="@configui"))
    R.append(row("Keys", "VR settings panel", "ConfigUI", "fMenuHoldS", "float", 1.0, "Menu button hold (s)", "", 0.5, 3.0, 0.1,
                 apply="@configui"))
    R.append(row("Keys", "VR settings panel", "ConfigUI", "fDistanceM", "float", 0.7, "Panel distance (m)", "", 0.4, 1.5, 0.05,
                 apply="@configui"))
    R.append(row("Keys", "VR settings panel", "ConfigUI", "fWidthM", "float", 0.9, "Panel width (m)", "", 0.5, 1.6, 0.05,
                 apply="@configui"))
    R.append(row("Keys", "VR settings panel", "ConfigUI", "fHeightM", "float", -0.12, "Panel height (m)", "Centre below (-) eye level.",
                 -0.6, 0.3, 0.02, apply="@configui"))
    R.append(row("Keys", "VR settings panel", "ConfigUI", "bHaptics", "bool", 1, "Panel haptics", "", apply="@configui"))
    return R
