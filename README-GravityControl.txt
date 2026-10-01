GravityControl 1.0 - arbitrary gravity direction for CONTROL Resonant
=====================================================================

Allows free use of the Gravity Anomaly ability on-demand after acquiring it,
not just in designated zones.

REQUIRES
  crloader (the winmm.dll mod loader) in the game folder. If you run other DLL mods, you have it.

INSTALL
  1. Extract this archive into the game folder (next to CONTROLResonant.exe):
       crmods\gravitycontrol.dll
       crmods\gravitycontrol_config.ini
  2. Start the game. Once the Gravity Anomaly ability is unlocked in your save, tap Right Shift
     or d-pad up.
  Made for Steam build 25472515. On another game version the mod warns once at start and runs
  anyway; if the game changed too much for it, it stays inactive and says so.

UNINSTALL
  Delete crmods\gravitycontrol.dll, crmods\gravitycontrol_config.ini and crmods\gravitycontrol.log.

CONTROLS
  Shift   tap Right Shift or d-pad up
  Reset   hold Right Shift or d-pad up for one second
  Both can be changed in gravitycontrol_config.ini.

HOW IT PLAYS
  Shift looks ahead and straight up; the closer surface within 10 m wins and gravity turns toward
  it, so you land on the wall you face or flip onto the ceiling above you. With nothing there, it
  probes past the edge in front of you, or simply turns gravity to your facing (roll forward over
  an edge you just crested). The new "up" is always a world axis.
  Reset returns to normal gravity. Dying resets it too. Walking into one of the game's own anomaly
  walls hands control to the game; leaving it returns to normal as usual.
  The keys do nothing while a game menu is open, and nothing at all until the save has the
  Gravity Anomaly ability.

SETTINGS
  crmods\gravitycontrol_config.ini, commented, reloaded while the game runs.
  [Blockers] is experimental and off; its comment says why to leave it alone.

LOG
  crmods\gravitycontrol.log, rewritten every start. Every shift logs what the ray found.

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
