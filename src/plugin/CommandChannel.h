#pragma once
// Named pipe \\.\pipe\fo4flatvr: one text command per line, one reply line per command.
// Lets scripts/agents drive the running game without a person at the keyboard.
//
//   ping                 -> pong
//   status               -> presenter/XR status line (refreshed every 30 frames)
//   pad k=v ... ms=<n>   -> virtual gamepad override for n ms (lx ly rx ry lt rt a b x y lb rb ls rs
//                           start back up down left right), e.g. "pad ly=1 ms=800" walks forward
//   rt                   -> engine render target sizes (properties + D3D textures)
//   counters             -> live presents/xrFrames + QPC time, for rate measurements
//   ready                -> ingame=0|1 loading=.. mainmenu=.. cell=.. camstate=.. (poll this, don't sleep)
//   background           -> game window to the bottom of the z-order (no focus change)
//   console <command>    -> queued on the main thread (e.g. "console coc QASmoke")
//   capture <path.bmp>   -> next presented frame written as BMP
//   recenter             -> XR recentre on next frame
//   mode cinema|mono|afr -> flat quad / mono world view / alternate-eye stereo world view
//   pose <y> [p] [r]     -> scripted head rotation in degrees (overrides the HMD); "pose off"
//   posepos <x> [y] [z]  -> scripted head offset in cm (right, forward, up)
//   track on|off         -> apply head pose to the game camera
//   posemode 0|1         -> debug: composition order base*head (0) or head*base (1)
//   hud [on|off|src 3e|src 3f|premul 0|1|crosshair 0|1] -> separated HUD (D1) switch / status line
//   hudcap <path.bmp>    -> next Present: HUD copies as BGRA BMPs with alpha (<path>, <stem>_3e/_3f.bmp) + stats in log/"hud"
//   pipboy [on|off|src 3c|src 3d] -> wrist Pip-Boy (D3): state, capture counts, quad pose, pointer uv, page/tab
//   pipcap <path.bmp>    -> next Present: Pip-Boy screen copies (<path>, <stem>_3c/_3d.bmp) + stats in log/"pipboy"
//   menus                -> UI menu stack: name flags renderer depth (main thread)
//   rs [on|off|reset|hist|cfg <key> <value>] -> roomscale/body-yaw readout (one line), switch, history
//   gun [on|off|arms [on|off]|dump|cfg <key> <value>] -> floating gun (FloatingGun.h): readout, switch, arm hiding, rig dump
//   stereo [0..4]        -> single-pass stereo PoC mode (0 off, 1 census, 2 debug stereo, 3 faithful, 4 identity) / status
//   stereoskip|stereotest 0|1, stereoipd <m>, stereofov <halfDeg>, stereojitter 0|1, stereodepth 0|1 -> PoC test knobs
//   stereocap <path.bmp> -> GBuffer RT0 after the next frame's converted (census: candidate) draws + .json sidecar
//   stereoeyecap <stem>  -> next stereo frame: <stem>_bb.bmp (back buffer) + <stem>_eye0/1.bmp (submitted XR eye images)
//   stereostat           -> stereo status line (per-frame draw statistics go to the log every iLogEveryFrames)

namespace fo4flatvr::command
{
    void Start();
}
