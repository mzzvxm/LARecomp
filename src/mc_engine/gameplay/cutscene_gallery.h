#pragma once
//
// Cutscene gallery: replay any of the game's cutscenes on demand from the
// pause menu.
//
// RE map (default.xex + xarchive_cache.rpf, read with tools/sco.py):
//
//   A cutscene is two assets and one script.
//
//     $/resources/animation/cineanimpacks/<scene leaf>.xapk   animation pack
//     $/script_obj/Game/CineScripts/generated[/Story|/hangout|/RaceStart]/
//         <scene leaf>_generated.sco                          the scene itself
//
//   The generated script derives from
//   $/script_obj/Game/CineScripts/source/core_rolling_prototype, and that base
//   class is the whole cutscene runtime -- disassembled, it runs:
//
//     CineScript_DoesCutsceneAnimPackExist / StartLoadCutsceneAnimPack
//     Characters_LoadAnimationWithFace / Cars_LoadMoverAnimation
//     Camera_LoadAnimation / Audio_PrepareStream
//     wait on IsLoadedCutsceneAnimPack, Cars_RegisterRacer, Prop_LoadSetDressing
//     SetSceneReadyForGame -> IsGameReadyForScene
//     BeginDescription -> Characters_LaunchAnimEventWithFace,
//         Cars_LaunchEvent, Camera_LaunchEvent -> EndDescriptionAndStartClock
//     Audio_PlayStream, transition movie up/down
//     Camera_Kill, PopKillBuffer, FinishScene
//
//   So the script loads its own assets, plays, and puts the game back. Nothing
//   outside it has to be set up, which is what makes a replay menu possible at
//   all: starting the thread is the entire operation.
//
//   The derived script opens by naming its scene, e.g. cut_intro_generated.sco
//   at offset 133:
//
//       PUSH_STR  'story/cut_intro'
//       CALL      argc=1 retc=0 7ACED201  CineScript_SetSceneName
//
//   7ACED201 is scrThread's hash of the native name (sub_82557F78: Jenkins
//   one-at-a-time, lowercased, stored little endian).
//
//   Threads are started by mcScriptManager::StartNewThread, sub_82727668:
//
//       r3 manager (dword_8290A9F8)   r4 script name, relative to $\script_obj
//       r5 parent thread, 0 = the manager's root thread
//       r6 -> thread+41                r7,r8 -> mcScript::Load args
//       r9 stack size, must be 300 / 1500 / 3800 or it is forced to 1500
//
//   sub_82727908 is the wrapper that supplies the manager. The game itself
//   calls it exactly this way from native code in sub_822C4BD8:
//
//       sub_82727908("titlestorage/tms_boot", 0, 1, 0, 0, 1500)
//
//   which is the call shape reproduced here.
//
// Everything runs on the game thread, off Patch_DeltaTimePre: the script
// manager is not thread safe, and the pause menu has to be off screen before
// the scene starts -- core_rolling_prototype's load loop polls Game_IsPaused
// (sub_822BF228) and gives up after ten seconds.
//

#include <cstdint>

// Cutscene table, exposed so the pause menu can build a value row over it.
int  CutsceneCount();
const char* CutsceneId(int index);     // cvar value, e.g. "cut_intro"
const char* CutsceneLabel(int index);  // row text, e.g. "INTRO"

// The same two columns as flat arrays, which is the shape the pause menu's
// value rows want (svals / slabels).
const char* const* CutsceneIds();
const char* const* CutsceneLabels();

// Index of the cutscene the `cutscene_replay` cvar currently names, or 0.
int  CutsceneSelectedIndex();

// Arm a replay of the selected cutscene. Safe to call from a menu click: the
// work happens on the next few frames, from TickCutsceneGallery().
void CutsceneRequestPlay();

// True while a replay is being set up or is on screen. The pause-menu row
// uses it for its label.
bool CutsceneReplayBusy();

// Per-frame, driven from Patch_DeltaTimePre() in hooks.cpp.
void TickCutsceneGallery();
