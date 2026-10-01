# GravityControl

Arbitrary gravity direction for **CONTROL Resonant**. Turn gravity toward the wall in front of you
or the ceiling above you and walk there, anywhere in the game, not only inside its Gravity Anomaly
zones. The mod feeds the game's own anomaly transition, so the camera turn, the character re-pose
and the collision fix-up are the game's own.

- **Shift**: tap Right Shift or d-pad up. The closer surface ahead or above wins.
- **Reset**: hold the same key or button for a second.
- Needs the Gravity Anomaly ability unlocked in your save. Does nothing while a menu is open.

## Install

Grab `GravityControl.zip` from the [releases](../../releases) and extract it into the game folder
(next to `CONTROLResonant.exe`). It needs crloader,
the `winmm.dll` mod loader, which you already have if you run other DLL mods. Full details are in
[README-GravityControl.txt](README-GravityControl.txt).

Made for Steam build 25472515. On another game version the mod warns once at start and runs anyway.

## Build

Visual Studio 2022 (MSVC, x64). From a terminal:

```bat
build.bat
```

It compiles `gravitycontrol.cpp` with the vendored [MinHook](https://github.com/TsudaKageyu/minhook)
into `out\gravitycontrol.dll`. Copy the DLL and `gravitycontrol_config.ini` into the game's
`crmods\` folder.

## How it works

The game turns gravity for its anomaly walls by slerping the player's `MovementPlane` quaternion
and re-posing the character controller. GravityControl hooks that pipeline and feeds it a target of
its own: a sweep with the engine's own query helper finds the surface, its normal becomes the new
"up" (snapped to a world axis), and the game's interpolation, camera and capsule fix-up run exactly
as inside an anomaly. Hook points are found by byte pattern in the game executable at start; if a
game update moves them beyond recognition the mod stays inactive and says so.

## Credits

Built with [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD-2-Clause,
vendored from upstream commit 8af6b4a; see `minhook/LICENSE.txt`, the notices are also embedded in the DLL). Loaded by crloader.
