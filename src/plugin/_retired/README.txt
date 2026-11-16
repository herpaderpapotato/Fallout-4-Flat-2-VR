Retired 14 Nov 2026 (work/handoff/fo4vr-baseline-hands.md): not built (xmake.lua excludes _retired/).
The hands code went beyond the Fallout 4 VR baseline (floating gun only) into features FRIK / ROCK provide:
arm IK (FRIK solveArmToHandWorldTarget port), body under the HMD, free off-hand fingers, two-handed grip, kept firing
recoil, wrist Pip-Boy on PipboyBone, and moving the first-person arm rig with the gun (stage 1, COM).
The FO4VR-baseline floating gun that replaced it is plugin/src/plugin/FloatingGun.{h,cpp} ([Weapon] bFloatingGun=1).
Files: Hands.cpp, HandsInput.cpp, Hands.h, HandsInternal.h, HandsFingerData.inl (unchanged copies of the last build).
