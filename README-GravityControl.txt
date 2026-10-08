GravityControl 1.3.1 - arbitrary gravity direction for CONTROL Resonant
=======================================================================

Allows free use of the Gravity Anomaly ability on-demand after acquiring it,
not just in designated zones.

REQUIRES
  crloader (the winmm.dll mod loader) in the game folder. If you run other DLL mods, you have it.
  Optional: Mod Settings Menu, to change the settings in game under Options > MODS.

INSTALL
  1. Extract this archive into the game folder (next to CONTROLResonant.exe). Everything goes
     into one folder:
       crmods\GravityControl\
  2. Upgrading from 1.1 or later: just extract and overwrite. If an older copy of the mod is
     installed somewhere else in crmods, the newest copy runs and the older one's DLL and menu
     file are renamed with .old on the end.
     Upgrading from 1.0: just extract. On its first start the mod takes over from the old files
     in crmods\ and renames them with .old on the end. Nothing is deleted; remove the .old
     files when you like. Settings from the old ini are not carried over.
  3. Start the game. Once the Gravity Anomaly ability is unlocked in your save, tap Right Shift
     or d-pad up.
  Checked on Steam build 25472515 and on game versions 1.4.0 and 1.4.1. After a later game
  update the mod still runs if the code it needs is unchanged, and shows no message. If an
  update stops the mod, or part of it, from working, a message at start says what is off.
  Works next to other mods that hook the same game functions.

UNINSTALL
  Delete the crmods\GravityControl folder.

CONTROLS
  Shift   tap Right Shift or d-pad up
  Reset   hold Right Shift or d-pad up for one second
  Both can be changed under Options > MODS or in gravitycontrol_config.ini.
  Chorded activation (off by default): hold LB / L1 and tap X / Square, or hold Shift and tap V,
  instead of the single key and button. Switch it on and change the four inputs in the same places.
  Xbox, DualSense and DualShock 4 controllers work, with or without Steam Input.

HOW IT PLAYS
  Shift looks ahead and straight up; the closer surface within 5 m wins and gravity turns toward
  it, so you land on the wall you face or flip onto the ceiling above you. With nothing there, it
  probes past the edge in front of you, or simply turns gravity to your facing (roll forward over
  an edge you just crested). The new "up" is always a world axis.
  Reset returns to normal gravity. Dying resets it too. The game's own anomaly surfaces and Reach
  points keep working while gravity is shifted: a transition the game starts takes over, and a
  shift tapped while it is still running is skipped.
  The keys do nothing while a game menu is open, and nothing at all until the save has the
  Gravity Anomaly ability.

SETTINGS
  crmods\GravityControl\gravitycontrol_config.ini, commented, reloaded while the game runs.
  With Mod Settings Menu, the same settings are under Options > MODS > GravityControl, with an
  Enable switch at the top. Its header shows the version and whether the mod is running.
  A value changed in the menu wins over the ini.
  [Blockers] is experimental and off; its comment says why to leave it alone.

LOG
  crmods\GravityControl\gravitycontrol.log, rewritten every start. Every shift logs what the
  ray found. If a controller button does not respond, turn on Diagnostics, press the button a
  few times in game, and send this log.

THIRD-PARTY NOTICES
  gravitycontrol.dll contains MinHook and its Hacker Disassembler Engine. Their license requires
  the following notices with any binary distribution; they are also embedded in the DLL.

  MinHook - The Minimalistic API Hooking Library for x64/x86
  Copyright (C) 2009-2017 Tsuda Kageyu.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

   1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  ================================================================================
  Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
  ================================================================================
  Hacker Disassembler Engine 32 C
  Copyright (c) 2008-2009, Vyacheslav Patkov.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

   1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  -------------------------------------------------------------------------------
  Hacker Disassembler Engine 64 C
  Copyright (c) 2008-2009, Vyacheslav Patkov.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

   1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
