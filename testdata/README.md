# testdata/

Copied by the exporter from the workspace machine's live setup (read only there):

| Path | What | Goes to (test machine) |
|---|---|---|
| `saves/vrtest_sanctuary.fos/.f4se`, `saves/vrtest_container.fos/.f4se` | the test saves the harness and session scripts load (`load vrtest_sanctuary`) | `%LOCALAPPDATA%\ModOrganizer\Fallout 4\profiles\TestProfile\saves\` (profile-local saves) |
| `mo2-profile/TestProfile/*` | the MO2 test profile: mod list, plugin/load order, archives, profile-local game inis (`LocalSaves=true`, `LocalSettings=true`) | `%LOCALAPPDATA%\ModOrganizer\Fallout 4\profiles\TestProfile\` (create the profile in MO2 first, then copy over) |

`fallout4prefs.ini` carries this machine's window size (3440x1440 borderless); adjust `iSize W/H` for the test
machine's monitor. Keys that differ from a stock prefs file and matter here: `bAlwaysActive=1` (also in
`fallout4custom.ini` [General]; without it the game stops presenting when unfocused), `bComputeShaderDeferredTiledLighting=0`,
`bMBEnable=0`, `bDialogueCameraEnable=0`. (The plugin can also force the last three at run time: CameraTweaks /
StereoCuts.)
