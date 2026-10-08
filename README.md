# GravityControl

Arbitrary gravity direction for **CONTROL Resonant**. Turn gravity toward the wall in front of you
or the ceiling above you and walk there, anywhere in the game, not only inside its Gravity Anomaly
zones. The mod feeds the game's own anomaly transition, so the camera turn, the character re-pose
and the collision fix-up are the game's own.

- **Shift**: tap Right Shift or d-pad up. The closer surface ahead or above wins.
- **Reset**: hold the same key or button for a second.
- **Chorded activation** (optional, off by default): hold LB and tap X, or hold Shift and tap V. All
  four inputs can be changed.
- Needs the Gravity Anomaly ability unlocked in your save. Does nothing while a menu is open.
- Xbox, DualSense and DualShock 4 controllers work.
- With Mod Settings Menu installed, every setting is under Options > MODS, with an Enable switch.

## Install

Grab `GravityControl.zip` from the [releases](../../releases) and extract it into the game folder
(next to `CONTROLResonant.exe`). Everything lands in `crmods\GravityControl\`. It needs crloader,
the `winmm.dll` mod loader, which you already have if you run other DLL mods. Full details are in
[README-GravityControl.txt](README-GravityControl.txt).

Upgrading from 1.0: just extract. On its first start the mod takes over from the old flat files in
`crmods\` and renames them with `.old` on the end. Nothing is deleted. The same goes for any older
copy elsewhere in `crmods`: the newest copy runs, and the older one's DLL and menu file get `.old`.

Checked on Steam build 25472515 and on game versions 1.4.0 and 1.4.1. After a later update the
mod still runs if the code it needs is unchanged. It shows a message only when an update stops the
mod, or part of it, from working: the message names the mod and game versions, the game function that
failed and the likely cause. It works next to other mods that hook the same game functions.

## Build

Visual Studio 2022 (MSVC, x64). From a terminal:

```bat
build.bat
```

It compiles `gravitycontrol.cpp` with the vendored [MinHook](https://github.com/TsudaKageyu/minhook)
into `out\gravitycontrol.dll`. Copy the DLL, `gravitycontrol_config.ini` and
`gravitycontrol.menu.json` into `crmods\GravityControl\` in the game folder.

`tests\build_padtest.bat` builds `out\padtest.exe`, which checks the PlayStation controller report
layouts in `sonypad.h` and prints the buttons it reads from a connected pad.
`tests\build_chordtest.bat` builds `out\chordtest.exe`, which checks the timing rules of chorded
activation in `chord.h`.

## How it works

The game turns gravity for its anomaly walls by slerping the player's `MovementPlane` quaternion
and re-posing the character controller. GravityControl hooks that pipeline and feeds it a target of
its own: a sweep with the engine's own query helper finds the surface, its normal becomes the new
"up" (snapped to a world axis), and the game's interpolation, camera and capsule fix-up run exactly
as inside an anomaly. A transition the game starts itself (an anomaly surface, a Reach point, a
respawn) always takes the plane back, and a shift tapped while one is running is skipped.
Hook points are found by byte pattern in the game executable at start; if a
game update moves them beyond recognition the mod stays inactive and says so. A function another mod
hooked first is found behind that hook, and both hooks run.

## Credits

Built with [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD-2-Clause,
vendored from upstream commit 8af6b4a; see `minhook/LICENSE.txt`, the notices are also embedded in the DLL). Loaded by crloader.
