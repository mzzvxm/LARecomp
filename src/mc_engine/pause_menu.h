#pragma once

#include <rex/ppc/context.h>

void InitPauseMenuHooks();

// Pop one level off the VirtualHSM navigation stack -- the same call the B
// button makes. The cutscene gallery uses it to walk the pause menu off screen
// before it starts a scene, since the game stays paused while any of it is up.
bool PauseMenuPopStack();

// True while the UI OPTIONS menu is the top form (the one the pad goes to).
bool PauseMenuUiOptionsOpen();

// The color buttons (mcla_menu::Color rows) of the RexGlue menu on top, for
// the swatches ui_palette.cpp draws beside them. Guest thread.
struct PauseMenuColorRows {
    static constexpr int kMax = 8;
    int count = 0;
    int row[kMax] = {};           // list row of each button
    const char* cvar[kMax] = {};  // its cvar
    int value[kMax] = {};         // 0 = the game's own color, 1.. = swatch
    int values = 0;               // how many values a button has
    int open = -1;                // the button whose strip is open, or -1
    int scroll = 0;               // first list row on screen
};
// False when the top form is not a RexGlue menu with color buttons.
bool PauseMenuColorButtons(PauseMenuColorRows* out);

// Close every RexGlue menu that is open, top first, the way B would. The
// cutscene gallery calls this when it unpauses the game to play a scene, so no
// menu of ours is left on the open-form stack holding the pad.
void PauseMenuLeaveSubmenu();

// Once per UI update (swf_dom.cpp), guest thread: closes our menus if the
// pause screen they were opened over goes away under them.
void PauseMenuFormsTick();

void Hook_CapturePMContinue(PPCRegister& r3);
bool Hook_EnablePMSave(PPCRegister& r3, PPCRegister& r4);
bool Hook_EnablePMTeste(PPCRegister& r3, PPCRegister& r4);
bool Hook_PMLodTrafficClick(PPCRegister& r3, PPCRegister& r31);
bool Hook_PopulateRedirect(PPCRegister& r3);
bool MCLA_MenuListNullTableGuard(PPCRegister& r30);

// Skip a vfunc call through a menu list whose vtable is null -- see the comment
// on the definition. Guards the crash that took the game down when the pause
// menu was reopened after a cutscene replay.
bool MCLA_PauseListVtableGuard(PPCRegister& r31);
bool Hook_RexGlueCancel(PPCRegister& r31);
bool Hook_FlashCommandLog(PPCRegister& r5);
bool Hook_ListViewPopulate(PPCRegister& r3);

// sub_82218A38 (string table SetLanguage, r4 = index into the .strtbl language
// table). Overrides the index the game derived from the console profile with
// whatever the `language` cvar asks for, so the boot load already comes up in
// the right language.
bool Hook_StringTableLanguage(PPCRegister& r3, PPCRegister& r4);

// Options > Controller key dispatch (sub_8264D0D8). Handles accept/customise
// on the extra "PLAYSTATION BUTTONS" row that Hook_ListViewPopulate appends to
// that screen, so the game never applies a control preset for it.
bool Hook_ControllerMenuKey(PPCRegister& r3, PPCRegister& r4);
bool Hook_MenuKeyDispatch(PPCRegister& r31, PPCRegister& r28, PPCRegister& r27);

// SYSTEM LINK > ONLINE. The SYSTEM LINK window is FlashNavigator drawing the
// "LAN" UIMenu into the pop-up movie. Hook_LanFillElement (0x8222064C, inside
// sub_82220308) makes the ONLINE element a checkbox; Hook_LanAccept
// (0x8263B9F0, inside the UIMenu event handler sub_8263B9A0) takes A on it.
void Hook_LanFillElement(PPCRegister& r27, PPCRegister& r28, PPCRegister& r30);
bool Hook_LanAccept(PPCRegister& r30, PPCRegister& r28);
void Hook_CarbonGarageProbe(PPCRegister& r3);
void Hook_CarbonGarageSelect(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                             PPCRegister& r6);
void Hook_CarbonUiTick(PPCRegister& r3);

// Called from the SCXML state-activate hook (0x8268DDD8) with the name of the
// state being entered. Selecting a garage menu item activates that item's
// state, which is the only click signal the garage screen gives us.
void CarbonOnStateActivate(const char* name, uint32_t state);
