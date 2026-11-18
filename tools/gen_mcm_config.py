"""Generate fo4flatvr's MCM menu (Mod Configuration Menu) from the gesture code.

Writes plugin/dist/Data/MCM/Config/fo4flatvr/config.json and settings.ini (MCM's defaults; every ModSetting must be
listed there). The key dropdown's names come from Gestures.cpp (between "MCM-KEYS-BEGIN" / "MCM-KEYS-END"), so the
stored index always matches the plugin's table. Run: python -I plugin/tools/gen_mcm_config.py

MCM stores changed values in Data/MCM/Settings/fo4flatvr.ini (under MO2: overwrite/MCM/Settings); the plugin re-reads
that file when its time stamp changes (Gestures.h).
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(HERE)
SRC = os.path.join(PLUGIN, "src", "plugin", "Gestures.cpp")
OUT = os.path.join(PLUGIN, "dist", "Data", "MCM", "Config", "fo4flatvr")

SLOTS = [  # (ini name, label) in VRIK's order (Gestures.h)
    ("Tap", "Press (no movement)"),
    ("Up", "Up"), ("Down", "Down"), ("Left", "Left"), ("Right", "Right"), ("Back", "Back (toward you)"),
    ("Fwd", "Forward"),
    ("UpDown", "Up, then down"), ("DownUp", "Down, then up"), ("LeftRight", "Left, then right"),
    ("RightLeft", "Right, then left"), ("BackFwd", "Back, then forward"), ("FwdBack", "Forward, then back"),
]
ACTIONS = ["None", "Key press", "Recentre view", "First / third person", "Camera view cycle (SCENE / TRACK / CINEMA)"]
MODS = ["No modifier", "Shift", "Ctrl", "Alt"]


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


def switch(id_, text, help_):
    return {"id": id_, "text": text, "type": "switcher", "help": help_, "valueOptions": {"sourceType": "ModSettingBool"}}


def dropdown(id_, text, help_, options):
    return {"id": id_, "text": text, "type": "dropdown", "help": help_,
            "valueOptions": {"sourceType": "ModSettingInt", "options": options}}


def slider(id_, text, help_, mn, mx, step):
    return {"id": id_, "text": text, "type": "slider", "help": help_,
            "valueOptions": {"min": mn, "max": mx, "step": step, "sourceType": "ModSettingFloat"}}


def section(text):
    return {"text": text, "type": "section"}


def note(text):
    return {"text": text, "type": "text"}


def hand_page(hand, label, keys, ini):
    content = [note(f"{label} stick click + hand movement. Each gesture does nothing until you pick an action. "
                    "'Key press' sends the chosen key (with an optional modifier) to the game while its window has the focus, "
                    "so it can trigger any game key, any MCM hotkey of another mod, or fo4flatvr's own keys "
                    "(F2 camera view cycle, F3 first/third person, F8 recentre).")]
    for name, text in SLOTS:
        base = f"i{hand}{name}"
        content.append(section(text))
        content.append(dropdown(f"{base}Action:Gestures", "Action", f"What the {label.lower()} '{text}' gesture does.", ACTIONS))
        content.append(dropdown(f"{base}Key:Gestures", "Key", "The key sent when the action is 'Key press'.", keys))
        content.append(dropdown(f"{base}Mod:Gestures", "Modifier", "Held together with the key (as an MCM hotkey's modifier).", MODS))
        ini += [f"{base}Action=0", f"{base}Key=0", f"{base}Mod=0"]
    return {"pageDisplayName": f"{label} stick gestures", "content": content}


def main():
    keys = key_names()
    ini_g, ini_s = [], []
    settings = {"pageDisplayName": "Gestures", "content": [
        note("VRIK-style gestures: hold a stick click, move the hand, release. Directions follow where your head faced "
             "when you clicked. A press without movement is 'Press'; out and back again is a two-way gesture. "
             "The right stick click no longer opens the favourites menu."),
        section("Gestures"),
        switch("bEnabled:Gestures", "Gestures", "Master switch. Off: no gesture runs and the sticks behave as without gestures."),
        switch("bRightHand:Gestures", "Right stick gestures", "Gestures with the right stick click."),
        switch("bLeftHand:Gestures", "Left stick gestures",
               "Gestures with the left stick click. The left click is also sprint: with this on, sprint starts once the "
               "click is held still for the button delay (as VRIK's button delay)."),
        section("Detection"),
        slider("fMoveCm:Gestures", "Movement threshold (cm)", "How far the hand must move for a direction (VRIK: about 11 cm).", 5.0, 30.0, 0.5),
        slider("fDominance:Gestures", "Direction clarity",
               "The main direction must be this many times larger than any other, else the gesture is ignored (1 = VRIK, no check).",
               1.0, 3.0, 0.1),
        slider("fMaxHoldS:Gestures", "Longest press (s)", "A press held longer than this is not a gesture.", 0.5, 5.0, 0.1),
        slider("fCooldownS:Gestures", "Cooldown (s)", "After a gesture fires, new presses are ignored for this long.", 0.0, 2.0, 0.05),
        slider("fButtonDelayS:Gestures", "Button delay (s)",
               "Left stick gestures only: holding the click still this long passes it to the game (sprint).", 0.2, 2.0, 0.05),
        slider("fKeyHoldMs:Gestures", "Key press length (ms)", "How long an emulated key is held down.", 30.0, 500.0, 10.0),
        section("Haptics"),
        switch("bHaptics:Gestures", "Haptic pulse", "A pulse on the gesture hand when a gesture fires (and on selfie on/off)."),
        slider("fHapticAmp:Gestures", "Pulse strength", "0..1", 0.0, 1.0, 0.05),
        slider("fHapticS:Gestures", "Pulse length (s)", "", 0.01, 0.3, 0.01),
    ]}
    ini_g += ["bEnabled=1", "bRightHand=1", "bLeftHand=0", "fMoveCm=11.0", "fDominance=1.5", "fMaxHoldS=2.0", "fCooldownS=0.3",
              "fButtonDelayS=0.5", "fKeyHoldMs=100.0", "bHaptics=1", "fHapticAmp=0.5", "fHapticS=0.06"]
    right = hand_page("R", "Right", keys, ini_g)
    left = hand_page("L", "Left", keys, ini_g)
    selfie = {"pageDisplayName": "Selfie", "content": [
        note("VRIK's selfie mode with FRIK's body: raise your hands above your head to put a mirror image of your body in "
             "front of you. While your hands stay up, roll your wrist to turn it. Raise them again to end it. It also ends "
             "on any menu or when the camera leaves first person. Needs FRIK-flat's body."),
        section("Selfie"),
        switch("bEnabled:Selfie", "Hands-up selfie", "Raising the hands toggles the mirror body."),
        dropdown("iHands:Selfie", "Hands to raise", "Which hands must be raised (VRIK selfieModeHands). The wrist that turns the "
                 "body is the off hand for 'Off hand', else the main hand.", ["Off hand", "Main hand", "Both hands"]),
        slider("fHandsUpCm:Selfie", "Height above the head (cm)", "How far above the headset the hands must be (VRIK: about 26 cm).", 10.0, 50.0, 1.0),
        slider("fHoldS:Selfie", "Hold time (s)", "How long the hands must stay up before it toggles.", 0.1, 2.0, 0.05),
        slider("fDistanceM:Selfie", "Distance (m)", "How far in front of you the body stands (VRIK: 1.4 m).", 0.6, 4.0, 0.05),
        slider("fRollGain:Selfie", "Wrist turn gain", "Body turn per wrist turn (1 = the same angle, as VRIK; negative = reversed).", -3.0, 3.0, 0.1),
        slider("fRollDeadzoneDeg:Selfie", "Wrist dead zone (deg)", "Wrist turns smaller than this are ignored (VRIK: none).", 0.0, 20.0, 1.0),
    ]}
    ini_s += ["bEnabled=1", "iHands=2", "fHandsUpCm=25.0", "fHoldS=0.5", "fDistanceM=1.4", "fRollGain=1.0", "fRollDeadzoneDeg=0.0"]
    cfg = {
        "modName": "fo4flatvr",
        "displayName": "VR (fo4flatvr)",
        "minMcmVersion": 2,
        "content": [note("fo4flatvr: flat Fallout 4 in VR. Gesture and selfie settings apply immediately (re-read within a second).")],
        "pages": [settings, right, left, selfie],
    }
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "config.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(cfg, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    with open(os.path.join(OUT, "settings.ini"), "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write("; MCM defaults for fo4flatvr (generated by plugin/tools/gen_mcm_config.py - edit that, not this).\n")
        fh.write("; Your changes are stored by MCM in Data\\MCM\\Settings\\fo4flatvr.ini.\n")
        fh.write("[Gestures]\n" + "\n".join(ini_g) + "\n\n[Selfie]\n" + "\n".join(ini_s) + "\n")
    print(f"wrote {OUT}: {len(keys)} keys, {len(ini_g) + len(ini_s)} settings")


if __name__ == "__main__":
    sys.exit(main())
