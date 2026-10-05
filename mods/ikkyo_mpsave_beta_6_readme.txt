Ikkyo's Multiplayer Quest Save Beta 6
By Ikkyo (jgripp@budget.net)
November 1, 2003
=====================================

I. Introduction

  Ikkyo's Multiplayer Quest Save is a modification for Dungeon Siege(TM) by Gas
  Powered Games, Inc.(TN).

  The one thing that annoyed me about the multiplayer version of the Ehb map
  was that the current states of the quests were not saved between game
  sessions. With this modification, that annoyance no longer exists. Also,
  starting with Beta 2, the Utraean Peninsula map is supported as well.

  The state of all quests and quest related events on both the Kingdom of Ehb
  and Utraean Peninsula maps are now saved using QuestBits so that one can pick
  up right where he/she left off.  For example, quest related doors, like the
  Glitterdelve Gate, will remain open if you complete the quest in a previous
  game. Also, most quest rewards are only given out once.  I hope to still make
  the rewards worth while though.

  All quest are saved in the journal, this gives a multiplayer character a much
  greater sense of accomplishment when all the quest have been completed.
  I hope to integrate veteran and elite modes for Ehb into this modification
  sometime as well.


II. Installation and Usage

  To install this modification, copy the ikkyo_mpsave_beta_6.dsres file into
  your Dungeon Siege/Resources folder. 
  (Usually C:/Program Files/Microsoft Games/Dungeon Siege/Resources)

  Make sure to remove any previous version of this mod when upgrading from an
  older version.

  This modification starts working immediately after installing it. Quests are
  automatically saved periodically (every 60 seconds) and when you exit the
  game.  There's no extra interaction required from the user.  To be extra
  safe, the user can click the "Save Quests" button on the game menu.

  The "Reset Quests" button will erase all quest state information stored in
  your character file.  This can only be done from the host of the game, and
  will only affect the character who clicks on it.


III. Removal

  To remove this modification, delete the ikkyo_mpsave_beta_6.dsres file.


IV. Compatibility Issues

  This mod will conflict with other mods that modify the base templates.  I
  don't know which ones, but be warned.  This mod should be compatible with all
  version of Dungeon Siege.  However, I have only tested it with 1.1+ so this
  may not be the case.  There are no conflicts for characters brought in from
  "normal" Dungeon Siege games, quests will just be uncompleted initially.

  All players need a copy of this modification to play in others games.
  Characters can continue to play in unmodified Dungeon Siege games with no
  adverse side-effects.


V. Changelog

  Beta 6
  ==========
  -Fixed Glitterdelve Gate quest load bug

  Beta 5
  ==========
  -Quests save less often
  -Quests automatically save when you press the exit button
  -Fixed SiegeEditor UI errors

  Beta 4
  ==========
  -Quest states are now only loaded from the host player
  -Quests will no longer revert to earlier states
  -Quest states should now save probably on clients machines
  -fixed instances of npc's becoming stuck in a conversation
  -improved the first time quest completion rewards for certain UP quests

  Beta 3
  ==========
  -Fixed the crash bug related to the overseer quest
  -Optimized the code a little

  Beta 2
  ==========
  -Added initial support for The Utraean Peninsula
  -Merik Staff quest in now undone if the staff is lost
  -Will now work with Dwarf characters
  -Should sync better with multiple players

  Beta 1
  ==========
  -initial release
  -all quest saved


VI. Information about the Author

  Jason 'Ikkyo' Gripp
  email: jgripp@budget.net

  I'm just a freelance coder. My biggest project has probably been Desert
  Crisis (www.desertcrisis.com), a Half-Life mod. There I am the lead/only
  coder.

  Send any bugs/issues or suggestions to me via email.  You may also contact me
  on the siegeworks.org message boards if you wish.  I haven't tested the whole
  game yet, so the comments will be appreciated.


VII. Legal Rhetoric

  This document and associated data files are 
  Copyright 2003 by Jason 'Ikkyo' Gripp.  All rights reserved.  

  The author grants permission for this work to be copied or otherwise
  transmitted, with the provision that the work remain unaltered and that the
  distribution be done free of charge.  All code contained herein is the
  intellectual property of the author (and, in specifically noted cases, of Gas
  Powered Games, Inc., and other entities).

  Dungeon Siege is a registered trademark of Gas Powered Games, Inc. and
  Microsoft, Inc.

Thanks to Icemage for the legal rhetoric.